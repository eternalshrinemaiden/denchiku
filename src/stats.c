#include "stats.h"

#include <glib/gstdio.h>

/* uri -> число прослушиваний (GINT_TO_POINTER). */
static GHashTable *plays;

/* Файл статистики: строки вида "<count>\t<uri>". Свой простой формат (а не
 * GKeyFile), потому что URI может содержать символы, недопустимые в ключах. */
static char *stats_filepath(void) {
    char *dir = g_build_filename(g_get_user_config_dir(), "keruu", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *path = g_build_filename(dir, "plays.tsv", NULL);
    g_free(dir);
    return path;
}

void stats_load(void) {
    if (plays) return;
    plays = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    char *path = stats_filepath();
    char *data = NULL;
    if (g_file_get_contents(path, &data, NULL, NULL)) {
        char **lines = g_strsplit(data, "\n", -1);
        for (guint i = 0; lines[i] != NULL; i++) {
            char *line = lines[i];
            if (!*line) continue;
            char *tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = '\0';
            guint count = (guint)g_ascii_strtoull(line, NULL, 10);
            const char *uri = tab + 1;
            if (*uri && count > 0) {
                g_hash_table_replace(plays, g_strdup(uri), GUINT_TO_POINTER(count));
            }
        }
        g_strfreev(lines);
        g_free(data);
    }
    g_free(path);
}

void stats_save(void) {
    if (!plays) return;

    GString *out = g_string_new(NULL);
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, plays);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        guint count = GPOINTER_TO_UINT(value);
        if (count == 0) continue;
        g_string_append_printf(out, "%u\t%s\n", count, (const char *)key);
    }

    char *path = stats_filepath();
    g_file_set_contents(path, out->str, out->len, NULL);
    g_free(path);
    g_string_free(out, TRUE);
}

void stats_on_play(const char *uri) {
    if (!plays || !uri || !*uri) return;
    guint count = GPOINTER_TO_UINT(g_hash_table_lookup(plays, uri));
    g_hash_table_replace(plays, g_strdup(uri), GUINT_TO_POINTER(count + 1));
}

guint stats_get_plays(const char *uri) {
    if (!plays || !uri) return 0;
    return GPOINTER_TO_UINT(g_hash_table_lookup(plays, uri));
}
