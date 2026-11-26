#pragma once
#include <glib.h>
#include "track.h"

/* Кэш медиатеки.
 *
 * В памяти — единственная каноническая копия треков: таблица uri -> Track*
 * (треки разделяемые, со счётчиком ссылок). GKeyFile строится только
 * транзиентно при загрузке и записи на диск (группа на трек — [track:<uri>]),
 * чтобы не держать всю медиатеку второй раз в тяжёлом формате GLib.
 *
 * Наружу кэш — единое потокобезопасное хранилище: правки применяются в памяти
 * мгновенно (для быстрого UI), а на диск пишутся отложенно (debounce) и
 * атомарно в фоновом потоке. Это избавляет от переписывания всего INI-файла на
 * каждое файловое событие.
 *
 * Возвращаемые снимки (cache_load_tracks / cache_snapshot_tracks) и
 * cache_lookup_track отдают НОВЫЕ ссылки на те же объекты Track: массив-снимок
 * освобождает элементы через track_unref, а трек живёт, пока на него ссылается
 * кэш или чей-либо список. */

/* Полный снимок медиатеки (GPtrArray<Track*> со ссылками; освобождать через
 * g_ptr_array_unref — элементы разыменуются через track_unref). NULL, если кэш
 * пуст/отсутствует. */
GPtrArray *cache_load_tracks(void);

/* Полностью заменяет содержимое кэша набором треков. */
void       cache_save_tracks(GPtrArray *tracks);

/* ===== инкрементальные операции (потокобезопасны) ===== */

/* Добавляет/обновляет трек (по URI) вместе с mtime/size и планирует запись. */
void       cache_put_track(const Track *track);

/* Удаляет трек по URI. */
void       cache_remove_uri(const char *uri);

/* Переносит запись при переименовании файла без перечитывания тегов.
 * Возвращает FALSE, если старой записи не было. */
gboolean   cache_rename_uri(const char *old_uri, const char *new_uri,
                            gint64 mtime, guint64 size);

/* Возвращает сохранённые (mtime,size) для URI; FALSE, если записи нет. */
gboolean   cache_lookup_meta(const char *uri, gint64 *mtime, guint64 *size);

/* Возвращает НОВУЮ ссылку на трек из кэша по URI (освобождать через
 * track_unref) или NULL. */
Track     *cache_lookup_track(const char *uri);

/* Удаляет из кэша все записи, чьих URI нет в наборе present (GHashTable строк). */
void       cache_prune_missing(GHashTable *present_uris);

/* Снимок содержимого кэша (GPtrArray<Track*> со ссылками); безопасно вызывать
 * из фонового потока — используется для доставки обновления в UI. */
GPtrArray *cache_snapshot_tracks(void);

/* Планирует отложенную запись кэша на диск (debounce). */
void       cache_schedule_flush(void);

/* Немедленно и атомарно сохраняет кэш (например, при выходе). */
void       cache_flush(void);
