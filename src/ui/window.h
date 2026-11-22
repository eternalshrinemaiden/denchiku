#pragma once
#include "player.h"
#include <adwaita.h>

GtkWidget *main_window_new(AdwApplication *app, Player *player);
void       main_window_set_tracks(GtkWidget *window, GPtrArray *tracks);
void       main_window_set_scanning(GtkWidget *window, gboolean scanning);
/* Восстанавливает очередь и текущий трек из сохранённого состояния. Вызывать
 * после загрузки медиатеки (кэша). */
void       main_window_restore_session(GtkWidget *window, GPtrArray *tracks);
