#pragma once
#include "player.h"
#include "view_controls.h"
#include <adwaita.h>

typedef struct _AlbumsView AlbumsView;

AlbumsView *albums_view_new(Player *player, AdwOverlaySplitView *split_view);
GtkWidget  *albums_view_get_widget(AlbumsView *av);
ViewControls *albums_view_get_controls(AlbumsView *av);
void        albums_view_set_tracks(AlbumsView *av, GPtrArray *tracks);
void        albums_view_refresh_playing(AlbumsView *av, const char *playing_uri);
/* Открывает страницу альбома (используется пунктом меню «Открыть альбом»). */
void        albums_view_open_album(AlbumsView *av, const char *album, const char *album_artist);
void        albums_view_free(AlbumsView *av);
