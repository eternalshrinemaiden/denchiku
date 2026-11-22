#pragma once
#include "player.h"
#include <adwaita.h>

/* Открытие другого альбома (карточки секции «другие альбомы»). Регистрируется
 * окном: альбом открывается в текущем разделе. */
typedef void (*AlbumOpenAlbumFn)(const char *album, const char *album_artist,
                                 gpointer user_data);
void album_detail_page_set_open_album_cb(AlbumOpenAlbumFn cb, gpointer user_data);

/* Открытие страницы исполнителя (клик по имени в шапке или по исполнителям
 * секций). Регистрируется окном. */
typedef void (*AlbumOpenArtistFn)(const char *artist, gpointer user_data);
void album_detail_page_set_open_artist_cb(AlbumOpenArtistFn cb, gpointer user_data);

AdwNavigationPage *album_detail_page_create(Player *player,
                                            GPtrArray *all_tracks,
                                            const char *album_name,
                                            const char *album_artist,
                                            GtkListBox **out_track_list);