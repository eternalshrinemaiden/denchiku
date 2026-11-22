#pragma once
#include "player.h"
#include "track.h"
#include "track_menu.h"
#include <adwaita.h>

/* Виртуализированный список треков на GtkListView.
 *
 * На больших медиатеках (тысячи треков) построение строк на каждый трек
 * (GtkListBox + AdwActionRow) съедало память и подтормаживало интерфейс. GtkListView
 * создаёт строки только для видимой области и переиспользует их при прокрутке, а
 * модель хранит лишь лёгкие обёртки над треками (сами Track остаются общими и
 * принадлежат источнику — список лишь держит ссылку на массив). */
typedef struct _TrackList TrackList;

typedef struct {
    TrackContext context;          /* контекст для меню трека (см. TrackContext) */
    gboolean subtitle_artist_only; /* подзаголовок — только исполнитель (иначе «исполнитель • альбом») */
    gboolean reorderable;          /* включить перетаскивание для смены порядка */

    /* Активация строки. Если не задан — запускается весь список как очередь с
     * выбранного трека (player_set_queue_and_play). position — индекс в списке. */
    void (*on_activate)(TrackList *tl, guint position, gpointer user_data);
    /* Смена порядка (вызывается только при reorderable): from/to — индексы списка. */
    void (*on_reorder)(TrackList *tl, int from, int to, gpointer user_data);
    gpointer user_data;
} TrackListConfig;

TrackList *track_list_new(Player *player, const TrackListConfig *config);

GtkWidget *track_list_get_widget(TrackList *tl);

/* Задаёт показываемые треки. Массив заимствуется (его элементы — общие Track);
 * список удерживает ссылку на массив, чтобы подстраницы не держали висячие
 * указатели после пересканирования. NULL очищает список. */
void track_list_set_tracks(TrackList *tl, GPtrArray *tracks);

/* Подсвечивает играющий трек по uri (NULL — снять подсветку). Обновляет только
 * изменившиеся строки, а не весь список. */
void track_list_set_playing_uri(TrackList *tl, const char *uri);

/* То же по индексу в списке: нужно для очереди, где один и тот же трек может
 * встречаться несколько раз. */
void track_list_set_playing_index(TrackList *tl, int index);
