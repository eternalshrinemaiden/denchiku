#pragma once
#include "player.h"
#include <adwaita.h>

typedef struct _ArtistsView ArtistsView;

ArtistsView *artists_view_new(Player *player, AdwOverlaySplitView *split_view,
                              gboolean liked_only);
GtkWidget   *artists_view_get_widget(ArtistsView *arv);
void         artists_view_set_tracks(ArtistsView *arv, GPtrArray *tracks);
/* Открывает альбом внутри стека раздела (страница исполнителя остаётся в истории). */
void         artists_view_open_album(ArtistsView *arv, const char *album, const char *album_artist);
/* Открывает страницу исполнителя внутри стека раздела (пункт меню «Перейти к
 * исполнителю»). Ничего не делает, если такого исполнителя нет в медиатеке. */
void         artists_view_open_artist(ArtistsView *arv, const char *artist);
void         artists_view_refresh_playing(ArtistsView *arv, const char *playing_uri);
void         artists_view_free(ArtistsView *arv);