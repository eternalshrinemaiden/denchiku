#include "settings.h"

#include <glib/gstdio.h>

static TagSplitSettings g_settings;
static gboolean g_initialized = FALSE;

static GRegex *g_regex_artists = NULL;
static GRegex *g_regex_genres  = NULL;

static const char *ARTISTS_SEPARATORS[] = {
    ",", ";", "&", "feat", "ft", "featuring", "vs", "versus", NULL
};
static const char *GENRES_SEPARATORS[] = {
    ",", ";", "/", "&", NULL
};

static char *settings_filepath(void) {
    char *dir = g_build_filename(g_get_user_config_dir(), "keruu", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *path = g_build_filename(dir, "settings.ini", NULL);
    g_free(dir);
    return path;
}

static void set_default_separators(TagSplitCategory *c, const char * const *separators) {
    for (guint i = 0; separators[i] != NULL; i++)
        g_ptr_array_add(c->separators, g_strdup(separators[i]));
}

static void set_defaults(void) {
    g_settings.artists.enabled    = TRUE;
    g_settings.artists.separators = g_ptr_array_new_with_free_func(g_free);
    set_default_separators(&g_settings.artists, ARTISTS_SEPARATORS);

    g_settings.genres.enabled    = TRUE;
    g_settings.genres.separators = g_ptr_array_new_with_free_func(g_free);
    set_default_separators(&g_settings.genres, GENRES_SEPARATORS);
}

static TagSplitCategory *category_for(gboolean artists) {
    return artists ? &g_settings.artists : &g_settings.genres;
}

/* Собирает фрагмент регулярного выражения для одного разделителя:
 *  - слово целиком (только буквы, напр. feat, versus) — границами служат
 *    пробелы, точка необязательна;
 *  - «&» — только между словами, чтобы не рвать имена вроде «R&B»;
 *  - прочее (знаки препинания, произвольные строки) — как есть, с необязательными
 *    пробелами вокруг. */
static char *separator_fragment(const char *token) {
    gboolean is_word = *token != '\0';
    for (const char *p = token; *p; p++) {
        if (!g_ascii_isalpha(*p)) { is_word = FALSE; break; }
    }

    char *escaped = g_regex_escape_string(token, -1);
    char *fragment;
    if (is_word)
        fragment = g_strdup_printf("\\s+%s\\.?(?:\\s+|$)", escaped);
    else if (g_strcmp0(token, "&") == 0)
        fragment = g_strdup("\\s+&\\s+");
    else
        fragment = g_strdup_printf("\\s*%s\\s*", escaped);
    g_free(escaped);
    return fragment;
}

/* Собирает тело регулярного выражения из списка разделителей. Возвращает NULL,
 * если разбиение выключено или список пуст. */
static char *build_pattern(const TagSplitCategory *c) {
    if (!c->enabled || !c->separators || c->separators->len == 0) return NULL;

    GPtrArray *frags = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < c->separators->len; i++) {
        const char *token = g_ptr_array_index(c->separators, i);
        if (!token || !*token) continue;
        g_ptr_array_add(frags, separator_fragment(token));
    }

    if (frags->len == 0) {
        g_ptr_array_free(frags, TRUE);
        return NULL;
    }

    GString *pattern = g_string_new("(?:");
    for (guint i = 0; i < frags->len; i++) {
        if (i > 0) g_string_append_c(pattern, '|');
        g_string_append(pattern, g_ptr_array_index(frags, i));
    }
    g_string_append_c(pattern, ')');
    g_ptr_array_free(frags, TRUE);
    return g_string_free(pattern, FALSE);
}

void settings_rebuild(void) {
    if (g_regex_artists) { g_regex_unref(g_regex_artists); g_regex_artists = NULL; }
    if (g_regex_genres)  { g_regex_unref(g_regex_genres);  g_regex_genres  = NULL; }

    char *pa = build_pattern(&g_settings.artists);
    if (pa) {
        g_regex_artists = g_regex_new(pa, G_REGEX_CASELESS, 0, NULL);
        g_free(pa);
    }
    char *pg = build_pattern(&g_settings.genres);
    if (pg) {
        g_regex_genres = g_regex_new(pg, G_REGEX_CASELESS, 0, NULL);
        g_free(pg);
    }
}

static void load_separators(GKeyFile *kf, const char *key, TagSplitCategory *c) {
    gsize n = 0;
    char **list = g_key_file_get_string_list(kf, "tag_split", key, &n, NULL);
    if (!list) return; /* ключа нет — оставляем значения по умолчанию */

    g_ptr_array_set_size(c->separators, 0);
    for (gsize i = 0; i < n; i++) {
        char *token = g_strstrip(g_strdup(list[i]));
        if (*token)
            g_ptr_array_add(c->separators, token);
        else
            g_free(token);
    }
    g_strfreev(list);
}

void settings_load(void) {
    if (!g_initialized) {
        set_defaults();
        g_initialized = TRUE;
    }

    char *path = settings_filepath();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        if (g_key_file_has_key(kf, "tag_split", "artists_enabled", NULL))
            g_settings.artists.enabled = g_key_file_get_boolean(kf, "tag_split", "artists_enabled", NULL);
        if (g_key_file_has_key(kf, "tag_split", "genres_enabled", NULL))
            g_settings.genres.enabled = g_key_file_get_boolean(kf, "tag_split", "genres_enabled", NULL);
        load_separators(kf, "artists_separators", &g_settings.artists);
        load_separators(kf, "genres_separators", &g_settings.genres);
    }
    g_key_file_free(kf);
    g_free(path);

    settings_rebuild();
}

static void save_separators(GKeyFile *kf, const char *key, const TagSplitCategory *c) {
    guint n = c->separators ? c->separators->len : 0;
    const char **list = g_new0(const char *, n + 1);
    for (guint i = 0; i < n; i++)
        list[i] = g_ptr_array_index(c->separators, i);
    g_key_file_set_string_list(kf, "tag_split", key, list, n);
    g_free(list);
}

void settings_save(void) {
    if (!g_initialized) return;

    GKeyFile *kf = g_key_file_new();
    g_key_file_set_boolean(kf, "tag_split", "artists_enabled", g_settings.artists.enabled);
    g_key_file_set_boolean(kf, "tag_split", "genres_enabled", g_settings.genres.enabled);
    save_separators(kf, "artists_separators", &g_settings.artists);
    save_separators(kf, "genres_separators", &g_settings.genres);

    char *path = settings_filepath();
    g_key_file_save_to_file(kf, path, NULL);
    g_free(path);
    g_key_file_free(kf);
}

TagSplitSettings *tag_split_settings(void) {
    if (!g_initialized) settings_load();
    return &g_settings;
}

GRegex *tag_split_regex(gboolean artists) {
    if (!g_initialized) settings_load();
    return artists ? g_regex_artists : g_regex_genres;
}

GPtrArray *tag_split_separators(gboolean artists) {
    if (!g_initialized) settings_load();
    return category_for(artists)->separators;
}

void tag_split_add_separator(gboolean artists, const char *separator) {
    if (!separator || !*separator) return;
    TagSplitCategory *c = category_for(artists);
    for (guint i = 0; i < c->separators->len; i++) {
        if (g_strcmp0(g_ptr_array_index(c->separators, i), separator) == 0) return;
    }
    g_ptr_array_add(c->separators, g_strdup(separator));
}

void tag_split_remove_separator(gboolean artists, const char *separator) {
    TagSplitCategory *c = category_for(artists);
    for (guint i = 0; i < c->separators->len; i++) {
        if (g_strcmp0(g_ptr_array_index(c->separators, i), separator) == 0) {
            g_ptr_array_remove_index(c->separators, i);
            return;
        }
    }
}
