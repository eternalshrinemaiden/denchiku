#pragma once
#include <glib.h>

/* Загружает треки из ~/.cache/keruu/library.cache. Возвращает NULL, если кэша нет */
GPtrArray *cache_load_tracks(void);

/* Сохраняет массив Track* в кэш */
void       cache_save_tracks(GPtrArray *tracks);
