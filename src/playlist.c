#include "playlist.h"
#include <gio/gio.h>
#include <glib/gstdio.h>

#define FAVORITE_FILE "favorites.m3u8"

static char *get_data_playlists_dir(void) {
    return g_build_filename(g_get_user_data_dir(), "denchiku", "playlists", NULL);
}

static char *get_favorites_path(void) {
    char *dir = get_data_playlists_dir();
    char *path = g_build_filename(dir, FAVORITE_FILE, NULL);
    g_free(dir);
    return path;
}

static gboolean has_playlist_ext(const char *name) {
    char *lower = g_ascii_strdown(name, -1);
    gboolean result = g_str_has_suffix(lower, ".m3u") || g_str_has_suffix(lower, ".m3u8");
    g_free(lower);
    return result;
}

/* Имя плейлиста берём из строки-комментария "#PLAYLIST:", которую пишем сами;
 * для внешних плейлистов — из имени файла. */
static char *read_playlist_name(const char *path, const char *fallback) {
    char *contents = NULL;
    char *name = NULL;

    if (g_file_get_contents(path, &contents, NULL, NULL)) {
        char **lines = g_strsplit(contents, "\n", -1);
        for (guint i = 0; lines[i] != NULL; i++) {
            if (g_str_has_prefix(lines[i], "#PLAYLIST:")) {
                char *value = g_strdup(lines[i] + strlen("#PLAYLIST:"));
                g_strstrip(value);
                if (*value) {
                    name = value;
                } else {
                    g_free(value);
                }
                break;
            }
        }
        g_strfreev(lines);
        g_free(contents);
    }

    return name ? name : g_strdup(fallback);
}

static Playlist *make_playlist(const char *path, const char *fav_path) {
    Playlist *pl = g_new0(Playlist, 1);
    pl->path = g_strdup(path);
    pl->favorite = fav_path && g_strcmp0(path, fav_path) == 0;

    char *base = g_path_get_basename(path);
    char *dot = g_strrstr(base, ".");
    if (dot) *dot = '\0';

    const char *fallback = pl->favorite ? "Любимые треки" : base;
    pl->name = read_playlist_name(path, fallback);
    g_free(base);
    return pl;
}

static gint cmp_playlist(gconstpointer a, gconstpointer b) {
    const Playlist *pa = *(const Playlist * const *)a;
    const Playlist *pb = *(const Playlist * const *)b;
    /* «Любимые треки» всегда первыми, дальше — по имени. */
    if (pa->favorite != pb->favorite) return pa->favorite ? -1 : 1;
    return g_utf8_collate(pa->name ? pa->name : "", pb->name ? pb->name : "");
}

static void scan_dir(GPtrArray *out, GHashTable *seen, const char *dir,
                     gboolean recursive, const char *fav_path) {
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;

    const char *name;
    while ((name = g_dir_read_name(d)) != NULL) {
        if (name[0] == '.') continue;
        char *full = g_build_filename(dir, name, NULL);

        if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
            /* Символические ссылки на каталоги не разворачиваем: иначе цикл
             * ссылок приводит к бесконечной рекурсии. */
            if (recursive && !g_file_test(full, G_FILE_TEST_IS_SYMLINK)) {
                scan_dir(out, seen, full, TRUE, fav_path);
            }
        } else if (has_playlist_ext(name) && !g_hash_table_contains(seen, full)) {
            g_hash_table_add(seen, g_strdup(full));
            g_ptr_array_add(out, make_playlist(full, fav_path));
        }
        g_free(full);
    }
    g_dir_close(d);
}

static void ensure_favorites_file(void) {
    char *dir = get_data_playlists_dir();
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    char *path = get_favorites_path();
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_file_set_contents(path, "#EXTM3U\n#PLAYLIST:Любимые треки\n", -1, NULL);
    }
    g_free(path);
}

GPtrArray *playlist_scan_all(void) {
    ensure_favorites_file();

    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)playlist_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *fav_path = get_favorites_path();

    char *data_dir = get_data_playlists_dir();
    scan_dir(out, seen, data_dir, FALSE, fav_path);
    g_free(data_dir);

    const char *music = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
    if (music) scan_dir(out, seen, music, TRUE, fav_path);

    g_free(fav_path);
    g_hash_table_destroy(seen);

    g_ptr_array_sort(out, cmp_playlist);
    return out;
}

Playlist *playlist_find_favorite(GPtrArray *playlists) {
    if (!playlists) return NULL;
    for (guint i = 0; i < playlists->len; i++) {
        Playlist *pl = g_ptr_array_index(playlists, i);
        if (pl->favorite) return pl;
    }
    return NULL;
}

Playlist *playlist_find_by_path(GPtrArray *playlists, const char *path) {
    if (!playlists || !path) return NULL;
    for (guint i = 0; i < playlists->len; i++) {
        Playlist *pl = g_ptr_array_index(playlists, i);
        if (g_strcmp0(pl->path, path) == 0) return pl;
    }
    return NULL;
}

/* Приводит строку плейлиста к file:// URI (или оставляет удалённую схему). */
static char *normalize_entry(const char *raw, const char *base_dir) {
    char *s = g_strdup(raw);
    g_strstrip(s);
    if (!*s) {
        g_free(s);
        return NULL;
    }

    char *uri = NULL;
    if (g_str_has_prefix(s, "file://")) {
        char *fs = g_filename_from_uri(s, NULL, NULL);
        if (fs) {
            uri = g_filename_to_uri(fs, NULL, NULL);
            g_free(fs);
        } else {
            uri = g_strdup(s);
        }
    } else {
        char *scheme = g_uri_parse_scheme(s);
        if (scheme) {
            /* Удалённый ресурс (http и т.п.) — оставляем как есть. */
            uri = g_strdup(s);
        } else {
            char *path = g_strdup(s);
            if (!g_path_is_absolute(path)) {
                char *abs = g_build_filename(base_dir, path, NULL);
                g_free(path);
                path = abs;
            }
            uri = g_filename_to_uri(path, NULL, NULL);
            g_free(path);
        }
        g_free(scheme);
    }

    g_free(s);
    return uri;
}

GPtrArray *playlist_load_uris(const Playlist *pl) {
    GPtrArray *uris = g_ptr_array_new_with_free_func(g_free);
    if (!pl) return uris;

    char *contents = NULL;
    if (!g_file_get_contents(pl->path, &contents, NULL, NULL)) return uris;

    /* Убираем BOM, если файл сохранён в UTF-8 с сигнатурой. */
    if (g_str_has_prefix(contents, "\xEF\xBB\xBF")) {
        memmove(contents, contents + 3, strlen(contents + 3) + 1);
    }

    char *dir = g_path_get_dirname(pl->path);
    char **lines = g_strsplit(contents, "\n", -1);
    for (guint i = 0; lines[i] != NULL; i++) {
        char *line = lines[i];
        g_strchomp(line);
        g_strstrip(line);
        if (!*line || line[0] == '#') continue;

        char *uri = normalize_entry(line, dir);
        if (uri) g_ptr_array_add(uris, uri);
    }
    g_strfreev(lines);
    g_free(dir);
    g_free(contents);
    return uris;
}

static Track *track_from_uri(const char *uri) {
    char *fs = g_filename_from_uri(uri, NULL, NULL);
    char *base = fs ? g_path_get_basename(fs) : g_strdup(uri);
    Track *t = track_new(uri, base, "Неизвестный исполнитель", "Неизвестный альбом",
                         NULL, "Без жанра", 0, 0, NULL, 0, NULL, 0);
    g_free(base);
    g_free(fs);
    return t;
}

GPtrArray *playlist_resolve_tracks(const Playlist *pl, GPtrArray *library) {
    GPtrArray *result = g_ptr_array_new_with_free_func((GDestroyNotify)track_free);
    if (!pl) return result;

    GHashTable *by_uri = g_hash_table_new(g_str_hash, g_str_equal);
    if (library) {
        for (guint i = 0; i < library->len; i++) {
            Track *t = g_ptr_array_index(library, i);
            if (t->uri) g_hash_table_insert(by_uri, t->uri, t);
        }
    }

    GPtrArray *uris = playlist_load_uris(pl);
    for (guint i = 0; i < uris->len; i++) {
        const char *uri = g_ptr_array_index(uris, i);
        Track *found = g_hash_table_lookup(by_uri, uri);
        g_ptr_array_add(result, found ? track_copy(found) : track_from_uri(uri));
    }

    g_ptr_array_unref(uris);
    g_hash_table_destroy(by_uri);
    return result;
}

/* Множество URI плейлиста, строится один раз и переиспользуется: проверки
 * принадлежности вызываются на каждую кнопку-«сердечко» и не должны каждый раз
 * читать файл с диска. */
static GHashTable *get_uri_set(const Playlist *pl) {
    Playlist *mutable = (Playlist *)pl;
    if (mutable->uri_cache) return mutable->uri_cache;

    GHashTable *set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GPtrArray *uris = playlist_load_uris(pl);
    for (guint i = 0; i < uris->len; i++) {
        g_hash_table_add(set, g_strdup(g_ptr_array_index(uris, i)));
    }
    g_ptr_array_unref(uris);

    mutable->uri_cache = set;
    return set;
}

static void invalidate_uri_cache(Playlist *pl) {
    if (pl->uri_cache) {
        g_hash_table_destroy(pl->uri_cache);
        pl->uri_cache = NULL;
    }
}

gboolean playlist_contains_uri(const Playlist *pl, const char *uri) {
    if (!pl || !uri) return FALSE;
    return g_hash_table_contains(get_uri_set(pl), uri);
}

/* Читает все строки файла как есть (без разбора), чтобы при изменении
 * сохранить комментарии, #EXTINF и прочие директивы сторонних плейлистов. */
static GPtrArray *load_raw_lines(const Playlist *pl) {
    GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
    if (!pl) return lines;

    char *contents = NULL;
    if (!g_file_get_contents(pl->path, &contents, NULL, NULL)) return lines;
    if (g_str_has_prefix(contents, "\xEF\xBB\xBF")) {
        memmove(contents, contents + 3, strlen(contents + 3) + 1);
    }

    char **split = g_strsplit(contents, "\n", -1);
    for (guint i = 0; split[i] != NULL; i++) {
        g_ptr_array_add(lines, g_strdup(split[i]));
    }
    g_strfreev(split);
    g_free(contents);
    return lines;
}

static gboolean write_raw_lines(const char *path, GPtrArray *lines) {
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < lines->len; i++) {
        g_string_append(s, (const char *)g_ptr_array_index(lines, i));
        if (i + 1 < lines->len) g_string_append_c(s, '\n');
    }
    gboolean ok = g_file_set_contents(path, s->str, s->len, NULL);
    g_string_free(s, TRUE);
    return ok;
}

/* Дописывает строку URI в конец, не перегенерируя файл: так сохраняются
 * метаданные и относительные пути плейлистов, созданных не приложением. */
static gboolean append_uri(const Playlist *pl, const char *uri) {
    GPtrArray *lines = load_raw_lines(pl);
    if (lines->len > 0) {
        const char *last = g_ptr_array_index(lines, lines->len - 1);
        if (last[0] == '\0') g_ptr_array_remove_index(lines, lines->len - 1);
    }
    g_ptr_array_add(lines, g_strdup(uri));
    g_ptr_array_add(lines, g_strdup(""));

    gboolean ok = write_raw_lines(pl->path, lines);
    g_ptr_array_unref(lines);

    invalidate_uri_cache((Playlist *)pl);
    return ok;
}

gboolean playlist_add_uri(const Playlist *pl, const char *uri) {
    if (!pl || !uri || !*uri) return FALSE;
    if (playlist_contains_uri(pl, uri)) return TRUE;
    return append_uri(pl, uri);
}

/* Добавляет URI даже если он уже есть — вторая копия трека в плейлисте. */
gboolean playlist_add_uri_dup(const Playlist *pl, const char *uri) {
    if (!pl || !uri || !*uri) return FALSE;
    return append_uri(pl, uri);
}

gboolean playlist_remove_uri(const Playlist *pl, const char *uri) {
    if (!pl || !uri) return FALSE;

    char *dir = g_path_get_dirname(pl->path);
    GPtrArray *lines = load_raw_lines(pl);
    GPtrArray *kept = g_ptr_array_new_with_free_func(g_free);
    gboolean removed = FALSE;

    for (guint i = 0; i < lines->len; i++) {
        const char *line = g_ptr_array_index(lines, i);
        char *trimmed = g_strdup(line);
        g_strstrip(trimmed);

        if (*trimmed && trimmed[0] != '#') {
            char *norm = normalize_entry(trimmed, dir);
            if (norm && g_strcmp0(norm, uri) == 0) {
                removed = TRUE;
                g_free(norm);
                g_free(trimmed);
                continue;
            }
            g_free(norm);
        }
        g_free(trimmed);
        g_ptr_array_add(kept, g_strdup(line));
    }

    gboolean ok = TRUE;
    if (removed) ok = write_raw_lines(pl->path, kept);

    g_ptr_array_unref(kept);
    g_ptr_array_unref(lines);
    g_free(dir);

    if (removed) invalidate_uri_cache((Playlist *)pl);
    return removed && ok;
}

static char *sanitize_filename(const char *name) {
    char *s = g_strdup(name ? name : "");
    for (char *p = s; *p; p++) {
        if (*p == '/' || *p == '\\') *p = '_';
    }
    g_strstrip(s);
    if (!*s) {
        g_free(s);
        s = g_strdup("Плейлист");
    }
    return s;
}

Playlist *playlist_create(const char *name) {
    if (!name || !*name) return NULL;

    char *dir = get_data_playlists_dir();
    g_mkdir_with_parents(dir, 0755);

    char *safe = sanitize_filename(name);
    char *path = g_build_filename(dir, safe, NULL);
    char *final = g_strconcat(path, ".m3u8", NULL);
    g_free(path);

    for (int n = 2; g_file_test(final, G_FILE_TEST_EXISTS); n++) {
        g_free(final);
        char *alt = g_strdup_printf("%s %d", safe, n);
        char *alt_path = g_build_filename(dir, alt, NULL);
        final = g_strconcat(alt_path, ".m3u8", NULL);
        g_free(alt);
        g_free(alt_path);
    }

    char *contents = g_strdup_printf("#EXTM3U\n#PLAYLIST:%s\n", name);
    gboolean ok = g_file_set_contents(final, contents, -1, NULL);
    g_free(contents);
    g_free(safe);
    g_free(dir);

    if (!ok) {
        g_free(final);
        return NULL;
    }

    Playlist *pl = g_new0(Playlist, 1);
    pl->path = final;
    pl->name = g_strdup(name);
    pl->favorite = FALSE;
    return pl;
}

char *playlist_rename(const Playlist *pl, const char *new_name) {
    if (!pl || pl->favorite || !new_name || !*new_name) return NULL;

    char *dir = g_path_get_dirname(pl->path);
    char *base = g_path_get_basename(pl->path);
    char *dot = g_strrstr(base, ".");
    char *ext = (dot && dot != base) ? g_strdup(dot) : g_strdup(".m3u8");
    char *safe = sanitize_filename(new_name);

    /* Новый файл — в той же папке, с тем же расширением; при совпадении имени
     * добавляем числовой суффикс. Совпадение с текущим путём не суффиксим:
     * это просто смена отображаемого имени без переноса файла. */
    char *filename = g_strconcat(safe, ext, NULL);
    char *newpath = g_build_filename(dir, filename, NULL);
    for (int n = 2; g_strcmp0(newpath, pl->path) != 0 &&
                    g_file_test(newpath, G_FILE_TEST_EXISTS); n++) {
        g_free(newpath);
        char *alt = g_strdup_printf("%s %d%s", safe, n, ext);
        newpath = g_build_filename(dir, alt, NULL);
        g_free(alt);
    }

    /* Перезаписываем содержимое: заголовок #PLAYLIST меняем на новый, а
     * содержимое (треки, #EXTINF, комментарии) сохраняем как есть. */
    GPtrArray *lines = load_raw_lines(pl);
    GString *out = g_string_new("#EXTM3U\n");
    g_string_append_printf(out, "#PLAYLIST:%s\n", new_name);
    for (guint i = 0; i < lines->len; i++) {
        const char *line = g_ptr_array_index(lines, i);
        if (g_str_has_prefix(line, "#EXTM3U") ||
            g_str_has_prefix(line, "#PLAYLIST:") ||
            *line == '\0') {
            continue;
        }
        g_string_append(out, line);
        g_string_append_c(out, '\n');
    }
    g_ptr_array_unref(lines);

    gboolean same = g_strcmp0(newpath, pl->path) == 0;
    gboolean ok = g_file_set_contents(newpath, out->str, out->len, NULL);
    g_string_free(out, TRUE);

    char *result = NULL;
    if (ok) {
        if (!same) g_remove(pl->path);
        result = newpath; /* владение переходит вызывающему */
        newpath = NULL;
    }

    g_free(newpath);
    g_free(filename);
    g_free(safe);
    g_free(ext);
    g_free(base);
    g_free(dir);
    return result;
}

gboolean playlist_delete(const Playlist *pl) {
    if (!pl || pl->favorite || !pl->path) return FALSE;
    return g_remove(pl->path) == 0;
}

void playlist_free(Playlist *pl) {
    if (!pl) return;
    if (pl->uri_cache) g_hash_table_destroy(pl->uri_cache);
    g_free(pl->path);
    g_free(pl->name);
    g_free(pl);
}
