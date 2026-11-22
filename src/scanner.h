#pragma once
#include <gio/gio.h>

/* Запускает асинхронное сканирование папки Музыка в фоновом потоке */
void       scanner_scan_async(GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data);

/* Забирает готовый GPtrArray* (с элементами Track*) по завершении */
GPtrArray *scanner_scan_finish(GAsyncResult *result, GError **error);
