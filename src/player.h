#pragma once
#include "track.h"
#include <glib.h>

typedef struct _Player Player;
typedef void (*PlayerStateChangedCb)(Player *player, gpointer user_data);

/* Режим повтора: выкл (по умолчанию) / вся очередь / один трек. */
typedef enum {
    PLAYER_REPEAT_OFF = 0, /* конец очереди — пауза и переход на первый трек */
    PLAYER_REPEAT_ALL,     /* конец очереди — переход на первый трек и игра */
    PLAYER_REPEAT_ONE,     /* повтор текущего трека */
} PlayerRepeatMode;

Player    *player_new(void);
void       player_free(Player *player);

void       player_set_state_changed_cb(Player *player, PlayerStateChangedCb cb, gpointer user_data);
void       player_set_queue_and_play(Player *player, GPtrArray *tracks, int start_index);
/* Восстанавливает очередь и текущий трек, не начиная воспроизведение: трек не
 * предзагружается, а загрузится лениво по кнопке «играть». current_index = -1 —
 * без текущего трека. */
void       player_restore_queue(Player *player, GPtrArray *tracks, int current_index);
void       player_add_to_queue(Player *player, Track *track, gboolean play_now);
/* Вставляет трек сразу после текущего («играть далее»); в конец, если очередь пуста. */
void       player_insert_next(Player *player, Track *track);
/* Вставляет копию трека в очередь сразу после него самого. */
void       player_duplicate_in_queue(Player *player, Track *track);
/* Удаляет элемент очереди (сравнение по указателю на объект трека). */
void       player_remove_from_queue(Player *player, Track *track);
/* Полностью очищает очередь и останавливает воспроизведение. */
void       player_clear_queue(Player *player);
/* Перемещает элемент очереди из позиции from в позицию to (перетаскивание). */
void       player_move_in_queue(Player *player, int from, int to);
void       player_play_index(Player *player, int index);
void       player_toggle_pause(Player *player);
void       player_next(Player *player);
void       player_prev(Player *player);

/* Перемешивание: очередь сохраняется, меняется только порядок воспроизведения. */
void       player_set_shuffle(Player *player, gboolean enabled);
gboolean   player_get_shuffle(Player *player);
void       player_set_repeat(Player *player, PlayerRepeatMode mode);
PlayerRepeatMode player_get_repeat(Player *player);

gint64     player_get_position(Player *player);
gint64     player_get_duration(Player *player);
void       player_seek(Player *player, gint64 position_ns);

double     player_get_volume(Player *player);
void       player_set_volume(Player *player, double volume);

GPtrArray *player_get_queue(Player *player);/* Растёт при каждом изменении состава очереди (не при смене текущего трека).
 * Позволяет UI перестраивать список очереди только когда это действительно нужно. */
guint      player_get_queue_version(Player *player);
int        player_get_current_index(Player *player);
Track     *player_get_current_track(Player *player);
gboolean   player_is_playing(Player *player);
