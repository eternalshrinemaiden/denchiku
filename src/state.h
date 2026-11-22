#pragma once
#include <glib.h>

/* Состояние сессии: размер/развёрнутость окна, режимы воспроизведения,
 * громкость, очередь с текущим треком и вид браузера (размер элементов и
 * сортировка). Хранится в ~/.config/keruu/state.ini. */

typedef struct {
    /* Окно */
    int      width;
    int      height;
    gboolean maximized;

    /* Плеер */
    gboolean shuffle;
    int      repeat;      /* PlayerRepeatMode */
    double   volume;      /* 0.0 .. 1.0 */

    /* Очередь */
    char   **queue_uris;  /* NULL-терминированный массив URI */
    int      queue_len;
    char    *current_uri; /* URI текущего трека или NULL */
    gboolean queue_visible;

    /* Вид браузера */
    int      view_mode;      /* ViewMode */
    int      view_zoom;      /* ViewZoom */
    int      view_sort;      /* ViewSortField */
    gboolean view_sort_desc;
} AppState;

/* Загружает состояние с диска. Никогда не возвращает NULL: при отсутствии файла
 * или ошибке возвращаются значения по умолчанию. */
AppState *state_load(void);

/* Сохраняет состояние на диск (best-effort). */
void state_save(const AppState *state);

void state_free(AppState *state);
