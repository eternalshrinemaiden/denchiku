#include "cache.h"
#include "track.h"

static char *get_cache_filepath(void) {
    char *dir = g_build_filename(g_get_user_cache_dir(), "keruu", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *path = g_build_filename(dir, "library.cache", NULL);
    g_free(dir);
    return path;
}

GPtrArray *cache_load_tracks(void) {
    char *path = get_cache_filepath();
    GKeyFile *kf = g_key_file_new();

    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        g_key_file_free(kf);
        g_free(path);
        return NULL;
    }

    gsize n_groups = 0;
    char **groups = g_key_file_get_groups(kf, &n_groups);
    GPtrArray *tracks = g_ptr_array_new_with_free_func((GDestroyNotify)track_free);

    for (gsize i = 0; i < n_groups; i++) {
        char *uri          = g_key_file_get_string(kf, groups[i], "uri", NULL);
        char *title        = g_key_file_get_string(kf, groups[i], "title", NULL);
        char *artist       = g_key_file_get_string(kf, groups[i], "artist", NULL);
        char *album        = g_key_file_get_string(kf, groups[i], "album", NULL);
        char *album_artist = g_key_file_get_string(kf, groups[i], "album_artist", NULL);
        char *genre        = g_key_file_get_string(kf, groups[i], "genre", NULL);
        int track_num      = g_key_file_get_integer(kf, groups[i], "track_number", NULL);
        int disc_num       = g_key_file_get_integer(kf, groups[i], "disc_number", NULL);
        int year_val       = g_key_file_get_integer(kf, groups[i], "year", NULL);
        char *cover_path   = g_key_file_get_string(kf, groups[i], "cover_path", NULL);
        char *disc_subtitle = g_key_file_get_string(kf, groups[i], "disc_subtitle", NULL);

        if (cover_path && cover_path[0] == '\0') {
            g_free(cover_path);
            cover_path = NULL;
        }
        if (disc_subtitle && disc_subtitle[0] == '\0') {
            g_free(disc_subtitle);
            disc_subtitle = NULL;
        }

        if (uri && title) {
            guint tn = track_num > 0 ? (guint)track_num : 0;
            guint dn = disc_num > 0 ? (guint)disc_num : 0;
            guint yr = year_val > 0 ? (guint)year_val : 0;
            g_ptr_array_add(tracks, track_new(uri, title, artist, album, album_artist,
                                              genre, tn, dn, disc_subtitle, yr, cover_path));
        }

        g_free(uri);
        g_free(title);
        g_free(artist);
        g_free(album);
        g_free(album_artist);
        g_free(genre);
        g_free(cover_path);
        g_free(disc_subtitle);
    }

    g_strfreev(groups);
    g_key_file_free(kf);
    g_free(path);
    return tracks;
}

void cache_save_tracks(GPtrArray *tracks) {
    if (!tracks) return;

    char *path = get_cache_filepath();
    GKeyFile *kf = g_key_file_new();

    for (guint i = 0; i < tracks->len; i++) {
        Track *t = g_ptr_array_index(tracks, i);
        char *group = g_strdup_printf("track_%u", i);

        g_key_file_set_string(kf, group, "uri", t->uri ? t->uri : "");
        g_key_file_set_string(kf, group, "title", t->title ? t->title : "");
        g_key_file_set_string(kf, group, "artist", t->artist ? t->artist : "");
        g_key_file_set_string(kf, group, "album", t->album ? t->album : "");
        g_key_file_set_string(kf, group, "album_artist", t->album_artist ? t->album_artist : "");
        g_key_file_set_string(kf, group, "genre", t->genre ? t->genre : "");
        g_key_file_set_integer(kf, group, "track_number", (int)t->track_number);
        g_key_file_set_integer(kf, group, "disc_number", (int)t->disc_number);
        g_key_file_set_integer(kf, group, "year", (int)t->year);
        g_key_file_set_string(kf, group, "cover_path", t->cover_path ? t->cover_path : "");
        g_key_file_set_string(kf, group, "disc_subtitle", t->disc_subtitle ? t->disc_subtitle : "");

        g_free(group);
    }

    g_key_file_save_to_file(kf, path, NULL);
    g_key_file_free(kf);
    g_free(path);
}
