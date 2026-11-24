#pragma once
#include "player.h"
#include "track.h"
#include "track_menu.h"
#include "view_controls.h"
#include <adwaita.h>

/* Виртуализированный список треков на GtkColumnView.
 *
 * На больших медиатеках (тысячи треков) построение строк на каждый трек
 * (GtkListBox + AdwActionRow) съедало память и подтормаживало интерфейс.
 * GtkColumnView создаёт строки только для видимой области и переиспользует их при
 * прокрутке, а модель хранит лишь лёгкие обёртки над треками (сами Track
 * остаются общими и принадлежат источнику — список лишь держит ссылку на массив).
 *
 * Есть два режима оформления:
 *  - подробный (по умолчанию): столбцы «Название» (обложка + название),
 *    «Исполнитель», «Альбом», «Длительность» и столбец с кнопками лайка и меню.
 *    Заголовки столбцов дают сортировку по клику;
 *  - компактный (compact): одна колонка без заголовка — обложка, название и
 *    исполнитель в две строки, длительность и те же кнопки. Используется в
 *    панели очереди. */
typedef struct _TrackList TrackList;

typedef struct {
    TrackContext context;      /* контекст для меню трека (см. TrackContext) */
    gboolean compact;          /* компактный режим без столбцов (очередь) */
    gboolean reorderable;      /* включить перетаскивание для смены порядка */
    ViewControls *controls;    /* общий вид (зум/сортировка), может быть NULL */

    /* Активация строки. Если не задан — запускается весь список как очередь в
     * порядке, который виден пользователю, с выбранного трека
     * (player_set_queue_and_play). position — позиция в списке. */
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

/* Применяет текущее состояние панели управления видом (размер элементов и
 * сортировку). Вызывается разделом при изменении ViewControls. */
void track_list_refresh(TrackList *tl);

/* Число треков в видимом (с учётом сортировки) порядке. */
guint track_list_get_visible_count(TrackList *tl);

/* Запускает весь список как очередь в видимом порядке, начиная с позиции.
 * Та же логика, что и при активации строки по умолчанию; вынесена, чтобы
 * разделы могли дополнительно подготовить плеер (например, включить
 * перемешивание) перед запуском. */
void track_list_play_position(TrackList *tl, guint position);
