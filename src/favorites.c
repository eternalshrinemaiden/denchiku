#include "favorites.h"

#include <glib/gstdio.h>
#include <string.h>

/* Пометки хранятся как два множества строк в памяти; на диск пишутся построчно,
 * чтобы произвольные UTF-8 имена (пробелы, «/», «;») не требовали экранирования,
 * как в GKeyFile-ключах. */
static GHashTable *g_album_likes;  /* ключ "album\x1falbum_artist" */
static GHashTable *g_artist_likes; /* имя исполнителя */

static GPtrArray *g_listeners; /* FavoritesListener* */
static gboolean g_loaded = FALSE;

typedef struct {
    FavoritesChangedFn cb;
    gpointer data;
} FavoritesListener;

static char *favorites_filepath(void) {
    char *dir = g_build_filename(g_get_user_config_dir(), "denchiku", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *path = g_build_filename(dir, "favorites.ini", NULL);
    g_free(dir);
    return path;
}

static char *album_key(const char *album, const char *album_artist) {
    return g_strdup_printf("%s\x1f%s", album ? album : "",
                           album_artist ? album_artist : "");
}

static void favorites_load(void) {
    if (g_loaded) return;
    g_loaded = TRUE;

    g_album_likes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_artist_likes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    char *path = favorites_filepath();
    char *contents = NULL;
    if (g_file_get_contents(path, &contents, NULL, NULL)) {
        char **lines = g_strsplit(contents, "\n", -1);
        for (guint i = 0; lines[i] != NULL; i++) {
            char *line = lines[i];
            g_strchomp(line);

            if (g_str_has_prefix(line, "A\t")) {
                /* "A\talbum\talbum_artist": разделяем по первому табу. */
                char *rest = line + 2;
                char *tab = strchr(rest, '\t');
                if (tab) {
                    *tab = '\0';
                    char *key = album_key(rest, tab + 1);
                    g_hash_table_add(g_album_likes, key);
                } else {
                    g_hash_table_add(g_album_likes, album_key(rest, ""));
                }
            } else if (g_str_has_prefix(line, "R\t")) {
                if (line[2] != '\0') g_hash_table_add(g_artist_likes, g_strdup(line + 2));
            }
        }
        g_strfreev(lines);
        g_free(contents);
    }
    g_free(path);
}

static void favorites_save(void) {
    GString *s = g_string_new("# denchiku: любимые альбомы и исполнители\n");

    if (g_album_likes) {
        GHashTableIter it;
        gpointer key, value;
        g_hash_table_iter_init(&it, g_album_likes);
        while (g_hash_table_iter_next(&it, &key, &value)) {
            const char *k = key;
            const char *sep = strchr(k, '\x1f');
            if (!sep) continue;
            /* Ключ уже собран как "album\x1fartist", поэтому пишем как есть. */
            g_string_append_printf(s, "A\t%.*s\t%s\n", (int)(sep - k), k, sep + 1);
        }
    }
    if (g_artist_likes) {
        GHashTableIter it;
        gpointer key, value;
        g_hash_table_iter_init(&it, g_artist_likes);
        while (g_hash_table_iter_next(&it, &key, &value)) {
            g_string_append_printf(s, "R\t%s\n", (const char *)key);
        }
    }

    char *path = favorites_filepath();
    g_file_set_contents(path, s->str, s->len, NULL);
    g_free(path);
    g_string_free(s, TRUE);
}

static void favorites_notify(void) {
    if (!g_listeners) return;
    for (guint i = 0; i < g_listeners->len; i++) {
        FavoritesListener *l = g_ptr_array_index(g_listeners, i);
        l->cb(l->data);
    }
}

gboolean favorites_album_is_liked(const char *album, const char *album_artist) {
    favorites_load();
    char *key = album_key(album, album_artist);
    gboolean liked = g_hash_table_contains(g_album_likes, key);
    g_free(key);
    return liked;
}

void favorites_album_set_liked(const char *album, const char *album_artist, gboolean liked) {
    favorites_load();

    char *key = album_key(album, album_artist);
    gboolean changed;
    if (liked) {
        changed = g_hash_table_add(g_album_likes, key); /* FALSE, если уже был */
        if (!changed) g_free(key);
    } else {
        changed = g_hash_table_remove(g_album_likes, key);
        g_free(key);
    }

    if (changed) {
        favorites_save();
        favorites_notify();
    }
}

gboolean favorites_artist_is_liked(const char *artist) {
    favorites_load();
    if (!artist || !*artist) return FALSE;
    return g_hash_table_contains(g_artist_likes, artist);
}

void favorites_artist_set_liked(const char *artist, gboolean liked) {
    favorites_load();
    if (!artist || !*artist) return;

    gboolean changed;
    if (liked) {
        changed = g_hash_table_add(g_artist_likes, g_strdup(artist));
    } else {
        changed = g_hash_table_remove(g_artist_likes, artist);
    }

    if (changed) {
        favorites_save();
        favorites_notify();
    }
}

void favorites_add_listener(FavoritesChangedFn cb, gpointer user_data) {
    if (!cb) return;
    if (!g_listeners) {
        g_listeners = g_ptr_array_new_with_free_func(g_free);
    }
    FavoritesListener *l = g_new0(FavoritesListener, 1);
    l->cb = cb;
    l->data = user_data;
    g_ptr_array_add(g_listeners, l);
}

void favorites_remove_listener(FavoritesChangedFn cb, gpointer user_data) {
    if (!g_listeners) return;
    for (guint i = 0; i < g_listeners->len; i++) {
        FavoritesListener *l = g_ptr_array_index(g_listeners, i);
        if (l->cb == cb && l->data == user_data) {
            g_ptr_array_remove_index(g_listeners, i);
            return;
        }
    }
}
