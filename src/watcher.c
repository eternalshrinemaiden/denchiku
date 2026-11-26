#include "watcher.h"

#include "cache.h"
#include "scanner.h"
#include "track.h"

#include <gst/gst.h>
#include <string.h>

/* Окно дебаунса: события копятся, а разбор идёт одним батчем. */
#define PARSE_DEBOUNCE_MS 200
#define WATCH_MAX_THREADS 4

typedef enum {
    WATCH_TASK_FILE, /* arg — file:// URI */
    WATCH_TASK_DIR,  /* arg — путь каталога */
} WatchTaskType;

typedef struct {
    WatchTaskType type;
    char *arg;
} WatchTask;

struct _Watcher {
    GHashTable *monitors;      /* char* путь -> GFileMonitor* */
    GMutex      monitors_lock;
    GHashTable *pending;       /* char* URI -> NULL (множество на разбор) */
    GMutex      pending_lock;
    GThreadPool *pool;
    char *root;
    WatcherChangedFunc changed_cb;
    gpointer changed_ud;
    gint parse_scheduled;      /* atomic: источник дебаунса уже поставлен */
    gint deliver_queued;       /* atomic: доставка в UI уже поставлена */
    gboolean started;
};

static void push_dir_task(Watcher *w, const char *path);
static void process_file(Watcher *w, const char *uri);
static void process_dir(Watcher *w, const char *path);
static void enqueue_file(Watcher *w, const char *uri);

/* ===== вспомогательное ===== */

static gboolean is_temp_name(const char *name) {
    static const char *suffixes[] = { ".tmp", ".part", ".crdownload", NULL };
    char *lower = g_ascii_strdown(name, -1);
    gboolean result = FALSE;
    for (int i = 0; suffixes[i] != NULL; i++) {
        if (g_str_has_suffix(lower, suffixes[i])) {
            result = TRUE;
            break;
        }
    }
    g_free(lower);
    return result;
}

/* Путь похож на аудиофайл и не является скрытым/временным. */
static gboolean audio_path_ok(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!base[0] || base[0] == '.') return FALSE;
    if (is_temp_name(base)) return FALSE;
    return scanner_is_audio_file(base);
}

static gboolean query_meta(const char *path, gint64 *mtime, guint64 *size) {
    GFile *file = g_file_new_for_path(path);
    GFileInfo *info = g_file_query_info(file,
        G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
        G_FILE_QUERY_INFO_NONE, NULL, NULL);
    g_object_unref(file);
    if (!info) return FALSE;
    if (mtime) *mtime = (gint64)g_file_info_get_attribute_uint64(
        info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
    if (size) *size = g_file_info_get_size(info);
    g_object_unref(info);
    return TRUE;
}

/* GstDiscoverer на поток: переиспользуем экземпляр вместо создания на файл. */
static GPrivate g_disc_key = G_PRIVATE_INIT((GDestroyNotify)gst_object_unref);

static GstDiscoverer *worker_discoverer(void) {
    GstDiscoverer *d = g_private_get(&g_disc_key);
    if (!d) {
        d = gst_discoverer_new(2 * GST_SECOND, NULL);
        g_private_set(&g_disc_key, d);
    }
    return d;
}

/* ===== доставка в UI (главный поток) ===== */

static gboolean deliver_idle_cb(gpointer data) {
    Watcher *w = data;
    g_atomic_int_set(&w->deliver_queued, 0);
    if (w->changed_cb) {
        GPtrArray *snap = cache_snapshot_tracks();
        w->changed_cb(snap, w->changed_ud);
        g_ptr_array_unref(snap);
    }
    return G_SOURCE_REMOVE;
}

/* Единый батч-сигнал: сколько бы файлов ни изменилось, UI обновится один раз. */
static void watcher_queue_deliver(Watcher *w) {
    if (g_atomic_int_compare_and_exchange(&w->deliver_queued, 0, 1))
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, deliver_idle_cb, w, NULL);
}

/* ===== дебаунс файловых событий ===== */

static gboolean parse_timeout_cb(gpointer data) {
    Watcher *w = data;
    g_atomic_int_set(&w->parse_scheduled, 0);

    GPtrArray *uris = g_ptr_array_new_with_free_func(g_free);
    g_mutex_lock(&w->pending_lock);
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, w->pending);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        g_ptr_array_add(uris, g_strdup(key));
        g_hash_table_iter_remove(&it);
    }
    g_mutex_unlock(&w->pending_lock);

    for (guint i = 0; i < uris->len; i++) {
        WatchTask *task = g_new0(WatchTask, 1);
        task->type = WATCH_TASK_FILE;
        task->arg = g_strdup(g_ptr_array_index(uris, i));
        g_thread_pool_push(w->pool, task, NULL);
    }
    g_ptr_array_unref(uris);
    return G_SOURCE_REMOVE;
}

static void schedule_parse(Watcher *w) {
    if (!g_atomic_int_compare_and_exchange(&w->parse_scheduled, 0, 1)) return;
    GSource *src = g_timeout_source_new(PARSE_DEBOUNCE_MS);
    g_source_set_callback(src, parse_timeout_cb, w, NULL);
    g_source_attach(src, g_main_context_default());
    g_source_unref(src);
}

static void enqueue_file(Watcher *w, const char *uri) {
    g_mutex_lock(&w->pending_lock);
    gboolean added = !g_hash_table_contains(w->pending, uri);
    if (added) g_hash_table_add(w->pending, g_strdup(uri));
    g_mutex_unlock(&w->pending_lock);
    if (added) schedule_parse(w);
}

static void remove_uri(Watcher *w, const char *uri) {
    cache_remove_uri(uri);
    watcher_queue_deliver(w);
}

/* ===== мониторы каталогов ===== */

static void on_monitor_changed(GFileMonitor *monitor, GFile *file, GFile *other_file,
                               GFileMonitorEvent event, gpointer user_data);

static void add_monitor(Watcher *w, const char *path) {
    g_mutex_lock(&w->monitors_lock);
    if (g_hash_table_contains(w->monitors, path)) {
        g_mutex_unlock(&w->monitors_lock);
        return;
    }

    GFile *file = g_file_new_for_path(path);
    GError *err = NULL;
    GFileMonitor *monitor = g_file_monitor_directory(file, G_FILE_MONITOR_WATCH_MOVES,
                                                     NULL, &err);
    g_object_unref(file);
    if (!monitor) {
        if (err) {
            if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NO_SPACE)) {
                g_warning("Не удалось следить за «%s»: исчерпан лимит inotify. "
                          "Увеличьте fs.inotify.max_user_watches.", path);
            } else {
                g_warning("Не удалось следить за «%s»: %s", path, err->message);
            }
            g_error_free(err);
        }
        g_mutex_unlock(&w->monitors_lock);
        return;
    }

    g_signal_connect(monitor, "changed", G_CALLBACK(on_monitor_changed), w);
    g_hash_table_insert(w->monitors, g_strdup(path), monitor); /* владение монитором */
    g_mutex_unlock(&w->monitors_lock);
}

static void remove_monitors_under(Watcher *w, const char *path) {
    g_mutex_lock(&w->monitors_lock);
    GPtrArray *keys = g_ptr_array_new_with_free_func(g_free);

    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, w->monitors);
    gsize plen = strlen(path);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        const char *k = key;
        if (g_strcmp0(k, path) == 0 ||
            (g_str_has_prefix(k, path) && k[plen] == '/')) {
            g_ptr_array_add(keys, g_strdup(k));
        }
    }
    for (guint i = 0; i < keys->len; i++) {
        const char *k = g_ptr_array_index(keys, i);
        GFileMonitor *m = g_hash_table_lookup(w->monitors, k);
        if (m) g_file_monitor_cancel(m);
        g_hash_table_remove(w->monitors, k); /* разыменовывает монитор */
    }
    g_ptr_array_unref(keys);
    g_mutex_unlock(&w->monitors_lock);
}

/* ===== обработка задач пула ===== */

static void process_file(Watcher *w, const char *uri) {
    GFile *file = g_file_new_for_uri(uri);
    GFileInfo *info = g_file_query_info(file,
        G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
        G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (!info) {
        g_object_unref(file);
        remove_uri(w, uri); /* файл исчез */
        return;
    }
    gint64 mtime = (gint64)g_file_info_get_attribute_uint64(
        info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
    guint64 size = g_file_info_get_size(info);
    g_object_unref(info);
    char *path = g_file_get_path(file);
    g_object_unref(file);
    if (!path) return;
    if (size == 0) {
        g_free(path); /* файл ещё дописывается — дожмём позже */
        return;
    }

    gint64 cached_mtime = 0;
    guint64 cached_size = 0;
    if (cache_lookup_meta(uri, &cached_mtime, &cached_size) &&
        cached_mtime == mtime && cached_size == size) {
        g_free(path); /* паспорт совпал — теги перечитывать не нужно */
        return;
    }

    GstDiscoverer *discoverer = worker_discoverer();
    Track *track = discoverer ? scanner_discover_file(discoverer, path) : NULL;
    g_free(path);
    if (track) {
        cache_put_track(track);
        track_unref(track);
        watcher_queue_deliver(w);
    }
}

static void process_dir(Watcher *w, const char *path) {
    add_monitor(w, path); /* сама папка тоже под наблюдением */

    GDir *dir = g_dir_open(path, 0, NULL);
    if (!dir) return;

    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (name[0] == '.') continue;
        char *full = g_build_filename(path, name, NULL);
        if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
            process_dir(w, full);
        } else if (scanner_is_audio_file(name)) {
            char *uri = g_filename_to_uri(full, NULL, NULL);
            if (uri) {
                enqueue_file(w, uri);
                g_free(uri);
            }
        }
        g_free(full);
    }
    g_dir_close(dir);
}

static void watch_task(gpointer data, gpointer user_data) {
    WatchTask *task = data;
    Watcher *w = user_data;
    if (task->type == WATCH_TASK_FILE)
        process_file(w, task->arg);
    else
        process_dir(w, task->arg);
    g_free(task->arg);
    g_free(task);
}

static void push_dir_task(Watcher *w, const char *path) {
    WatchTask *task = g_new0(WatchTask, 1);
    task->type = WATCH_TASK_DIR;
    task->arg = g_strdup(path);
    g_thread_pool_push(w->pool, task, NULL);
}

static void maybe_enqueue_path(Watcher *w, const char *path) {
    if (!audio_path_ok(path)) return;
    char *uri = g_filename_to_uri(path, NULL, NULL);
    if (uri) {
        enqueue_file(w, uri);
        g_free(uri);
    }
}

static void handle_rename(Watcher *w, const char *old_path, GFile *new_file) {
    char *new_path = new_file ? g_file_get_path(new_file) : NULL;
    if (!new_path) return;

    char *old_uri = g_filename_to_uri(old_path, NULL, NULL);
    char *new_uri = g_filename_to_uri(new_path, NULL, NULL);
    if (!old_uri || !new_uri) {
        g_free(old_uri);
        g_free(new_uri);
        g_free(new_path);
        return;
    }

    if (!audio_path_ok(new_path)) {
        remove_uri(w, old_uri); /* переименовали в не-аудио/скрытое */
    } else {
        gint64 mtime = 0;
        guint64 size = 0;
        query_meta(new_path, &mtime, &size);
        /* Переносим запись в кэше по новому URI без перечитывания тегов. */
        if (cache_rename_uri(old_uri, new_uri, mtime, size))
            watcher_queue_deliver(w);
        else
            enqueue_file(w, new_uri); /* записи не было — разберём как новый файл */
    }

    g_free(old_uri);
    g_free(new_uri);
    g_free(new_path);
}

static void on_monitor_changed(GFileMonitor *monitor, GFile *file, GFile *other_file,
                               GFileMonitorEvent event, gpointer user_data) {
    (void)monitor;
    Watcher *w = user_data;

    char *path = g_file_get_path(file);
    if (!path) return;

    switch (event) {
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_MOVED_IN: {
        GFileType type = g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL);
        if (type == G_FILE_TYPE_DIRECTORY) {
            /* Файлы могли скопироваться до регистрации watch — сканируем сразу. */
            push_dir_task(w, path);
        } else {
            maybe_enqueue_path(w, path);
        }
        break;
    }
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT: {
        GFileType type = g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL);
        if (type != G_FILE_TYPE_DIRECTORY) maybe_enqueue_path(w, path);
        break;
    }
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_MOVED_OUT: {
        GFileType type = g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL);
        remove_monitors_under(w, path);
        if (type != G_FILE_TYPE_DIRECTORY) {
            char *uri = g_filename_to_uri(path, NULL, NULL);
            if (uri) {
                remove_uri(w, uri);
                g_free(uri);
            }
        }
        break;
    }
    case G_FILE_MONITOR_EVENT_RENAMED:
        handle_rename(w, path, other_file);
        break;
    default:
        break;
    }

    g_free(path);
}

/* ===== публичный интерфейс ===== */

Watcher *watcher_new(void) {
    Watcher *w = g_new0(Watcher, 1);
    w->monitors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    w->pending  = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_mutex_init(&w->monitors_lock);
    g_mutex_init(&w->pending_lock);

    guint n = g_get_num_processors();
    if (n < 2) n = 2;
    if (n > WATCH_MAX_THREADS) n = WATCH_MAX_THREADS;
    w->pool = g_thread_pool_new(watch_task, w, (gint)n, FALSE, NULL);
    return w;
}

void watcher_set_changed_cb(Watcher *w, WatcherChangedFunc cb, gpointer user_data) {
    if (!w) return;
    w->changed_cb = cb;
    w->changed_ud = user_data;
}

void watcher_start(Watcher *w, const char *root) {
    if (!w || w->started || !root) return;
    w->started = TRUE;
    w->root = g_strdup(root);
    push_dir_task(w, root); /* начальный рекурсивный обход — в фоне */
}

void watcher_free(Watcher *w) {
    if (!w) return;
    if (w->pool) {
        g_thread_pool_free(w->pool, TRUE, TRUE);
        w->pool = NULL;
    }

    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, w->monitors);
    while (g_hash_table_iter_next(&it, &key, &value))
        g_file_monitor_cancel(G_FILE_MONITOR(value));

    g_hash_table_destroy(w->monitors);
    g_hash_table_destroy(w->pending);
    g_mutex_clear(&w->monitors_lock);
    g_mutex_clear(&w->pending_lock);
    g_free(w->root);
    g_free(w);
}
