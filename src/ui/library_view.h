#pragma once
#include "player.h"
#include "view_controls.h"
#include <adwaita.h>

typedef struct _LibraryView LibraryView;

LibraryView *library_view_new(Player *player, AdwOverlaySplitView *split_view);
void         library_view_attach_to_stack(LibraryView *lv, GtkStack *stack);
void         library_view_set_tracks(LibraryView *lv, GPtrArray *tracks);
/* Перестраивает только группировку по исполнителям (после смены настроек
 * tag-splitting), не трогая остальные списки. */
void         library_view_regroup_artists(LibraryView *lv, GPtrArray *tracks);
/* Перестраивает только группировку по жанрам. */
void         library_view_regroup_genres(LibraryView *lv, GPtrArray *tracks);
void         library_view_refresh_playing(LibraryView *lv);
/* Открывает страницу альбома (пункт меню «Открыть альбом») в заданном разделе,
 * чтобы не переключать вкладку: section — имя видимого раздела content_stack. */
void         library_view_open_album(LibraryView *lv, const char *section,
                                      const char *album, const char *album_artist);
/* Открывает страницу исполнителя (пункт меню «Перейти к исполнителю»); окно
 * перед этим переключает раздел на «Исполнители». */
void         library_view_open_artist(LibraryView *lv, const char *artist);
/* Элементы управления видом браузера альбомов (размер элементов, сортировка). */
ViewControls *library_view_get_view_controls(LibraryView *lv);
void         library_view_free(LibraryView *lv);
