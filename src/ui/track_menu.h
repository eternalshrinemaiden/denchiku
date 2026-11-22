#pragma once
#include "player.h"
#include "playlist.h"
#include "track.h"
#include <adwaita.h>

/* Контекст, в котором трек показан пользователю. От него зависит состав меню:
 * например, на странице самого альбома пункт «Открыть альбом» не нужен.
 * Объекты медиатеки разделяются между списками (один и тот же Track виден и во
 * вкладке «Треки», и на странице альбома), поэтому контекст хранится не в самом
 * Track, а в строке-представлении трека и передаётся из неё в меню. */
typedef enum {
    TRACK_CONTEXT_TRACKS,     /* вкладка «Треки» */
    TRACK_CONTEXT_ALBUM,      /* страница альбома */
    TRACK_CONTEXT_ARTIST,     /* треки исполнителя */
    TRACK_CONTEXT_GENRE,      /* треки жанра */
    TRACK_CONTEXT_QUEUE,      /* панель очереди */
    TRACK_CONTEXT_PLAYER_BAR, /* плеер-бар */
} TrackContext;

/* Обработчик пункта «Открыть альбом». Регистрируется окном. */
typedef void (*TrackOpenAlbumFn)(const char *album, const char *album_artist,
                                 gpointer user_data);
void track_menu_set_open_album_cb(TrackOpenAlbumFn cb, gpointer user_data);

/* Обработчик пункта «Перейти к исполнителю». Регистрируется окном: раздел с
 * страницами исполнителей — «Исполнители», поэтому окно переключает вкладку. */
typedef void (*TrackOpenArtistFn)(const char *artist, gpointer user_data);
void track_menu_set_open_artist_cb(TrackOpenArtistFn cb, gpointer user_data);

/* Плейлисты. Колбэки регистрируются окном (у него есть доступ к разделу). */
typedef gboolean   (*TrackIsFavoriteFn)(const char *uri, gpointer user_data);
typedef void       (*TrackToggleFavoriteFn)(const char *uri, gboolean favorite,
                                            gpointer user_data);
/* Заимствованный массив Playlist*, живущий до пересканирования. */
typedef GPtrArray *(*TrackListPlaylistsFn)(gpointer user_data);
typedef void       (*TrackAddToPlaylistFn)(const char *playlist_path, const char *uri,
                                           gpointer user_data);

void track_menu_set_playlist_cbs(TrackIsFavoriteFn is_favorite,
                                 TrackToggleFavoriteFn toggle_favorite,
                                 TrackListPlaylistsFn list_playlists,
                                 TrackAddToPlaylistFn add_to_playlist,
                                 gpointer user_data);

/* Строит всплывающее меню трека с учётом контекста. */
GtkWidget *track_menu_popover_new(Track *track, TrackContext context, Player *player);

/* Стилизованная кнопка «три точки» без привязанного меню. */
GtkWidget *track_menu_button_new(void);

/* Открывает menu_button по правому клику и долгому нажатию основной кнопкой. */
void track_menu_add_triggers(GtkWidget *target, GtkWidget *menu_button);

/* Навешивает меню на строку трека (AdwActionRow): кнопка «…» и сердечко
 * добавляются суффиксами, а строка запоминает свой контекст. */
void track_menu_attach(GtkWidget *row, Track *track, TrackContext context, Player *player);

/* Вариант для виртуализированных списков (GtkListView), где строки — обычные
 * виджеты, а не AdwActionRow (в GtkListView он недопустим), и переиспользуются:
 * меню и сердечко навешиваются один раз при создании строки в контейнер
 * suffix_box, а текущий трек подставляется при каждой привязке через
 * track_menu_row_set_track(). Так на строку не накапливаются новые кнопки. */
void track_menu_attach_row(GtkWidget *row, GtkWidget *suffix_box,
                           TrackContext context, Player *player);

/* Меняет трек, к которому привязаны меню и сердечко строки (NULL — очистить). */
void track_menu_row_set_track(GtkWidget *row, Track *track);

/* Кнопка-«сердечко» для добавления трека в любимые (пустой uri — неактивна). */
GtkWidget *track_favorite_button_new(const char *uri);
/* Меняет трек, к которому привязана кнопка, и обновляет её вид. */
void       track_favorite_button_set_uri(GtkWidget *button, const char *uri);
/* Обновляет все созданные кнопки-«сердечки» по текущему состоянию. */
void       track_menu_refresh_favorites(void);

/* После закрытия popover отвязывает его от кнопки, чтобы при следующем открытии
 * меню строилось заново: состояние «любимых» и список плейлистов могли
 * измениться. Используется вместе с gtk_menu_button_set_create_popup_func(). */
void track_menu_detach_on_close(GtkWidget *popover, GtkWidget *button);
