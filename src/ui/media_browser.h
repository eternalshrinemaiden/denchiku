#pragma once
#include "view_controls.h"
#include <adwaita.h>

typedef struct _MediaBrowser MediaBrowser;

/* Коллбэки для извлечения данных из произвольного GObject-элемента модели */
typedef const char *(*MediaItemTextFn)(GObject *item);
typedef guint       (*MediaItemYearFn)(GObject *item);
typedef void        (*MediaItemActivatedFn)(GObject *item, gpointer user_data);

typedef struct {
    const char *title_col_name;
    const char *subtitle_col_name;
    const char *year_col_name;      /* NULL, если колонка года не нужна */

    MediaItemTextFn get_title;
    MediaItemTextFn get_subtitle;   /* Может быть NULL (например, для жанров) */
    MediaItemTextFn get_cover_path;
    MediaItemYearFn get_year;       /* Может быть NULL */

    /* Обрезать обложки по кругу (исполнители). */
    gboolean round_covers;
} MediaBrowserConfig;

MediaBrowser *media_browser_new(GListStore *store,
                                ViewControls *controls,
                                const MediaBrowserConfig *config,
                                MediaItemActivatedFn on_activate,
                                gpointer user_data);

GtkWidget    *media_browser_get_widget(MediaBrowser *mb);
void          media_browser_refresh(MediaBrowser *mb);
void          media_browser_free(MediaBrowser *mb);
