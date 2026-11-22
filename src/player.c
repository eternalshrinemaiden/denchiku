#include "player.h"
#include "stats.h"
#include "track.h"
#include <gst/gst.h>

struct _Player {
    GstElement *playbin;
    GPtrArray *queue;      /* канонический порядок очереди; перемешивание его не меняет */
    guint queue_version;   /* растёт при каждом изменении СОСТАВА очереди */
    int current_index;     /* индекс играющего трека в queue */
    gboolean is_playing;

    gboolean shuffle;
    int *order;            /* перестановка индексов queue (порядок воспроизведения) */
    int order_pos;         /* позиция текущего трека в order */
    PlayerRepeatMode repeat;

    guint bus_watch_id;
    /* Бесшовные переходы (gapless): обработчик about-to-finish готовит
     * следующий uri прямо в стриминговом потоке, а состояние и UI обновляются
     * отдельно, уже на главном потоке. */
    guint gapless_idle_id;
    int   gapless_next;

    PlayerStateChangedCb state_changed_cb;
    gpointer state_changed_data;
};

/* ===== порядок воспроизведения (с учётом перемешивания) ===== */

static int play_seq_pos(Player *p) {
    return (p->shuffle && p->order) ? p->order_pos : p->current_index;
}

static int play_seq_index(Player *p, int pos) {
    if (p->shuffle && p->order && pos >= 0 && pos < (int)p->queue->len) {
        return p->order[pos];
    }
    return pos;
}

/* Строит (или сбрасывает) перемешанный порядок. keep_index ставится первым. */
static void shuffle_rebuild(Player *p, int keep_index) {
    int n = (int)p->queue->len;
    g_clear_pointer(&p->order, g_free);
    p->order_pos = 0;
    if (!p->shuffle || n == 0) return;

    p->order = g_new(int, n);
    for (int i = 0; i < n; i++) p->order[i] = i;
    for (int i = n - 1; i > 0; i--) {
        int j = g_random_int_range(0, i + 1);
        int tmp = p->order[i];
        p->order[i] = p->order[j];
        p->order[j] = tmp;
    }
    if (keep_index >= 0 && keep_index < n) {
        for (int i = 0; i < n; i++) {
            if (p->order[i] == keep_index) {
                int tmp = p->order[0];
                p->order[0] = p->order[i];
                p->order[i] = tmp;
                break;
            }
        }
    }
}

static void order_on_append(Player *p, int k) {
    if (!p->shuffle) return;
    int n = (int)p->queue->len;
    p->order = g_renew(int, p->order, n);
    p->order[n - 1] = k;
}

static void order_on_insert(Player *p, int k) {
    if (!p->shuffle || !p->order) return;
    int n = (int)p->queue->len; /* уже после вставки */
    int old = n - 1;
    p->order = g_renew(int, p->order, n);
    int ipos = p->order_pos + 1;
    if (ipos > old) ipos = old;
    if (ipos < 0) ipos = 0;
    for (int i = old; i > ipos; i--) {
        p->order[i] = p->order[i - 1];
    }
    p->order[ipos] = k;
    for (int i = 0; i < n; i++) {
        if (i == ipos) continue;
        if (p->order[i] >= k) p->order[i]++;
    }
}

static void order_on_remove(Player *p, int k) {
    if (!p->shuffle || !p->order) return;
    int n = (int)p->queue->len; /* уже после удаления */
    int old = n + 1;
    int j = -1;
    for (int i = 0; i < old; i++) {
        if (p->order[i] == k) { j = i; break; }
    }
    if (j < 0) return;
    for (int i = j; i < n; i++) {
        p->order[i] = p->order[i + 1];
    }
    for (int i = 0; i < n; i++) {
        if (p->order[i] > k) p->order[i]--;
    }
    if (j < p->order_pos) p->order_pos--;
    if (p->order_pos >= n) p->order_pos = n - 1;
    if (p->order_pos < 0) p->order_pos = 0;
    if (n == 0) {
        g_clear_pointer(&p->order, g_free);
    } else {
        p->order = g_renew(int, p->order, n);
    }
}

static void notify_state_changed(Player *p) {
    if (p->state_changed_cb) {
        p->state_changed_cb(p, p->state_changed_data);
    }
}

static void player_advance(Player *p, gboolean from_eos);

/* Индекс следующего трека в порядке воспроизведения для бесшовного перехода
 * (about-to-finish). -1 — переходить некуда: конец очереди без повтора. */
static int player_next_seq_index(Player *p) {
    int n = (int)p->queue->len;
    if (n == 0 || p->current_index < 0) return -1;
    if (p->repeat == PLAYER_REPEAT_ONE) return p->current_index;
    int pos = play_seq_pos(p);
    if (pos + 1 < n) return play_seq_index(p, pos + 1);
    if (p->repeat == PLAYER_REPEAT_ALL) return play_seq_index(p, 0);
    return -1;
}

/* about-to-finish приходит из стримингового потока, поэтому состояние плеера и
 * интерфейс трогаем только здесь, на главном потоке. */
static gboolean apply_gapless_next(gpointer user_data) {
    Player *p = user_data;
    p->gapless_idle_id = 0;
    int next = p->gapless_next;
    p->gapless_next = -1;
    if (next < 0 || next >= (int)p->queue->len) return G_SOURCE_REMOVE;

    p->current_index = next;
    if (p->shuffle && p->order) {
        for (int i = 0; i < (int)p->queue->len; i++) {
            if (p->order[i] == next) { p->order_pos = i; break; }
        }
    }
    p->is_playing = TRUE;
    Track *t = g_ptr_array_index(p->queue, next);
    stats_on_play(t->uri);
    notify_state_changed(p);
    return G_SOURCE_REMOVE;
}

/* playbin в режиме gapless: uri, выставленный из этого обработчика, playbin
 * проигрывает сразу за текущим без разрыва — именно это убирает паузы между
 * треками. Если следующего трека нет, ничего не ставим: playbin дойдёт до EOS,
 * и конец очереди отработает on_bus_message. */
static void on_about_to_finish(GstElement *playbin, gpointer user_data) {
    Player *p = user_data;
    int next = player_next_seq_index(p);
    if (next < 0) return;

    Track *t = g_ptr_array_index(p->queue, next);
    g_object_set(playbin, "uri", t->uri, NULL);

    p->gapless_next = next;
    if (!p->gapless_idle_id) {
        p->gapless_idle_id = g_idle_add(apply_gapless_next, p);
    }
}

static gboolean on_bus_message(GstBus *bus, GstMessage *msg, gpointer user_data) {
    (void)bus;
    Player *p = user_data;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            player_advance(p, TRUE);
            break;
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *debug = NULL;
            gst_message_parse_error(msg, &err, &debug);
            g_warning("GStreamer error: %s (%s)", err->message, debug ? debug : "");
            g_error_free(err);
            g_free(debug);
            /* При ошибке трека переходим дальше, но не зацикливаемся на повторе. */
            player_advance(p, FALSE);
            break;
        }
        default:
            break;
    }
    return TRUE;
}

Player *player_new(void) {
    Player *p = g_new0(Player, 1);
    p->playbin = gst_element_factory_make("playbin", "player");
    p->queue = g_ptr_array_new_with_free_func((GDestroyNotify)track_free);
    p->current_index = -1;
    p->is_playing = FALSE;

    if (p->playbin) {
        /* Подключение к about-to-finish переводит playbin в режим gapless. */
        g_signal_connect(p->playbin, "about-to-finish", G_CALLBACK(on_about_to_finish), p);
        GstBus *bus = gst_element_get_bus(p->playbin);
        p->bus_watch_id = gst_bus_add_watch(bus, on_bus_message, p);
        gst_object_unref(bus);
    }
    return p;
}

void player_free(Player *p) {
    if (!p) return;
    /* Снимаем подписку на шину до освобождения: иначе источник в главном
     * контексте продолжит вызывать on_bus_message с уже освобождённым p. */
    if (p->bus_watch_id) {
        g_source_remove(p->bus_watch_id);
        p->bus_watch_id = 0;
    }
    if (p->gapless_idle_id) {
        g_source_remove(p->gapless_idle_id);
        p->gapless_idle_id = 0;
    }
    if (p->playbin) {
        gst_element_set_state(p->playbin, GST_STATE_NULL);
        gst_object_unref(p->playbin);
    }
    if (p->queue) {
        g_ptr_array_unref(p->queue);
    }
    g_free(p->order);
    g_free(p);
}

void player_set_state_changed_cb(Player *p, PlayerStateChangedCb cb, gpointer user_data) {
    p->state_changed_cb = cb;
    p->state_changed_data = user_data;
}

void player_set_queue_and_play(Player *p, GPtrArray *tracks, int start_index) {
    g_ptr_array_set_size(p->queue, 0);
    p->queue_version++;
    if (tracks) {
        for (guint i = 0; i < tracks->len; i++) {
            g_ptr_array_add(p->queue, track_copy(g_ptr_array_index(tracks, i)));
        }
    }
    if (p->queue->len > 0 && start_index >= 0 && start_index < (int)p->queue->len) {
        if (p->shuffle) shuffle_rebuild(p, start_index);
        player_play_index(p, start_index);
    } else {
        if (p->playbin) {
            gst_element_set_state(p->playbin, GST_STATE_NULL);
        }
        p->current_index = -1;
        p->is_playing = FALSE;
        if (p->shuffle) shuffle_rebuild(p, -1);
        notify_state_changed(p);
    }
}

void player_restore_queue(Player *p, GPtrArray *tracks, int current_index) {
    g_ptr_array_set_size(p->queue, 0);
    p->queue_version++;
    if (tracks) {
        for (guint i = 0; i < tracks->len; i++) {
            g_ptr_array_add(p->queue, track_copy(g_ptr_array_index(tracks, i)));
        }
    }
    if (p->queue->len > 0 && current_index >= 0 && current_index < (int)p->queue->len) {
        p->current_index = current_index;
        if (p->shuffle && p->order) {
            for (int i = 0; i < (int)p->queue->len; i++) {
                if (p->order[i] == current_index) { p->order_pos = i; break; }
            }
        }
        /* Playbin оставляем в NULL: трек загрузится лениво при первом нажатии
         * «играть». Предзагрузка uri на этапе восстановления приводила к тому,
         * что при последующей смене трека на «чужой» (другой альбом) playbin
         * мог не применить новый uri: на автоматическом переходе (EOS)
         * продолжал звучать восстановленный трек, тогда как плеер-бар показывал
         * уже новый. */
        if (p->playbin) {
            gst_element_set_state(p->playbin, GST_STATE_NULL);
        }
        p->is_playing = FALSE;
        notify_state_changed(p);
    } else {
        if (p->playbin) {
            gst_element_set_state(p->playbin, GST_STATE_NULL);
        }
        p->current_index = -1;
        p->is_playing = FALSE;
        if (p->shuffle) shuffle_rebuild(p, -1);
        notify_state_changed(p);
    }
}

void player_add_to_queue(Player *p, Track *track, gboolean play_now) {
    if (!track) return;
    Track *copy = track_copy(track);
    g_ptr_array_add(p->queue, copy);
    order_on_append(p, (int)p->queue->len - 1);
    p->queue_version++;
    if (play_now || p->current_index == -1) {
        player_play_index(p, (int)p->queue->len - 1);
    } else {
        notify_state_changed(p);
    }
}

/* Вставляет трек сразу после текущего в очереди («играть далее»). Если ничего
 * не играет, трек добавляется в очередь и начинает воспроизводиться. */
void player_insert_next(Player *p, Track *track) {
    if (!track) return;
    Track *copy = track_copy(track);
    if (p->current_index >= 0) {
        int pos = p->current_index + 1;
        if (pos > (int)p->queue->len) pos = (int)p->queue->len;
        g_ptr_array_insert(p->queue, pos, copy);
        order_on_insert(p, pos);
        p->queue_version++;
        notify_state_changed(p);
    } else {
        g_ptr_array_add(p->queue, copy);
        order_on_append(p, (int)p->queue->len - 1);
        p->queue_version++;
        player_play_index(p, (int)p->queue->len - 1);
    }
}

/* Удаляет элемент очереди по указателю на объект трека. Если удаляется
 * играющий трек, начинает играть следующий (или останавливается, если он был
 * последним). */
void player_remove_from_queue(Player *p, Track *track) {
    if (!track) return;

    int index = -1;
    for (guint i = 0; i < p->queue->len; i++) {
        if (g_ptr_array_index(p->queue, i) == track) { index = (int)i; break; }
    }
    if (index < 0) return;

    /* remove_index освобождает элемент (free func = track_free): указатель
     * track после этого недействителен. */
    g_ptr_array_remove_index(p->queue, index);
    order_on_remove(p, index);
    p->queue_version++;

    if (index < p->current_index) {
        p->current_index--;
    } else if (index == p->current_index) {
        int next = -1;
        if (p->shuffle) {
            if (p->order_pos >= 0 && p->order_pos < (int)p->queue->len) {
                next = p->order[p->order_pos];
            }
        } else if (p->current_index < (int)p->queue->len) {
            next = p->current_index;
        }
        if (next >= 0) {
            player_play_index(p, next);
        } else {
            if (p->playbin) gst_element_set_state(p->playbin, GST_STATE_NULL);
            p->current_index = -1;
            p->is_playing = FALSE;
            notify_state_changed(p);
        }
        return;
    }
    notify_state_changed(p);
}

/* Новый индекс элемента при переносе queue[from] в позицию to. */
static int remap_index(int idx, int from, int to) {
    if (idx == from) return to;
    int r = idx;
    if (from < r) r--;
    if (to <= r) r++;
    return r;
}

/* Перемещает элемент очереди. Воспроизведение не прерывается: играющий трек
 * остаётся тем же, обновляются только его индекс и порядок перемешивания. */
void player_move_in_queue(Player *p, int from, int to) {
    int n = (int)p->queue->len;
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;

    /* steal освобождает слот без вызова free func, затем вставляем обратно. */
    gpointer track = g_ptr_array_steal_index(p->queue, from);
    g_ptr_array_insert(p->queue, to, track);

    p->current_index = remap_index(p->current_index, from, to);
    if (p->shuffle && p->order) {
        for (int i = 0; i < n; i++) {
            p->order[i] = remap_index(p->order[i], from, to);
        }
    }
    p->queue_version++;
    notify_state_changed(p);
}

/* Запускает трек по индексу queue; playing=FALSE оставляет его на паузе
 * (используется в конце очереди при выключенном повторе). count_play=FALSE —
 * не засчитывать прослушивание (возобновление уже игравшего трека после
 * восстановления сессии). */
static void start_track(Player *p, int index, gboolean playing, gboolean count_play) {
    if (!p->playbin || index < 0 || index >= (int)p->queue->len) return;

    Track *t = g_ptr_array_index(p->queue, index);
    p->current_index = index;

    if (p->shuffle && p->order) {
        for (int i = 0; i < (int)p->queue->len; i++) {
            if (p->order[i] == index) { p->order_pos = i; break; }
        }
    }

    gst_element_set_state(p->playbin, GST_STATE_NULL);
    g_object_set(p->playbin, "uri", t->uri, NULL);
    gst_element_set_state(p->playbin, playing ? GST_STATE_PLAYING : GST_STATE_PAUSED);
    p->is_playing = playing;

    /* Учитываем запуск (а не восстановление на паузе/возобновление) для
     * статистики прослушиваний, по которой строятся «популярные треки». */
    if (playing && count_play) stats_on_play(t->uri);

    notify_state_changed(p);
}

void player_play_index(Player *p, int index) {
    start_track(p, index, TRUE, TRUE);
}

void player_toggle_pause(Player *p) {
    if (!p->playbin || p->current_index < 0) return;

    if (p->is_playing) {
        gst_element_set_state(p->playbin, GST_STATE_PAUSED);
        p->is_playing = FALSE;
    } else {
        /* После восстановления сессии playbin пуст (в NULL): грузим текущий
         * трек и запускаем его. При обычной паузе трек уже загружен — просто
         * возобновляем воспроизведение с сохранением позиции. */
        GstState st = GST_STATE_VOID_PENDING;
        gst_element_get_state(p->playbin, &st, NULL, 0);
        if (st <= GST_STATE_READY) {
            start_track(p, p->current_index, TRUE, FALSE);
            return;
        }
        gst_element_set_state(p->playbin, GST_STATE_PLAYING);
        p->is_playing = TRUE;
    }
    notify_state_changed(p);
}

/* Переход по последовательности воспроизведения. from_eos=TRUE — трек
 * закончился сам (учитывается повтор одного трека). */
static void player_advance(Player *p, gboolean from_eos) {
    int n = (int)p->queue->len;
    if (n == 0) return;

    if (from_eos && p->repeat == PLAYER_REPEAT_ONE) {
        start_track(p, p->current_index, TRUE, TRUE);
        return;
    }

    int pos = play_seq_pos(p);
    if (pos + 1 < n) {
        start_track(p, play_seq_index(p, pos + 1), TRUE, TRUE);
        return;
    }

    /* Конец очереди: повтор — играем с первого трека, иначе пауза на нём. */
    int first = play_seq_index(p, 0);
    start_track(p, first, p->repeat == PLAYER_REPEAT_ALL, TRUE);
}

void player_next(Player *p) {
    player_advance(p, FALSE);
}

void player_prev(Player *p) {
    int n = (int)p->queue->len;
    if (n == 0) return;

    int pos = play_seq_pos(p);
    if (pos - 1 >= 0) {
        start_track(p, play_seq_index(p, pos - 1), TRUE, TRUE);
    } else if (p->repeat == PLAYER_REPEAT_ALL) {
        start_track(p, play_seq_index(p, n - 1), TRUE, TRUE);
    } else {
        start_track(p, play_seq_index(p, 0), TRUE, TRUE);
    }
}

void player_set_shuffle(Player *p, gboolean enabled) {
    if (p->shuffle == enabled) return;
    p->shuffle = enabled;
    shuffle_rebuild(p, p->current_index);
    notify_state_changed(p);
}

gboolean player_get_shuffle(Player *p) {
    return p->shuffle;
}

void player_set_repeat(Player *p, PlayerRepeatMode mode) {
    if (p->repeat == mode) return;
    p->repeat = mode;
    notify_state_changed(p);
}

PlayerRepeatMode player_get_repeat(Player *p) {
    return p->repeat;
}

gint64 player_get_position(Player *p) {
    gint64 pos = 0;
    if (p->playbin && gst_element_query_position(p->playbin, GST_FORMAT_TIME, &pos)) {
        return pos;
    }
    return 0;
}

gint64 player_get_duration(Player *p) {
    gint64 dur = 0;
    if (p->playbin && gst_element_query_duration(p->playbin, GST_FORMAT_TIME, &dur)) {
        return dur;
    }
    return 0;
}

void player_seek(Player *p, gint64 position_ns) {
    if (!p->playbin || p->current_index < 0) return;
    gst_element_seek_simple(
        p->playbin,
        GST_FORMAT_TIME,
        GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
        position_ns
    );
}

double player_get_volume(Player *p) {
    double vol = 1.0;
    if (p->playbin) {
        g_object_get(p->playbin, "volume", &vol, NULL);
    }
    return vol;
}

void player_set_volume(Player *p, double volume) {
    if (p->playbin) {
        g_object_set(p->playbin, "volume", CLAMP(volume, 0.0, 1.0), NULL);
    }
}

GPtrArray *player_get_queue(Player *p) {
    return p->queue;
}

guint player_get_queue_version(Player *p) {
    return p->queue_version;
}

int player_get_current_index(Player *p) {
    return p->current_index;
}

Track *player_get_current_track(Player *p) {
    if (p->current_index >= 0 && p->current_index < (int)p->queue->len) {
        return g_ptr_array_index(p->queue, p->current_index);
    }
    return NULL;
}

gboolean player_is_playing(Player *p) {
    return p->is_playing;
}
