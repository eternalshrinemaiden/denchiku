#pragma once
#include "player.h"
#include <adwaita.h>

typedef struct _PlayerBar PlayerBar;

typedef enum {
    PLAYER_BAR_VARIANT_BAR,        /* нижний бар: 3 колонки (широко) / компакт (узко) */
    PLAYER_BAR_VARIANT_FULLSCREEN, /* полноэкранный «сейчас играет»: большая обложка */
} PlayerBarVariant;

PlayerBar *player_bar_new(Player *player, PlayerBarVariant variant);
GtkWidget *player_bar_get_widget(PlayerBar *pb);
/* Кнопка-переключатель очереди (правее ползунка громкости). В баре управляет
 * боковой панелью окна, в развёрнутом плеере — встроенной панелью очереди. */
GtkWidget *player_bar_get_queue_button(PlayerBar *pb);
/* Закрывает встроенную панель очереди развёрнутого плеера. */
void       player_bar_close_queue(PlayerBar *pb);
void       player_bar_refresh(PlayerBar *pb);
void       player_bar_free(PlayerBar *pb);
