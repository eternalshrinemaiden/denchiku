#include "cache.h"
#include "track.h"
#include <gio/gio.h>

/* Медиатека в памяти — таблица uri -> Track*. Раньше весь список держался ещё
 * и в GKeyFile постоянно, что дублировало метаданные в тяжёлом формате GLib;
 * теперь GKeyFile собирается только транзиентно при загрузке и записи. */
#define CACHE_FLUSH_DELAY_MS 1500

static char *get_cache_filepath(void) {
    char *dir = g_build_filename(g_get_user_cache_dir(), "denchiku", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *path = g_build_filename(dir, "library.cache", NULL);
    g_free(dir);
    return path;
}

/* ===== общее in-memory-хранилище ===== */

static GMutex      g_lock;
static GHashTable *g_tracks;   /* char* uri -> Track* (кэш владеет ссылкой) */

/* Мемоизация INI-текста группы на трек, чтобы при flush пересобирать текст
 * только у изменившихся треков, а не у всей медиатеки. Актуальность — по
 * указателю на Track: при изменении в кэш кладётся новый объект, поэтому
 * отдельная инвалидация не нужна, достаточно чистить записи удалённых uri. */
typedef struct {
    const Track *track;
    char *text;
} GroupMemo;

static GHashTable *g_group_memo; /* char* uri -> GroupMemo* */

static gboolean    g_loaded;
static gboolean    g_dirty;
static guint       g_flush_id; /* id источника отложенной записи (0 — нет) */

static void schedule_flush_locked(void);

static void group_memo_free(gpointer data) {
    GroupMemo *m = data;
    g_free(m->text);
    g_free(m);
}

/* ===== сериализация =====
 * На диск пишем INI-текст напрямую из таблицы (без промежуточного GKeyFile):
 * формат совместим с прежним, а лишняя полная копия структуры на каждый flush
 * не создаётся. Чтение всё равно идёт через GKeyFile — он устойчиво разбирает
 * экранирование. Значения экранируем совместимо с g_key_file_to_data(): \\,
 * \n, \r, \t, а ведущие пробелы — \s (см. ini_append_escaped). */
static void ini_append_escaped(GString *out, const char *s) {
    /* Ведущие пробелы/табы парсер GKeyFile срезает, поэтому их экономно
     * экранируем (\s, \t), чтобы значение сохранилось; пробелы в остальной
     * части остаются как есть. \n, \r, \t и \\ экранируются всегда. */
    gboolean leading = TRUE;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (leading && (c == ' ' || c == '\t')) {
            g_string_append(out, c == ' ' ? "\\s" : "\\t");
            continue;
        }
        leading = FALSE;
        switch (c) {
            case '\\': g_string_append(out, "\\\\"); break;
            case '\n': g_string_append(out, "\\n"); break;
            case '\r': g_string_append(out, "\\r"); break;
            case '\t': g_string_append(out, "\\t"); break;
            default:   g_string_append_c(out, c);
        }
    }
}

static void ini_append_str(GString *out, const char *key, const char *value) {
    g_string_append(out, key);
    g_string_append_c(out, '=');
    ini_append_escaped(out, value ? value : "");
    g_string_append_c(out, '\n');
}

/* Track -> группа [track:<uri>]. Группа канонична по uri: иначе одна и та же
 * песня со старых форматов (например [track_N] с ключом uri) могла бы попасть в
 * кэш дважды. */
static void track_append_ini(GString *out, const Track *t) {
    g_string_append(out, "[track:");
    ini_append_escaped(out, t->uri);
    g_string_append(out, "]\n");

    ini_append_str(out, "uri", t->uri);
    ini_append_str(out, "title", t->title);
    ini_append_str(out, "artist", t->artist);
    ini_append_str(out, "album", t->album);
    ini_append_str(out, "album_artist", t->album_artist);
    ini_append_str(out, "genre", t->genre);
    g_string_append_printf(out, "track_number=%u\n", t->track_number);
    g_string_append_printf(out, "disc_number=%u\n", t->disc_number);
    g_string_append_printf(out, "year=%u\n", t->year);
    g_string_append_printf(out, "duration=%u\n", t->duration);
    ini_append_str(out, "cover_path", t->cover_path);
    ini_append_str(out, "disc_subtitle", t->disc_subtitle);
    g_string_append_printf(out, "mtime=%" G_GINT64_FORMAT "\n", t->mtime);
    g_string_append_printf(out, "size=%" G_GUINT64_FORMAT "\n", t->size);
    g_string_append_c(out, '\n');
}

/* Группа GKeyFile -> Track (счётчик ссылок = 1). NULL, если группы нет смысла
 * загружать (нет uri или title). */
static Track *track_from_group(GKeyFile *kf, const char *group) {
    char *uri = g_key_file_get_string(kf, group, "uri", NULL);
    if (!uri || !*uri) {
        g_free(uri);
        return NULL;
    }

    char *title        = g_key_file_get_string(kf, group, "title", NULL);
    if (!title) {
        g_free(uri);
        return NULL;
    }
    char *artist       = g_key_file_get_string(kf, group, "artist", NULL);
    char *album        = g_key_file_get_string(kf, group, "album", NULL);
    char *album_artist = g_key_file_get_string(kf, group, "album_artist", NULL);
    char *genre        = g_key_file_get_string(kf, group, "genre", NULL);
    int track_num      = g_key_file_get_integer(kf, group, "track_number", NULL);
    int disc_num       = g_key_file_get_integer(kf, group, "disc_number", NULL);
    int year_val       = g_key_file_get_integer(kf, group, "year", NULL);
    int duration_val   = g_key_file_get_integer(kf, group, "duration", NULL);
    char *cover_path   = g_key_file_get_string(kf, group, "cover_path", NULL);
    char *disc_subtitle = g_key_file_get_string(kf, group, "disc_subtitle", NULL);

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
    t->mtime = g_key_file_get_int64(kf, group, "mtime", NULL);
    t->size  = g_key_file_get_uint64(kf, group, "size", NULL);

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

/* Загружает файл кэша в таблицу. GKeyFile освобождается сразу после разбора —
 * в памяти остаются только сами треки. */
static void load_file_locked(void) {
    GKeyFile *kf = g_key_file_new();
    char *path = get_cache_filepath();
    /* Отсутствие файла — не ошибка: просто начнём с пустого кэша. */
    gboolean ok = g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    g_free(path);
    if (!ok) {
        g_key_file_free(kf);
        return;
    }

    gboolean legacy = FALSE;
    gsize n = 0;
    char **groups = g_key_file_get_groups(kf, &n);
    for (gsize i = 0; i < n; i++) {
        Track *t = track_from_group(kf, groups[i]);
        if (!t) continue;

        char *canon = g_strdup_printf("track:%s", t->uri);
        if (g_strcmp0(groups[i], canon) != 0) legacy = TRUE;
        g_free(canon);

        g_hash_table_replace(g_tracks, g_strdup(t->uri), t); /* владение t */
    }
    g_strfreev(groups);
    g_key_file_free(kf);

    /* Прежние версии хранили записи как [track_N] с ключом uri: переписываем
     * файл в канонический вид один раз, чтобы формат не расходился. */
    if (legacy) schedule_flush_locked();
}

static void ensure_loaded_locked(void) {
    if (g_loaded) return;
    g_loaded = TRUE;
    g_tracks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                     (GDestroyNotify)track_unref);
    g_group_memo = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         group_memo_free);
    load_file_locked();
}

/* ===== отложенная атомарная запись ===== */

/* Готовый INI-текст всей медиатеки. Вызывается с удержанным g_lock;
 * освобождать результат через g_free. */
static char *serialize_locked_data(void) {
    GString *out = g_string_new(NULL);
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, g_tracks);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        const char *uri = key;
        const Track *t = value;

        GroupMemo *m = g_hash_table_lookup(g_group_memo, uri);
        if (!m) {
            m = g_new0(GroupMemo, 1);
            g_hash_table_insert(g_group_memo, g_strdup(uri), m);
        }
        /* Тексты неизменившихся треков переиспользуем; пересобираем только те,
         * у которых сменился объект (значит, поменялись метаданные/паспорт). */
        if (m->track != t) {
            g_free(m->text);
            GString *group = g_string_new(NULL);
            track_append_ini(group, t);
            m->text = g_string_free(group, FALSE);
            m->track = t;
        }
        g_string_append(out, m->text);
    }
    return g_string_free(out, FALSE);
}

/* Сериализация и запись идут в фоновом потоке, чтобы не трогать главный цикл. */
static void write_task(GTask *task, gpointer source_object, gpointer task_data,
                       GCancellable *cancellable) {
    (void)source_object;
    (void)task_data;
    (void)cancellable;

    char *data = NULL;
    g_mutex_lock(&g_lock);
    g_dirty = FALSE;
    if (g_tracks) data = serialize_locked_data();
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

/* ===== операции над таблицей (с удержанным g_lock) ===== */

static void put_track_locked(const Track *t) {
    if (!t || !t->uri || !*t->uri) return;
    /* Тот же объект уже канонический (типовой случай: скан вернул неизменённый
     * трек из кэша) — ничего не меняем и не пишем на диск. */
    if (g_hash_table_lookup(g_tracks, t->uri) == t) return;
    g_hash_table_replace(g_tracks, g_strdup(t->uri), track_ref((Track *)t));
    schedule_flush_locked();
}

/* Снимок: массив НОВЫХ ссылок на те же треки. */
static GPtrArray *snapshot_locked(void) {
    GPtrArray *tracks = g_ptr_array_new_with_free_func((GDestroyNotify)track_unref);
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, g_tracks);
    while (g_hash_table_iter_next(&it, &key, &value))
        g_ptr_array_add(tracks, track_ref((Track *)value));
    return tracks;
}

/* ===== публичный интерфейс ===== */

GPtrArray *cache_load_tracks(void) {
    g_mutex_lock(&g_lock);
    ensure_loaded_locked();
    GPtrArray *tracks = snapshot_locked();
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
    GPtrArray *tracks = snapshot_locked();
    g_mutex_unlock(&g_lock);
    return tracks;
}

void cache_save_tracks(GPtrArray *tracks) {
    g_mutex_lock(&g_lock);
    ensure_loaded_locked();
    g_hash_table_remove_all(g_tracks);
    g_hash_table_remove_all(g_group_memo);
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
    g_mutex_unlock(&g_lock);
}

void cache_remove_uri(const char *uri) {
    if (!uri) return;
    g_mutex_lock(&g_lock);
    ensure_loaded_locked();
    g_hash_table_remove(g_group_memo, uri);
    if (g_hash_table_remove(g_tracks, uri)) schedule_flush_locked();
    g_mutex_unlock(&g_lock);
}

gboolean cache_rename_uri(const char *old_uri, const char *new_uri,
                          gint64 mtime, guint64 size) {
    if (!old_uri || !new_uri) return FALSE;
    g_mutex_lock(&g_lock);
    ensure_loaded_locked();

    Track *old = g_hash_table_lookup(g_tracks, old_uri);
    gboolean ok = FALSE;
    if (old) {
        /* URI трека меняется, поэтому старый объект (он может ещё жить в
         * списках UI) не трогаем — кладём в кэш новый объект с новым uri. */
        Track *moved = track_copy(old);
        g_free(moved->uri);
        moved->uri = g_strdup(new_uri);
        moved->mtime = mtime;
        moved->size = size;
        g_hash_table_remove(g_tracks, old_uri);
        g_hash_table_remove(g_group_memo, old_uri);
        g_hash_table_replace(g_tracks, g_strdup(new_uri), moved);
        schedule_flush_locked();
        ok = TRUE;
    }

    g_mutex_unlock(&g_lock);
    return ok;
}

gboolean cache_lookup_meta(const char *uri, gint64 *mtime, guint64 *size) {
    if (!uri) return FALSE;
    g_mutex_lock(&g_lock);
    ensure_loaded_locked();
    Track *t = g_hash_table_lookup(g_tracks, uri);
    gboolean ok = t != NULL;
    if (ok) {
        if (mtime) *mtime = t->mtime;
        if (size)  *size  = t->size;
    }
    g_mutex_unlock(&g_lock);
    return ok;
}

Track *cache_lookup_track(const char *uri) {
    if (!uri) return NULL;
    g_mutex_lock(&g_lock);
    ensure_loaded_locked();
    Track *t = g_hash_table_lookup(g_tracks, uri);
    if (t) track_ref(t); /* отдаём новую ссылку */
    g_mutex_unlock(&g_lock);
    return t;
}

void cache_prune_missing(GHashTable *present_uris) {
    g_mutex_lock(&g_lock);
    ensure_loaded_locked();
    gboolean changed = FALSE;
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, g_tracks);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        if (!present_uris || !g_hash_table_contains(present_uris, key)) {
            g_hash_table_remove(g_group_memo, key);
            g_hash_table_iter_remove(&it);
            changed = TRUE;
        }
    }
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
    if (g_tracks) data = serialize_locked_data();
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
