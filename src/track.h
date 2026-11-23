#pragma once
#include <glib.h>

typedef struct {
    char *uri;
    char *title;
    char *artist;
    char *album;
    char *album_artist;
    char *genre;
    guint track_number;
    guint disc_number;
    char *disc_subtitle;
    guint year;
    char *cover_path;
    guint duration; /* длительность трека в секундах (0 — неизвестна) */
} Track;

Track *track_new(const char *uri, const char *title, const char *artist,
                 const char *album, const char *album_artist, const char *genre,
                 guint track_number, guint disc_number, const char *disc_subtitle,
                 guint year, const char *cover_path, guint duration);
/* Полная копия трека: элементы очереди хранят собственные копии, потому что
 * объекты медиатеки разделяются между несколькими списками одновременно. */
Track *track_copy(const Track *track);
void   track_free(Track *track);

/* Разбивает тег исполнителя (artist или album_artist) на отдельных
 * исполнителей по разделителям: запятая, точка с запятой, « & », а также
 * « feat. », « ft. », « featuring », « vs. », « versus » (регистр не важен).
 * Возвращает NULL-терминированный массив строк, который нужно освобождать
 * через g_strfreev(), либо NULL, если тег пуст. */
char **track_split_artists(const char *artist_tag);

/* То же для жанров: разбивает тег жанра по разделителям (запятая, точка с
 * запятой, слэш, « & »). Возвращает NULL-терминированный массив строк
 * (g_strfreev) или NULL, если тег пуст. */
char **track_split_genres(const char *genre_tag);
