#pragma once
#include <glib.h>

/* Слежение за папкой медиатеки в реальном времени.
 *
 * Watcher рекурсивно вешает GFileMonitor (inotify) на все подпапки, дебаунсит
 * события, асинхронно перечитывает теги изменившихся/новых файлов и отдаёт в UI
 * единый батч-снимок медиатеки. Мониторы и парсинг живут в фоновых потоках и не
 * блокируют главный цикл и аудиопоток. */

typedef struct _Watcher Watcher;

/* Доставка обновления в UI. Вызывается в главном потоке со снимком медиатеки
 * (GPtrArray<Track*>); владение снимком остаётся за watcher, поэтому вызывающий
 * должен сделать ref, если собирается хранить его дольше колбэка. */
typedef void (*WatcherChangedFunc)(GPtrArray *tracks, gpointer user_data);

Watcher *watcher_new(void);
void     watcher_set_changed_cb(Watcher *w, WatcherChangedFunc cb, gpointer user_data);

/* Запускает рекурсивное слежение за корнем медиатеки. */
void     watcher_start(Watcher *w, const char *root);

void     watcher_free(Watcher *w);
