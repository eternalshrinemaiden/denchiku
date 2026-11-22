#pragma once
#include "track.h"
#include <glib.h>

/* Плейлист — это файл m3u/m3u8 на диске. Список треков хранится как набор
 * file:// URI, что позволяет сопоставлять записи с медиатекой по t->uri. */
typedef struct {
    char    *path;     /* абсолютный путь к файлу плейлиста (он же идентификатор) */
    char    *name;     /* отображаемое имя */
    gboolean favorite; /* специальный плейлист «Любимые треки» */

    /* Ленивый кэш множества URI для быстрых проверок принадлежности.
     * Принадлежит плейлисту, сбрасывается при изменении файла. */
    GHashTable *uri_cache;
} Playlist;

/* Сканирует папку «Музыка» (рекурсивно) и собственную папку данных
 * (~/.local/share/keruu/playlists), гарантируя существование «Любимых треков».
 * Возвращает GPtrArray<Playlist*> (владеет элементами). */
GPtrArray *playlist_scan_all(void);

/* Создаёт новый пустой плейлист в папке данных. Возвращает новый Playlist*. */
Playlist  *playlist_create(const char *name);

Playlist  *playlist_find_favorite(GPtrArray *playlists);
Playlist  *playlist_find_by_path(GPtrArray *playlists, const char *path);

/* Загружает записи плейлиста как file:// URI. Возвращает GPtrArray<char*>. */
GPtrArray *playlist_load_uris(const Playlist *pl);

/* Разрешает записи плейлиста в объекты Track, сопоставляя их с медиатекой по
 * uri. Записи без совпадения превращаются в минимальные воспроизводимые треки.
 * Возвращает GPtrArray<Track*> (владеет элементами). */
GPtrArray *playlist_resolve_tracks(const Playlist *pl, GPtrArray *library);

gboolean   playlist_contains_uri(const Playlist *pl, const char *uri);
gboolean   playlist_add_uri(const Playlist *pl, const char *uri);
gboolean   playlist_remove_uri(const Playlist *pl, const char *uri);

void       playlist_free(Playlist *pl);
