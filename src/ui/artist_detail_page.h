#pragma once
#include "player.h"
#include <adwaita.h>

/* Открытие альбома со страницы исполнителя. Обработчик регистрирует artists_view:
 * он открывает страницу альбома внутри своего же стека навигации, чтобы страница
 * исполнителя осталась в истории (иначе пришлось бы переключать раздел). */
typedef void (*ArtistOpenAlbumFn)(const char *album, const char *album_artist,
                                  gpointer user_data);

/* Страница исполнителя в духе Spotify: шапка с круглой обложкой, «Популярное»
 * (до пяти треков), «Альбомы» (где исполнитель указан как исполнитель альбома),
 * «Синглы» (треки без альбома) и «С участием» (альбомы других исполнителей, где
 * он отмечен как исполнитель трека).
 *
 * artist_tracks — треки, в которых исполнитель указан (владеется вызывающим;
 * страница держит ссылку через ui_pin_tracks_reference). Возвращает NULL, если
 * треков нет.
 *
 * Для подсветки играющего трека в данных страницы лежит массив "playing-lists"
 * (GPtrArray<GtkListBox*>): вызывающий обновляет по нему списки. */
AdwNavigationPage *artist_detail_page_create(Player *player,
                                             GPtrArray *artist_tracks,
                                             GPtrArray *all_tracks,
                                             const char *artist,
                                             ArtistOpenAlbumFn on_open_album,
                                             gpointer user_data);

/* Открывает страницу исполнителя в переданном стеке навигации, НЕ переключая
 * раздел: треки исполнителя собираются из all_tracks (с учётом разбиения тегов,
 * как в разделе «Исполнители»), а on_open_album должен открывать альбом в том же
 * стеке. Так страницу исполнителя можно показать из любого раздела
 * (альбомы/жанры/треки/плейлисты), не уводя пользователя во вкладку
 * «Исполнители». */
void artist_detail_page_open(AdwNavigationView *nav, Player *player,
                             GPtrArray *all_tracks, const char *artist,
                             ArtistOpenAlbumFn on_open_album, gpointer user_data);
