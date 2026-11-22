#pragma once
#include "player.h"
#include <adwaita.h>

typedef struct _QueueView QueueView;

QueueView *queue_view_new(Player *player);
GtkWidget *queue_view_get_widget(QueueView *qv);
/* Полная перестройка строк очереди (состав изменился). */
void       queue_view_refresh(QueueView *qv);
/* Точечное обновление индикатора играющего трека (состав не менялся). */
void       queue_view_update_playing(QueueView *qv);
void       queue_view_free(QueueView *qv);