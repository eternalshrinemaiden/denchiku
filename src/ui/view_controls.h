#pragma once
#include <adwaita.h>

typedef enum {
    VIEW_MODE_GRID,
    VIEW_MODE_LIST
} ViewMode;

typedef enum {
    VIEW_ZOOM_SMALL  = 0,
    VIEW_ZOOM_MEDIUM = 1,
    VIEW_ZOOM_LARGE  = 2
} ViewZoom;

typedef enum {
    VIEW_SORT_TITLE  = 0,
    VIEW_SORT_ARTIST = 1,
    VIEW_SORT_YEAR   = 2
} ViewSortField;

typedef struct _ViewControls ViewControls;

typedef void (*ViewControlsChangedCb)(ViewControls *vc, gpointer user_data);

ViewControls *view_controls_new(gboolean show_year_sort,
                                ViewControlsChangedCb on_changed,
                                gpointer user_data);
GtkWidget    *view_controls_get_widget(ViewControls *vc);

ViewMode      view_controls_get_mode(ViewControls *vc);
ViewZoom      view_controls_get_zoom(ViewControls *vc);
ViewSortField view_controls_get_sort_field(ViewControls *vc);
gboolean      view_controls_get_sort_descending(ViewControls *vc);

/* Программно задать сортировку (например, после клика по столбцу таблицы). */
void          view_controls_set_sort(ViewControls *vc, ViewSortField field, gboolean descending);

/* Программно задать режим отображения и размер элементов. */
void          view_controls_set_mode(ViewControls *vc, ViewMode mode);
void          view_controls_set_zoom(ViewControls *vc, ViewZoom zoom);

/* Вспомогательные геттеры размеров в пикселях в зависимости от зума */
int           view_controls_get_grid_cover_size(ViewControls *vc);
int           view_controls_get_list_cover_size(ViewControls *vc);

void          view_controls_free(ViewControls *vc);
