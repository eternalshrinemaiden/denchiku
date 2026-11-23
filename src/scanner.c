#include "scanner.h"
#include "cache.h"
#include "track.h"
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>

static gboolean is_audio_file(const char *filename) {
    static const char *extensions[] = {
        ".mp3", ".flac", ".ogg", ".opus", ".oga",
        ".m4a", ".m4b", ".aac", ".wav", ".aiff",
        ".aif", ".wma", ".ape", ".wv", ".mka",
        NULL
    };

    char *lower = g_ascii_strdown(filename, -1);
    gboolean result = FALSE;
    for (int i = 0; extensions[i] != NULL; i++) {
        if (g_str_has_suffix(lower, extensions[i])) {
            result = TRUE;
            break;
        }
    }
    g_free(lower);
    return result;
}

/* Извлечение встроенной обложки из GstTagList и сохранение в кэш по имени альбома */
static char *extract_and_cache_cover(const GstTagList *tags, const char *album_artist, const char *album) {
    if (!tags) return NULL;

    char *key = g_strdup_printf("%s - %s", album_artist ? album_artist : "", album ? album : "");
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, key, -1);
    g_free(key);

    char *dir = g_build_filename(g_get_user_cache_dir(), "keruu", "covers", NULL);
    g_mkdir_with_parents(dir, 0755);

    char *filepath = g_strdup_printf("%s/%s.img", dir, hash);
    g_free(dir);
    g_free(hash);

    /* Если обложка для этого альбома уже извлечена — сразу возвращаем путь */
    if (g_file_test(filepath, G_FILE_TEST_EXISTS)) {
        return filepath;
    }

    GstSample *sample = NULL;
    if (!gst_tag_list_get_sample(tags, GST_TAG_IMAGE, &sample)) {
        gst_tag_list_get_sample(tags, GST_TAG_PREVIEW_IMAGE, &sample);
    }

    if (sample) {
        GstBuffer *buf = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
            g_file_set_contents(filepath, (const char *)map.data, map.size, NULL);
            gst_buffer_unmap(buf, &map);
            gst_sample_unref(sample);
            return filepath;
        }
        gst_sample_unref(sample);
    }

    g_free(filepath);
    return NULL;
}

/* Обложки разных треков одного альбома дают один и тот же путь, поэтому запись
 * одного и того же файла из нескольких потоков нужно сериализовать. */
static GMutex g_cover_mutex;

static char *extract_and_cache_cover_locked(const GstTagList *tags,
                                            const char *album_artist, const char *album) {
    g_mutex_lock(&g_cover_mutex);
    char *result = extract_and_cache_cover(tags, album_artist, album);
    g_mutex_unlock(&g_cover_mutex);
    return result;
}

/* Извлекает числовой префикс из значения вида "2" или "2/10". */
static guint parse_uint_prefix(const char *value) {
    if (!value) return 0;
    while (*value && !g_ascii_isdigit(*value)) value++;
    guint n = 0;
    while (g_ascii_isdigit(*value)) {
        n = n * 10 + (guint)(*value - '0');
        value++;
    }
    return n;
}

/* Возвращает значение текстового поля (например DISCNUMBER или DISCSUBTITLE)
 * среди Vorbis-комментариев блока (little-endian длины) либо NULL. */
static char *vorbis_field_value(const guchar *data, gsize len, const char *field) {
    if (len < 4) return NULL;

    guint32 vendor_len = data[0] | (data[1] << 8) | (data[2] << 16) | ((guint32)data[3] << 24);
    /* Считаем в 64 битах: vendor_len приходит из файла и иначе может
     * переполнить 4 + vendor_len и обойти проверку границ. */
    if ((guint64)vendor_len + 4 > len) return NULL;
    gsize pos = 4 + (gsize)vendor_len;
    if (pos + 4 > len) return NULL;

    guint32 count = data[pos] | (data[pos+1] << 8) | (data[pos+2] << 16) | ((guint32)data[pos+3] << 24);
    pos += 4;

    gsize flen = strlen(field);
    for (guint32 i = 0; i < count; i++) {
        if (pos + 4 > len) break;
        guint32 clen = data[pos] | (data[pos+1] << 8) | (data[pos+2] << 16) | ((guint32)data[pos+3] << 24);
        pos += 4;
        if (pos + clen > len) break;

        const gchar *comment = (const gchar *)data + pos;
        if (clen > flen + 1 &&
            g_ascii_strncasecmp(comment, field, flen) == 0 &&
            comment[flen] == '=') {
            return g_strndup(comment + flen + 1, clen - flen - 1);
        }
        pos += clen;
    }
    return NULL;
}

/* Числовое поле: значение разбирается до первого нецифрового символа. */
static guint vorbis_uint_field(const guchar *data, gsize len, const char *field) {
    char *value = vorbis_field_value(data, len, field);
    if (!value) return 0;
    guint n = parse_uint_prefix(value);
    g_free(value);
    return n;
}

/* Читает блок VORBIS_COMMENT FLAC-файла (только метаданные в начале файла, без
 * загрузки аудиоданных). Возвращает данные блока (g_free) и их длину, либо NULL.
 * GStreamer отбрасывает TRACKNUMBER/DISCNUMBER, если значение не является чистым
 * числом (например Bandcamp пишет "2/"), поэтому номера и DISCSUBTITLE читаем
 * напрямую отсюда. */
static guchar *read_flac_vorbis_comment(const char *filepath, gsize *out_len) {
    *out_len = 0;

    gchar *lower = g_ascii_strdown(filepath, -1);
    gboolean is_flac = g_str_has_suffix(lower, ".flac");
    g_free(lower);
    if (!is_flac) return NULL;

    GFile *file = g_file_new_for_path(filepath);
    GFileInputStream *in = g_file_read(file, NULL, NULL);
    g_object_unref(file);
    if (!in) return NULL;

    guchar *result = NULL;
    guchar magic[4];
    gsize got = 0;
    if (g_input_stream_read_all(G_INPUT_STREAM(in), magic, 4, &got, NULL, NULL) &&
        got == 4 && memcmp(magic, "fLaC", 4) == 0) {
        for (;;) {
            guchar header[4];
            got = 0;
            if (!g_input_stream_read_all(G_INPUT_STREAM(in), header, 4, &got, NULL, NULL) || got != 4)
                break;

            guint type = header[0] & 0x7f;
            gsize block_len = ((guint)header[1] << 16) | ((guint)header[2] << 8) | (guint)header[3];

            if (type == 4) { /* VORBIS_COMMENT */
                guchar *buf = g_malloc(block_len);
                got = 0;
                if (g_input_stream_read_all(G_INPUT_STREAM(in), buf, block_len, &got, NULL, NULL) &&
                    got == block_len) {
                    result = buf;
                    *out_len = block_len;
                } else {
                    g_free(buf);
                }
                break;
            }

            if (header[0] & 0x80) break; /* последний блок метаданных */
            if (g_input_stream_skip(G_INPUT_STREAM(in), block_len, NULL, NULL) != (gssize)block_len)
                break;
        }
    }

    g_object_unref(in);
    return result;
}

static guint extract_year(const GstTagList *tags) {
    if (!tags) return 0;

    GstDateTime *dt = NULL;
    if (gst_tag_list_get_date_time(tags, GST_TAG_DATE_TIME, &dt) && dt) {
        guint y = gst_date_time_has_year(dt) ? (guint)gst_date_time_get_year(dt) : 0;
        gst_date_time_unref(dt);
        if (y > 0) return y;
    }

    GDate *date = NULL;
    if (gst_tag_list_get_date(tags, GST_TAG_DATE, &date) && date) {
        guint y = g_date_valid(date) ? (guint)g_date_get_year(date) : 0;
        g_date_free(date);
        return y;
    }

    return 0;
}

/* Разбирает один файл и возвращает трек (или NULL). Вызывается из нескольких
 * потоков, поэтому каждый поток использует собственный GstDiscoverer. */
static Track *discover_track(GstDiscoverer *discoverer, const char *filepath) {
    char *uri = gst_filename_to_uri(filepath, NULL);
    if (!uri) return NULL;

    GError *err = NULL;
    GstDiscovererInfo *info = gst_discoverer_discover_uri(discoverer, uri, &err);
    if (err) {
        g_error_free(err);
        g_free(uri);
        return NULL;
    }

    Track *result = NULL;
    if (gst_discoverer_info_get_result(info) == GST_DISCOVERER_OK) {
        const GstTagList *tags = gst_discoverer_info_get_tags(info);
        char *title = NULL, *artist = NULL, *album = NULL, *album_artist = NULL, *genre = NULL;
        guint track_number = 0;
        guint disc_number = 0;

        if (!tags || !gst_tag_list_get_string(tags, GST_TAG_TITLE, &title))
            title = g_path_get_basename(filepath);

        if (!tags || !gst_tag_list_get_string(tags, GST_TAG_ARTIST, &artist))
            artist = g_strdup("Неизвестный исполнитель");

        if (!tags || !gst_tag_list_get_string(tags, GST_TAG_ALBUM, &album))
            album = g_strdup("Неизвестный альбом");

        if (!tags || !gst_tag_list_get_string(tags, GST_TAG_ALBUM_ARTIST, &album_artist))
            album_artist = g_strdup(artist);

        if (!tags || !gst_tag_list_get_string(tags, GST_TAG_GENRE, &genre))
            genre = g_strdup("Без жанра");

        if (tags) {
            gst_tag_list_get_uint(tags, GST_TAG_TRACK_NUMBER, &track_number);
            gst_tag_list_get_uint(tags, GST_TAG_ALBUM_VOLUME_NUMBER, &disc_number);
        }

        guint year = extract_year(tags);
        char *cover_path = extract_and_cache_cover_locked(tags, album_artist, album);

        /* Длительность из discoverer приходит в наносекундах; в модели храним
         * секунды — этого достаточно для показа и сортировки. */
        guint duration = 0;
        GstClockTime dur_ns = gst_discoverer_info_get_duration(info);
        if (dur_ns != GST_CLOCK_TIME_NONE) duration = (guint)(dur_ns / GST_SECOND);

        result = track_new(uri, title, artist, album, album_artist,
                           genre, track_number, disc_number, NULL,
                           year, cover_path, duration);

        g_free(title);
        g_free(artist);
        g_free(album);
        g_free(album_artist);
        g_free(genre);
        g_free(cover_path);
    }

    gst_discoverer_info_unref(info);
    g_free(uri);
    return result;
}

/* Дозаполняет номера/подпись диска из FLAC-блока VORBIS_COMMENT. Читается только
 * для файлов, где это действительно нужно (GStreamer не дал номер трека —
 * типичный случай у Bandcamp с тегом вида «2/»; либо альбом многодисковый и нужен
 * DISCNUMBER/DISCSUBTITLE), чтобы не открывать каждый FLAC лишний раз. */
static void refine_disc_metadata(Track *t, const char *filepath) {
    gsize vlen = 0;
    guchar *vorbis = read_flac_vorbis_comment(filepath, &vlen);
    if (!vorbis) return;

    if (t->track_number == 0) {
        guint n = vorbis_uint_field(vorbis, vlen, "TRACKNUMBER");
        if (n > 0) t->track_number = n;
    }
    if (t->disc_number == 0) {
        guint n = vorbis_uint_field(vorbis, vlen, "DISCNUMBER");
        if (n > 0) t->disc_number = n;
    }
    if (!t->disc_subtitle) {
        char *sub = vorbis_field_value(vorbis, vlen, "DISCSUBTITLE");
        if (!sub) sub = vorbis_field_value(vorbis, vlen, "DISCTITLE");
        t->disc_subtitle = sub;
    }
    g_free(vorbis);
}

/* ===== параллельное сканирование =====
 * Метаданные каждого файла извлекаются синхронным API GStreamer, но несколько
 * файлов обрабатываются одновременно: на больших медиатеках (десятки тысяч
 * файлов) это в разы быстрее последовательного обхода. Порядок результатов
 * сохраняется — файлы сначала собираются списком, а треки раскладываются по
 * своим индексам. */

/* Собирает пути аудиофайлов рекурсивным обходом (быстро, без GStreamer). */
static void collect_audio_files(const char *dir_path, GPtrArray *files) {
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (!dir) return;

    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (name[0] == '.') continue;
        char *full_path = g_build_filename(dir_path, name, NULL);

        if (g_file_test(full_path, G_FILE_TEST_IS_DIR)) {
            collect_audio_files(full_path, files);
            g_free(full_path);
        } else if (is_audio_file(name)) {
            g_ptr_array_add(files, full_path); /* владение переходит массиву */
        } else {
            g_free(full_path);
        }
    }
    g_dir_close(dir);
}

#define SCAN_MAX_THREADS 8

typedef struct {
    GPtrArray *files;   /* char* (заимствованы) */
    Track **results;    /* трек по индексу файла */
    gint next;          /* atomic-счётчик следующего файла */
} ScanWork;

static gpointer scan_worker_thread(gpointer data) {
    ScanWork *work = data;
    GstDiscoverer *discoverer = gst_discoverer_new(5 * GST_SECOND, NULL);
    if (!discoverer) return NULL;

    for (;;) {
        gint index = g_atomic_int_add(&work->next, 1);
        if (index >= (gint)work->files->len) break;
        work->results[index] = discover_track(discoverer,
                                              g_ptr_array_index(work->files, index));
    }

    if (discoverer) gst_object_unref(discoverer);
    return NULL;
}

static void scan_thread_func(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object;
    (void)task_data;
    (void)cancellable;

    const char *music_dir = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
    if (!music_dir) music_dir = g_get_home_dir();

    GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
    collect_audio_files(music_dir, files);

    Track **results = g_new0(Track *, files->len);
    ScanWork work = { files, results, 0 };

    guint n_threads = g_get_num_processors();
    if (n_threads < 2) n_threads = 2;
    if (n_threads > SCAN_MAX_THREADS) n_threads = SCAN_MAX_THREADS;
    if (files->len < n_threads) n_threads = MAX(files->len, 1u);

    GThread *threads[SCAN_MAX_THREADS] = { NULL };
    for (guint i = 0; i < n_threads; i++) {
        threads[i] = g_thread_new("keruu-scan", scan_worker_thread, &work);
    }
    for (guint i = 0; i < n_threads; i++) {
        g_thread_join(threads[i]);
    }

    /* Второй проход: дочитываем FLAC-блоки VORBIS_COMMENT только там, где это
     * нужно. Сначала определяем многодисковые альбомы (max номер диска > 1),
     * затем для их треков и для треков без номера (Bandcamp «2/») вытаскиваем
     * DISCNUMBER/DISCSUBTITLE. Так на однодисковых альбомах без проблемных тегов
     * лишнего открытия каждого FLAC не происходит. */
    GHashTable *album_max_disc = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < files->len; i++) {
        Track *t = results[i];
        if (!t) continue;
        char *key = g_strdup_printf("%s\x1f%s",
                                    t->album_artist ? t->album_artist : "",
                                    t->album ? t->album : "");
        guint cur = GPOINTER_TO_UINT(g_hash_table_lookup(album_max_disc, key));
        if (t->disc_number > cur) {
            g_hash_table_replace(album_max_disc, key, GUINT_TO_POINTER(MAX(t->disc_number, 1u)));
        } else {
            g_free(key);
        }
    }
    for (guint i = 0; i < files->len; i++) {
        Track *t = results[i];
        if (!t) continue;
        char *key = g_strdup_printf("%s\x1f%s",
                                    t->album_artist ? t->album_artist : "",
                                    t->album ? t->album : "");
        guint max_disc = GPOINTER_TO_UINT(g_hash_table_lookup(album_max_disc, key));
        g_free(key);
        if (t->track_number == 0 || max_disc > 1) {
            refine_disc_metadata(t, g_ptr_array_index(files, i));
        }
    }
    g_hash_table_destroy(album_max_disc);

    GPtrArray *tracks = g_ptr_array_new_with_free_func((GDestroyNotify)track_free);
    for (guint i = 0; i < files->len; i++) {
        if (results[i]) g_ptr_array_add(tracks, results[i]);
    }
    g_free(results);
    g_ptr_array_unref(files);

    cache_save_tracks(tracks);
    g_task_return_pointer(task, tracks, (GDestroyNotify)g_ptr_array_unref);
}

void scanner_scan_async(GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data) {
    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_run_in_thread(task, scan_thread_func);
    g_object_unref(task);
}

GPtrArray *scanner_scan_finish(GAsyncResult *res, GError **error) {
    return g_task_propagate_pointer(G_TASK(res), error);
}
