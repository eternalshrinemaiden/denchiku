#pragma once
#include "player.h"
#include <adwaita.h>

typedef struct _TracksView TracksView;

TracksView *tracks_view_new(Player *player, AdwOverlaySplitView *split_view);
GtkWidget  *tracks_view_get_widget(TracksView *tv);
void        tracks_view_set_tracks(TracksView *tv, GPtrArray *tracks);
/* Открывает альбом внутри стека раздела «Треки». */
void        tracks_view_open_album(TracksView *tv, const char *album, const char *album_artist);
void        tracks_view_refresh_playing(TracksView *tv, const char *playing_uri);
void        tracks_view_free(TracksView *tv);
