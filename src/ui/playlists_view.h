#pragma once
#include "player.h"
#include "playlist.h"
#include <adwaita.h>

typedef struct _PlaylistsView PlaylistsView;

/* Создаёт раздел плейлистов: добавляет свои строки в боковую панель и страницу
 * в стек контента (имя страницы — "playlists"). */
PlaylistsView *playlists_view_new(Player *player,
                                  AdwOverlaySplitView *split_view,
                                  GtkListBox *sidebar_list,
                                  GtkStack *content_stack);

/* Обновляет ссылку на медиатеку (для сопоставления записей плейлистов). */
void playlists_view_set_tracks(PlaylistsView *pv, GPtrArray *tracks);

/* Открывает плейлист в области контента. */
void playlists_view_show(PlaylistsView *pv, const char *path);

/* Пересканирует плейлисты (например, после создания нового). Если select_path
 * задан — выделяет соответствующий пункт в боковой панели. */
void playlists_view_reload(PlaylistsView *pv, const char *select_path);

gboolean    playlists_view_is_favorite(PlaylistsView *pv, const char *uri);
void        playlists_view_set_favorite(PlaylistsView *pv, const char *uri, gboolean favorite);
GPtrArray  *playlists_view_get_playlists(PlaylistsView *pv); /* заимствованный массив */
void        playlists_view_add_uri(PlaylistsView *pv, const char *path, const char *uri);

/* Обновляет подсветку играющего трека в списке текущего плейлиста. */
void        playlists_view_refresh_playing(PlaylistsView *pv, const char *playing_uri);

/* Открывает альбом внутри стека раздела плейлистов. */
void        playlists_view_open_album(PlaylistsView *pv, const char *album, const char *album_artist);

/* Диалог создания нового плейлиста. */
void playlists_view_create_dialog(PlaylistsView *pv, GtkWidget *parent);

/* Диалог переименования плейлиста по его пути (parent — окно/виджет для
 * показа). Для «Любимых треков» не показывается. */
void playlists_view_rename_dialog(PlaylistsView *pv, const char *path, GtkWidget *parent);

/* Диалог подтверждения удаления плейлиста. Для «Любимых треков» не
 * показывается. */
void playlists_view_delete_dialog(PlaylistsView *pv, const char *path, GtkWidget *parent);

void playlists_view_free(PlaylistsView *pv);
