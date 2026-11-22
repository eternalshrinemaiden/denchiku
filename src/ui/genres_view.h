#pragma once
#include "player.h"
#include <adwaita.h>

typedef struct _GenresView GenresView;

GenresView *genres_view_new(Player *player, AdwOverlaySplitView *split_view);
GtkWidget  *genres_view_get_widget(GenresView *gv);
void        genres_view_set_tracks(GenresView *gv, GPtrArray *tracks);
/* Открывает альбом внутри стека раздела (страница жанра остаётся в истории). */
void        genres_view_open_album(GenresView *gv, const char *album, const char *album_artist);
void        genres_view_refresh_playing(GenresView *gv, const char *playing_uri);
void        genres_view_free(GenresView *gv);
