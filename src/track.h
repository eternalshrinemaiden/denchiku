#pragma once
#include <glib.h>

typedef struct {
    /* Счётчик ссылок: один и тот же трек разделяется между кэшем медиатеки,
     * списками UI, очередью плеера и плейлистами, чтобы не держать несколько
     * полных копий метаданных. Атомарный — объекты создаются и удерживаются в
     * разных потоках (сканер, watcher). */
    gint refcount;
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
    /* Паспорт файла на диске: нужен кэшу и инкрементальному обновлению, чтобы
     * перечитывать теги только у новых или изменённых файлов. */
    gint64  mtime;  /* время последней модификации, секунды эпохи (0 — неизвестно) */
    guint64 size;   /* размер файла в байтах */
} Track;

Track *track_new(const char *uri, const char *title, const char *artist,
                 const char *album, const char *album_artist, const char *genre,
                 guint track_number, guint disc_number, const char *disc_subtitle,
                 guint year, const char *cover_path, guint duration);
/* Новая независимая копия трека (счётчик ссылок = 1). Нужна редко: обычно
 * достаточно track_ref() на существующий объект. */
Track *track_copy(const Track *track);
/* Увеличивает счётчик ссылок и возвращает тот же указатель (для chaining). */
Track *track_ref(Track *track);
/* Уменьшает счётчик ссылок; освобождает трек, когда ссылок не осталось. */
void   track_unref(Track *track);

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
