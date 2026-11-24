#include "cache.h"
#include "track.h"
#include <gio/gio.h>

/* Медиатека в кэше называется группой на трек: [track:<uri>]. Так добавление,
 * удаление и переименование записи не требует перебора всего INI-файла. */
#define CACHE_FLUSH_DELAY_MS 1500

static char *get_cache_filepath(void) {
    char *dir = g_build_filename(g_get_user_cache_dir(), "denchiku", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *path = g_build_filename(dir, "library.cache", NULL);
    g_free(dir);
    return path;
}

/* ===== общее in-memory-хранилище ===== */

static GMutex    g_lock;
static GKeyFile *g_kf;
static gboolean  g_loaded;
static gboolean  g_dirty;
static guint     g_flush_id; /* id источника отложенной записи (0 — нет) */

static void schedule_flush_locked(void);

static char *group_for_uri(const char *uri) {
    return g_strdup_printf("track:%s", uri);
}

/* Кэш прежних версий хранил треки как [track_N] с ключом uri. Приводим такие
 * записи к канонической группе [track:<uri>]: иначе одна и та же песня окажется
 * в кэше дважды — старой группой и новой с тем же uri (и в UI задвоится). */
static void normalize_legacy_locked(void) {
    gsize n = 0;
    char **groups = g_key_file_get_groups(g_kf, &n);
    gboolean changed = FALSE;

    for (gsize i = 0; i < n; i++) {
        char *uri = g_key_file_get_string(g_kf, groups[i], "uri", NULL);
        if (!uri || !*uri) {
            g_free(uri);
            continue;
        }
        char *canon = group_for_uri(uri);
        if (g_strcmp0(groups[i], canon) != 0) {
            if (!g_key_file_has_group(g_kf, canon)) {
                gsize nk = 0;
                char **keys = g_key_file_get_keys(g_kf, groups[i], &nk, NULL);
                for (gsize k = 0; k < nk; k++) {
                    char *v = g_key_file_get_string(g_kf, groups[i], keys[k], NULL);
                    g_key_file_set_string(g_kf, canon, keys[k], v ? v : "");
                    g_free(v);
                }
                g_strfreev(keys);
            }
            g_key_file_remove_group(g_kf, groups[i], NULL);
            changed = TRUE;
        }
        g_free(canon);
        g_free(uri);
    }
    g_strfreev(groups);

    if (changed) {
        g_dirty = TRUE;
        schedule_flush_locked();
    }
}

static void ensure_loaded_locked(void) {
    if (g_loaded) return;
    g_loaded = TRUE;
    g_kf = g_key_file_new();
    char *path = get_cache_filepath();
    /* Отсутствие файла — не ошибка: просто начнём с пустого кэша. */
    g_key_file_load_from_file(g_kf, path, G_KEY_FILE_NONE, NULL);
    g_free(path);
    normalize_legacy_locked();
}

static Track *track_from_group_locked(const char *group) {
    char *uri = g_key_file_get_string(g_kf, group, "uri", NULL);
    if (!uri || !*uri) {
        g_free(uri);
        return NULL;
    }

    char *title        = g_key_file_get_string(g_kf, group, "title", NULL);
    if (!title) {
        g_free(uri);
        return NULL;
    }
    char *artist       = g_key_file_get_string(g_kf, group, "artist", NULL);
    char *album        = g_key_file_get_string(g_kf, group, "album", NULL);
    char *album_artist = g_key_file_get_string(g_kf, group, "album_artist", NULL);
    char *genre        = g_key_file_get_string(g_kf, group, "genre", NULL);
    int track_num      = g_key_file_get_integer(g_kf, group, "track_number", NULL);
    int disc_num       = g_key_file_get_integer(g_kf, group, "disc_number", NULL);
    int year_val       = g_key_file_get_integer(g_kf, group, "year", NULL);
    int duration_val   = g_key_file_get_integer(g_kf, group, "duration", NULL);
    char *cover_path   = g_key_file_get_string(g_kf, group, "cover_path", NULL);
    char *disc_subtitle = g_key_file_get_string(g_kf, group, "disc_subtitle", NULL);

    if (cover_path && !*cover_path) {
        g_free(cover_path);
        cover_path = NULL;
    }
    if (disc_subtitle && !*disc_subtitle) {
        g_free(disc_subtitle);
        disc_subtitle = NULL;
    }

    guint tn = track_num > 0 ? (guint)track_num : 0;
    guint dn = disc_num > 0 ? (guint)disc_num : 0;
    guint yr = year_val > 0 ? (guint)year_val : 0;
    guint dur = duration_val > 0 ? (guint)duration_val : 0;

    Track *t = track_new(uri, title, artist, album, album_artist, genre,
                         tn, dn, disc_subtitle, yr, cover_path, dur);
    t->mtime = g_key_file_get_int64(g_kf, group, "mtime", NULL);
    t->size  = g_key_file_get_uint64(g_kf, group, "size", NULL);

    g_free(uri);
    g_free(title);
    g_free(artist);
    g_free(album);
    g_free(album_artist);
    g_free(genre);
    g_free(cover_path);
    g_free(disc_subtitle);
    return t;
}

static GPtrArray *parse_all_locked(void) {
    GPtrArray *tracks = g_ptr_array_new_with_free_func((GDestroyNotify)track_free);
    gsize n = 0;
    char **groups = g_key_file_get_groups(g_kf, &n);
    for (gsize i = 0; i < n; i++) {
        Track *t = track_from_group_locked(groups[i]);
        if (t) g_ptr_array_add(tracks, t);
    }
    g_strfreev(groups);
    return tracks;
}

static void put_track_locked(const Track *t) {
    if (!t || !t->uri || !*t->uri) return;
    char *group = group_for_uri(t->uri);
    g_key_file_set_string(g_kf, group, "uri", t->uri);
    g_key_file_set_string(g_kf, group, "title", t->title ? t->title : "");
    g_key_file_set_string(g_kf, group, "artist", t->artist ? t->artist : "");
    g_key_file_set_string(g_kf, group, "album", t->album ? t->album : "");
    g_key_file_set_string(g_kf, group, "album_artist", t->album_artist ? t->album_artist : "");
    g_key_file_set_string(g_kf, group, "genre", t->genre ? t->genre : "");
    g_key_file_set_integer(g_kf, group, "track_number", (int)t->track_number);
    g_key_file_set_integer(g_kf, group, "disc_number", (int)t->disc_number);
    g_key_file_set_integer(g_kf, group, "year", (int)t->year);
    g_key_file_set_integer(g_kf, group, "duration", (int)t->duration);
    g_key_file_set_string(g_kf, group, "cover_path", t->cover_path ? t->cover_path : "");
    g_key_file_set_string(g_kf, group, "disc_subtitle", t->disc_subtitle ? t->disc_subtitle : "");
    g_key_file_set_int64(g_kf, group, "mtime", t->mtime);
    g_key_file_set_uint64(g_kf, group, "size", t->size);
    g_free(group);
}

/* ===== отложенная атомарная запись ===== */

/* Сериализация и запись идут в фоновом потоке, чтобы не трогать главный цикл. */
static void write_task(GTask *task, gpointer source_object, gpointer task_data,
                       GCancellable *cancellable) {
    (void)source_object;
    (void)task_data;
    (void)cancellable;

    char *data = NULL;
    g_mutex_lock(&g_lock);
    g_dirty = FALSE;
    if (g_kf) data = g_key_file_to_data(g_kf, NULL, NULL);
    g_mutex_unlock(&g_lock);

    if (data) {
        char *path = get_cache_filepath();
        GError *err = NULL;
        if (!g_file_set_contents(path, data, -1, &err)) {
            g_warning("Не удалось сохранить кэш медиатеки: %s", err->message);
            g_error_free(err);
        }
        g_free(path);
        g_free(data);
    }
    g_task_return_boolean(task, TRUE);
}

static void cache_dispatch_write(void) {
    GTask *task = g_task_new(NULL, NULL, NULL, NULL);
    g_task_run_in_thread(task, write_task);
    g_object_unref(task);
}

static gboolean cache_flush_timeout(gpointer data) {
    (void)data;
    g_mutex_lock(&g_lock);
    g_flush_id = 0;
    gboolean dirty = g_dirty;
    g_mutex_unlock(&g_lock);
    if (dirty) cache_dispatch_write();
    return G_SOURCE_REMOVE;
}

/* Вызывается с удержанным g_lock. Ставит отложенную запись, если её ещё нет. */
static void schedule_flush_locked(void) {
    g_dirty = TRUE;
    if (g_flush_id != 0) return;
    GSource *src = g_timeout_source_new(CACHE_FLUSH_DELAY_MS);
    g_source_set_callback(src, cache_flush_timeout, NULL, NULL);
    g_flush_id = g_source_attach(src, g_main_context_default());
    g_source_unref(src);
}

/* ===== публичный интерфейс ===== */

GPtrArray *cache_load_tracks(void) {
    g_mutex_lock(&g_lock);
    ensure_loaded_locked();
    GPtrArray *tracks = parse_all_locked();
    g_mutex_unlock(&g_lock);
    if (tracks->len == 0) {
        g_ptr_array_unref(tracks);
        return NULL;
    }
    return tracks;
}

GPtrArray *cache_snapshot_tracks(void) {
    g_mutex_lock(&g_lock);
    ensure_loaded_locked();
    GPtrArray *tracks = parse_all_locked();
    g_mutex_unlock(&g_lock);
    return tracks;
}

void cache_save_tracks(GPtrArray *tracks) {
    g_mutex_lock(&g_lock);
    ensure_loaded_locked();
    g_key_file_free(g_kf);
    g_kf = g_key_file_new();
    if (tracks) {
        for (guint i = 0; i < tracks->len; i++)
            put_track_locked(g_ptr_array_index(tracks, i));
    }
    schedule_flush_locked();
    g_mutex_unlock(&g_lock);
}

void cache_put_track(const Track *track) {
    g_mutex_lock(&g_lock);
    ensure_loaded_locked();
    put_track_locked(track);
    schedule_flush_locked();
    g_mutex_unlock(&g_lock);
}

void cache_remove_uri(const char *uri) {
    if (!uri) return;
    g_mutex_lock(&g_lock);
    ensure_loaded_locked();
    char *group = group_for_uri(uri);
    if (g_key_file_remove_group(g_kf, group, NULL)) schedule_flush_locked();
    g_free(group);
    g_mutex_unlock(&g_lock);
}

gboolean cache_rename_uri(const char *old_uri, const char *new_uri,
                          gint64 mtime, guint64 size) {
    if (!old_uri || !new_uri) return FALSE;
    g_mutex_lock(&g_lock);
    ensure_loaded_locked();

    char *old_group = group_for_uri(old_uri);
    char *new_group = group_for_uri(new_uri);
    gboolean ok = FALSE;

    if (g_key_file_has_group(g_kf, old_group)) {
        gsize nk = 0;
        char **keys = g_key_file_get_keys(g_kf, old_group, &nk, NULL);
        for (gsize i = 0; i < nk; i++) {
            char *v = g_key_file_get_string(g_kf, old_group, keys[i], NULL);
            g_key_file_set_string(g_kf, new_group, keys[i], v ? v : "");
            g_free(v);
        }
        g_strfreev(keys);
        g_key_file_set_string(g_kf, new_group, "uri", new_uri);
        g_key_file_set_int64(g_kf, new_group, "mtime", mtime);
        g_key_file_set_uint64(g_kf, new_group, "size", size);
        g_key_file_remove_group(g_kf, old_group, NULL);
        schedule_flush_locked();
        ok = TRUE;
    }

    g_free(old_group);
    g_free(new_group);
    g_mutex_unlock(&g_lock);
    return ok;
}

gboolean cache_lookup_meta(const char *uri, gint64 *mtime, guint64 *size) {
    if (!uri) return FALSE;
    g_mutex_lock(&g_lock);
    ensure_loaded_locked();
    char *group = group_for_uri(uri);
    gboolean ok = g_key_file_has_group(g_kf, group);
    if (ok) {
        if (mtime) *mtime = g_key_file_get_int64(g_kf, group, "mtime", NULL);
        if (size)  *size  = g_key_file_get_uint64(g_kf, group, "size", NULL);
    }
    g_free(group);
    g_mutex_unlock(&g_lock);
    return ok;
}

Track *cache_lookup_track(const char *uri) {
    if (!uri) return NULL;
    g_mutex_lock(&g_lock);
    ensure_loaded_locked();
    char *group = group_for_uri(uri);
    Track *t = g_key_file_has_group(g_kf, group) ? track_from_group_locked(group) : NULL;
    g_free(group);
    g_mutex_unlock(&g_lock);
    return t;
}

void cache_prune_missing(GHashTable *present_uris) {
    g_mutex_lock(&g_lock);
    ensure_loaded_locked();
    gsize n = 0;
    char **groups = g_key_file_get_groups(g_kf, &n);
    gboolean changed = FALSE;
    for (gsize i = 0; i < n; i++) {
        char *uri = g_key_file_get_string(g_kf, groups[i], "uri", NULL);
        /* Записи без uri, а также неканонические группы (наследие прежнего
         * формата) удаляем, чтобы не задваивать треки. */
        char *canon = uri ? group_for_uri(uri) : NULL;
        gboolean canonical = canon && g_strcmp0(groups[i], canon) == 0;
        if (!uri || !canonical ||
            !present_uris || !g_hash_table_contains(present_uris, uri)) {
            g_key_file_remove_group(g_kf, groups[i], NULL);
            changed = TRUE;
        }
        g_free(canon);
        g_free(uri);
    }
    g_strfreev(groups);
    if (changed) schedule_flush_locked();
    g_mutex_unlock(&g_lock);
}

void cache_schedule_flush(void) {
    g_mutex_lock(&g_lock);
    ensure_loaded_locked();
    schedule_flush_locked();
    g_mutex_unlock(&g_lock);
}

void cache_flush(void) {
    char *data = NULL;
    g_mutex_lock(&g_lock);
    ensure_loaded_locked();
    if (g_flush_id != 0) {
        g_source_remove(g_flush_id);
        g_flush_id = 0;
    }
    g_dirty = FALSE;
    if (g_kf) data = g_key_file_to_data(g_kf, NULL, NULL);
    g_mutex_unlock(&g_lock);

    if (data) {
        char *path = get_cache_filepath();
        GError *err = NULL;
        if (!g_file_set_contents(path, data, -1, &err)) {
            g_warning("Не удалось сохранить кэш медиатеки: %s", err->message);
            g_error_free(err);
        }
        g_free(path);
        g_free(data);
    }
}
