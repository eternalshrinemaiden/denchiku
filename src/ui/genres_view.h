#pragma once
#include "player.h"
#include <adwaita.h>

typedef struct _GenresView GenresView;

GenresView *genres_view_new(Player *player, AdwOverlaySplitView *split_view);
GtkWidget  *genres_view_get_widget(GenresView *gv);
void        genres_view_set_tracks(GenresView *gv, GPtrArray *tracks);
/* Делает жанр активным фильтром (заменяя прежний выбор) и возвращает раздел на
 * корневую страницу. Используется ссылкой на жанр со страницы альбома. */
void        genres_view_select_genre(GenresView *gv, const char *genre);
/* Открывает альбом внутри стека раздела (страница жанра остаётся в истории). */
void        genres_view_open_album(GenresView *gv, const char *album, const char *album_artist);
/* Открывает страницу исполнителя внутри стека раздела жанров, не переключая
 * вкладку. */
void        genres_view_open_artist(GenresView *gv, const char *artist);
void        genres_view_refresh_playing(GenresView *gv, const char *playing_uri);
void        genres_view_free(GenresView *gv);
