#pragma once
#include "player.h"
#include "track_menu.h"
#include <adwaita.h>

GtkWidget *ui_create_scrolled_list(GtkListBox **out_list);
GtkWidget *ui_create_plain_row(const char *title, const char *subtitle);

/* Обёртка вокруг одного дочернего виджета, которая не задаёт минимальную ширину
 * содержимого (натуральную ширину сохраняет). Нужна, чтобы AdwBottomSheet не
 * требовал ширины больше, чем есть в окне: боковые панели внутри содержимого
 * складывают свои минимумы, и на узком окне шторка упирается в предел. */
GtkWidget *ui_create_min_width_bin(GtkWidget *child);

/* Общий приём «не задавать минимальную ширину»: измеряет ребёнка и обнуляет
 * минимальную ширину по горизонтали (натуральную сохраняет). Используется
 * обёртками, которые не должны раздувать минимум AdwBottomSheet. */
void       ui_measure_min_width(GtkWidget *child, GtkOrientation orientation, int for_size,
                                int *minimum, int *natural,
                                int *minimum_baseline, int *natural_baseline);

/* Строка навигации для боковой панели: иконка + подпись. page_id сохраняется в
 * данных строки и используется при переключении раздела. */
GtkWidget *ui_create_sidebar_row(const char *page_id, const char *title, const char *icon_name);

/* Содержимое строки боковой панели (иконка + подпись) без самой строки:
 * используется и для навигационных строк, и для кнопки создания плейлиста. */
GtkWidget *ui_create_sidebar_row_content(const char *title, const char *icon_name);

/* Подзаголовок раздела боковой панели («Медиатека», «Треки», «Плейлисты»).
 * Строка не выбирается и не активируется; suffix (может быть NULL) добавляется
 * справа — так у подзаголовка «Плейлисты» живёт кнопка создания. */
GtkWidget *ui_create_sidebar_section_header(const char *title, GtkWidget *suffix);
void       ui_clear_list_box(GtkListBox *list);
void       ui_set_row_playing(AdwActionRow *row, gboolean is_playing);

GtkWidget *ui_create_content_header(AdwOverlaySplitView *split_view);
GtkWidget *ui_create_cover_widget(const char *cover_path, int size);
/* То же, но с обрезкой по кругу (обложки исполнителей). */
GtkWidget *ui_create_round_cover_widget(const char *cover_path, int size);
/* Вешает на произвольный контейнер с обложкой стиль круглой обрезки. */
void       ui_make_cover_round(GtkWidget *widget);

/* Обложка, заполняющая выделенное место (для полноэкранного плеера):
 * GtkPicture с сохранением пропорций, растягивается по ширине/высоте. */
GtkWidget *ui_create_cover_picture(const char *cover_path, int size);

/* Синхронно декодирует уменьшенную обложку в текстуру (для размытого фона).
 * Возвращает NULL, если обложки нет или её не удалось прочитать. */
GdkTexture *ui_create_cover_texture(const char *cover_path, int size);

/* Доминирующий цвет обложки (для цветного фона страницы альбома). Возвращает
 * FALSE, если обложку не удалось прочитать. */
gboolean   ui_cover_dominant_color(const char *cover_path, GdkRGBA *out);

/* Удерживает ссылку на исходный массив треков, пока жив виджет (список подстраницы).
 * Иначе после пересканирования библиотеки подстраница остаётся с висячими указателями. */
void       ui_pin_tracks_reference(GtkWidget *widget, GPtrArray *tracks);

gint       ui_compare_tracks_by_number(gconstpointer a, gconstpointer b);

/* Размер обложки-миниатюры слева от названия трека в строке списка. */
#define UI_ROW_COVER_SIZE 32

void       ui_highlight_playing_in_list(GtkListBox *list, const char *playing_uri);

/* Обновляет подсветку в списках, зарегистрированных на странице под ключом
 * "playing-listboxes" (GPtrArray<GtkListBox*>). Используется разделами, которые
 * открывают страницы альбома/исполнителя внутри своего стека навигации. */
void       ui_highlight_playing_on_page(GtkWidget *page, const char *playing_uri);
