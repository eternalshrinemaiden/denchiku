#include "track.h"
#include "settings.h"

/* Общий сплиттер тега: режет строку по скомпилированному выражению re,
 * обрезает пробелы и отбрасывает пустые куски. Если re == NULL (разбиение
 * выключено в настройках), тег возвращается как единственный элемент.
 * Возвращает NULL-терминированный массив строк (g_strfreev) либо NULL, если
 * тег пуст. */
static char **split_by_regex(const char *tag, GRegex *re) {
    if (!tag || !*tag) return NULL;

    if (!re) {
        char *trimmed = g_strstrip(g_strdup(tag));
        if (!*trimmed) {
            g_free(trimmed);
            return NULL;
        }
        char **one = g_new0(char *, 2);
        one[0] = trimmed;
        return one;
    }

    char **parts = g_regex_split(re, tag, 0);
    GPtrArray *out = g_ptr_array_new();
    for (guint i = 0; parts[i] != NULL; i++) {
        char *trimmed = g_strstrip(g_strdup(parts[i]));
        if (*trimmed)
            g_ptr_array_add(out, trimmed);
        else
            g_free(trimmed);
    }
    g_strfreev(parts);

    if (out->len == 0) {
        g_ptr_array_free(out, TRUE);
        return NULL;
    }
    g_ptr_array_add(out, NULL);
    return (char **)g_ptr_array_free(out, FALSE);
}

char **track_split_artists(const char *artist_tag) {
    return split_by_regex(artist_tag, tag_split_regex(TRUE));
}

char **track_split_genres(const char *genre_tag) {
    return split_by_regex(genre_tag, tag_split_regex(FALSE));
}

Track *track_new(const char *uri, const char *title, const char *artist,
                 const char *album, const char *album_artist, const char *genre,
                 guint track_number, guint disc_number, const char *disc_subtitle,
                 guint year, const char *cover_path, guint duration) {
    Track *t = g_new0(Track, 1);
    t->uri           = g_strdup(uri);
    t->title         = g_strdup(title);
    t->artist        = g_strdup(artist);
    t->album         = g_strdup(album);
    t->album_artist  = g_strdup(album_artist);
    t->genre         = g_strdup(genre);
    t->track_number  = track_number;
    t->disc_number   = disc_number;
    t->disc_subtitle = g_strdup(disc_subtitle);
    t->year          = year;
    t->cover_path    = g_strdup(cover_path);
    t->duration      = duration;
    return t;
}

Track *track_copy(const Track *t) {
    if (!t) return NULL;
    return track_new(t->uri, t->title, t->artist, t->album, t->album_artist,
                     t->genre, t->track_number, t->disc_number, t->disc_subtitle,
                     t->year, t->cover_path, t->duration);
}

void track_free(Track *t) {
    if (!t) return;
    g_free(t->uri);
    g_free(t->title);
    g_free(t->artist);
    g_free(t->album);
    g_free(t->album_artist);
    g_free(t->genre);
    g_free(t->disc_subtitle);
    g_free(t->cover_path);
    g_free(t);
}
