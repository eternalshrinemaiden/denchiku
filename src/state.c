#include "state.h"

#include <glib/gstdio.h>

static char *state_filepath(void) {
    char *dir = g_build_filename(g_get_user_config_dir(), "keruu", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *path = g_build_filename(dir, "state.ini", NULL);
    g_free(dir);
    return path;
}

static void free_queue_uris(char **uris, int len) {
    if (!uris) return;
    for (int i = 0; i < len; i++) g_free(uris[i]);
    g_free(uris);
}

AppState *state_load(void) {
    AppState *s = g_new0(AppState, 1);

    /* Значения по умолчанию. */
    s->width = 1480;
    s->height = 800;
    s->maximized = FALSE;
    s->shuffle = FALSE;
    s->repeat = 0;
    s->volume = 1.0;
    s->queue_uris = NULL;
    s->queue_len = 0;
    s->current_uri = NULL;
    s->queue_visible = FALSE;
    s->view_mode = 0;       /* VIEW_MODE_GRID */
    s->view_zoom = 1;       /* VIEW_ZOOM_MEDIUM */
    s->view_sort = 0;       /* VIEW_SORT_TITLE */
    s->view_sort_desc = FALSE;

    char *path = state_filepath();
    GKeyFile *kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        g_key_file_free(kf);
        g_free(path);
        return s;
    }

    s->width  = g_key_file_get_integer(kf, "window", "width", NULL);
    s->height = g_key_file_get_integer(kf, "window", "height", NULL);
    if (s->width <= 0) s->width = 1480;
    if (s->height <= 0) s->height = 800;
    s->maximized = g_key_file_get_boolean(kf, "window", "maximized", NULL);

    s->shuffle = g_key_file_get_boolean(kf, "player", "shuffle", NULL);
    s->repeat  = g_key_file_get_integer(kf, "player", "repeat", NULL);
    if (s->repeat < 0 || s->repeat > 2) s->repeat = 0;
    {
        GError *err = NULL;
        double vol = g_key_file_get_double(kf, "player", "volume", &err);
        if (err) { g_error_free(err); vol = 1.0; }
        s->volume = CLAMP(vol, 0.0, 1.0);
    }

    s->queue_len = g_key_file_get_integer(kf, "queue", "count", NULL);
    if (s->queue_len < 0) s->queue_len = 0;
    if (s->queue_len > 0) {
        s->queue_uris = g_new0(char *, s->queue_len + 1);
        int kept = 0;
        for (int i = 0; i < s->queue_len; i++) {
            char key[32];
            g_snprintf(key, sizeof key, "uri%d", i);
            char *uri = g_key_file_get_string(kf, "queue", key, NULL);
            if (uri && *uri) {
                s->queue_uris[kept++] = uri;
            } else {
                g_free(uri);
            }
        }
        s->queue_uris[kept] = NULL;
        s->queue_len = kept;
    }
    s->current_uri = g_key_file_get_string(kf, "queue", "current", NULL);
    if (s->current_uri && !*s->current_uri) {
        g_free(s->current_uri);
        s->current_uri = NULL;
    }
    s->queue_visible = g_key_file_get_boolean(kf, "queue", "visible", NULL);

    s->view_mode = g_key_file_get_integer(kf, "view", "mode", NULL);
    if (s->view_mode != 0 && s->view_mode != 1) s->view_mode = 0;
    s->view_zoom = g_key_file_get_integer(kf, "view", "zoom", NULL);
    if (s->view_zoom < 0 || s->view_zoom > 2) s->view_zoom = 1;
    s->view_sort = g_key_file_get_integer(kf, "view", "sort", NULL);
    if (s->view_sort < 0 || s->view_sort > 2) s->view_sort = 0;
    s->view_sort_desc = g_key_file_get_boolean(kf, "view", "sort_desc", NULL);

    g_key_file_free(kf);
    g_free(path);
    return s;
}

void state_save(const AppState *s) {
    if (!s) return;

    GKeyFile *kf = g_key_file_new();

    g_key_file_set_integer(kf, "window", "width", s->width);
    g_key_file_set_integer(kf, "window", "height", s->height);
    g_key_file_set_boolean(kf, "window", "maximized", s->maximized);

    g_key_file_set_boolean(kf, "player", "shuffle", s->shuffle);
    g_key_file_set_integer(kf, "player", "repeat", s->repeat);
    g_key_file_set_double(kf, "player", "volume", s->volume);

    g_key_file_set_integer(kf, "queue", "count", s->queue_len);
    for (int i = 0; i < s->queue_len; i++) {
        char key[32];
        g_snprintf(key, sizeof key, "uri%d", i);
        g_key_file_set_string(kf, "queue", key, s->queue_uris[i] ? s->queue_uris[i] : "");
    }
    g_key_file_set_string(kf, "queue", "current", s->current_uri ? s->current_uri : "");
    g_key_file_set_boolean(kf, "queue", "visible", s->queue_visible);

    g_key_file_set_integer(kf, "view", "mode", s->view_mode);
    g_key_file_set_integer(kf, "view", "zoom", s->view_zoom);
    g_key_file_set_integer(kf, "view", "sort", s->view_sort);
    g_key_file_set_boolean(kf, "view", "sort_desc", s->view_sort_desc);

    char *path = state_filepath();
    g_key_file_save_to_file(kf, path, NULL);
    g_free(path);
    g_key_file_free(kf);
}

void state_free(AppState *state) {
    if (!state) return;
    free_queue_uris(state->queue_uris, state->queue_len);
    g_free(state->current_uri);
    g_free(state);
}
