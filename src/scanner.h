#pragma once
#include <gio/gio.h>
#include <gst/pbutils/pbutils.h>
#include "track.h"

/* Запускает асинхронное инкрементальное сканирование папки Музыка в фоновом
 * потоке: теги перечитываются только у новых или изменённых файлов (по mtime и
 * размеру), остальные берутся из кэша. */
void       scanner_scan_async(GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data);

/* Забирает готовый GPtrArray* (с элементами Track*) по завершении */
GPtrArray *scanner_scan_finish(GAsyncResult *result, GError **error);

/* Расширение файла относится к поддерживаемым аудиоформатам. */
gboolean   scanner_is_audio_file(const char *filename);

/* Извлекает метаданные одного файла синхронным API GStreamer и заполняет uri,
 * mtime и size. discoverer можно переиспользовать в пределах одного потока.
 * Возвращает NULL при ошибке чтения. */
Track     *scanner_discover_file(GstDiscoverer *discoverer, const char *filepath);
