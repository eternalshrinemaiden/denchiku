#include "tag_editor.h"

/* Типы, по которым ищем приложения-кандидаты: фактически это «аудиофайлы»,
 * которые умеет открывать плеер/редактор. Список нужен, чтобы в настройках
 * показать осмысленный выбор, а не все программы системы. */
static const char *const AUDIO_MIME_TYPES[] = {
    "audio/mpeg", "audio/flac", "audio/x-flac", "audio/ogg",
    "audio/x-vorbis+ogg", "audio/opus", "audio/mp4", "audio/x-m4a",
    "audio/aac", "audio/x-wav", "audio/x-aiff", "audio/x-ape",
    NULL
};

static gint cmp_app_by_name(gconstpointer a, gconstpointer b) {
    GAppInfo *app_a = *(GAppInfo *const *)a;
    GAppInfo *app_b = *(GAppInfo *const *)b;
    const char *na = g_app_info_get_display_name(app_a);
    const char *nb = g_app_info_get_display_name(app_b);
    return g_utf8_collate(na ? na : "", nb ? nb : "");
}

GPtrArray *tag_editor_candidate_apps(void) {
    GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (guint t = 0; AUDIO_MIME_TYPES[t] != NULL; t++) {
        GList *apps = g_app_info_get_all_for_type(AUDIO_MIME_TYPES[t]);
        for (GList *l = apps; l != NULL; l = l->next) {
            GAppInfo *app = G_APP_INFO(l->data);
            const char *id = g_app_info_get_id(app);
            if (!id || g_hash_table_contains(seen, id)) {
                g_object_unref(app);
                continue;
            }
            g_hash_table_add(seen, g_strdup(id));
            g_ptr_array_add(out, app); /* владение переходит массиву */
        }
        g_list_free(apps);
    }

    g_hash_table_destroy(seen);
    g_ptr_array_sort(out, cmp_app_by_name);
    return out;
}

/* Находит установленное приложение по идентификатору .desktop. Отдельного
 * GDesktopAppInfo здесь не используем: заголовок gdesktopappinfo.h не входит в
 * набор include-путей текущих зависимостей (вынесен в gio-unix). */
static GAppInfo *find_app_by_id(const char *desktop_id) {
    if (!desktop_id || !*desktop_id) return NULL;

    GAppInfo *found = NULL;
    GList *all = g_app_info_get_all();
    for (GList *l = all; l != NULL; l = l->next) {
        GAppInfo *app = G_APP_INFO(l->data);
        if (g_strcmp0(g_app_info_get_id(app), desktop_id) == 0) {
            found = g_object_ref(app);
            break;
        }
    }
    g_list_free_full(all, g_object_unref);
    return found;
}

gboolean tag_editor_launch(const char *editor, GPtrArray *uris, GError **error) {
    if (!editor || !*editor) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Не выбрано приложение для редактирования тегов");
        return FALSE;
    }
    if (!uris || uris->len == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Не выбрано ни одного файла");
        return FALSE;
    }

    /* Приложение задаётся либо идентификатором/путём .desktop-файла, либо
     * командой (путём к исполняемому файлу). */
    GAppInfo *app = NULL;
    if (g_str_has_suffix(editor, ".desktop")) {
        const char *id = editor;
        const char *slash = strrchr(editor, '/');
        if (slash) id = slash + 1; /* идентификатор — имя файла .desktop */
        app = find_app_by_id(id);
    }
    if (!app) {
        app = g_app_info_create_from_commandline(editor, NULL,
                                                 G_APP_INFO_CREATE_NONE, error);
    }
    if (!app) return FALSE;

    GList *files = NULL;
    for (guint i = 0; i < uris->len; i++) {
        files = g_list_prepend(files, g_file_new_for_uri(g_ptr_array_index(uris, i)));
    }
    files = g_list_reverse(files);

    gboolean ok = g_app_info_launch(app, files, NULL, error);

    g_list_free_full(files, g_object_unref);
    g_object_unref(app);
    return ok;
}
