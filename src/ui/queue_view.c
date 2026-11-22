#include "queue_view.h"
#include "track_list.h"

struct _QueueView {
    Player *player;
    GtkWidget *widget;
    TrackList *list;
};

/* Смена порядка применяется в idle: строки будут перестроены при обновлении
 * очереди, и делать это внутри сигнала "drop" небезопасно. */
typedef struct {
    QueueView *qv;
    int from;
    int to;
} QueueMoveRequest;

static gboolean perform_queue_move(gpointer data) {
    QueueMoveRequest *req = data;
    player_move_in_queue(req->qv->player, req->from, req->to);
    g_free(req);
    return G_SOURCE_REMOVE;
}

static void on_reorder(TrackList *tl, int from, int to, gpointer user_data) {
    (void)tl;
    QueueView *qv = user_data;
    QueueMoveRequest *req = g_new0(QueueMoveRequest, 1);
    req->qv = qv;
    req->from = from;
    req->to = to;
    g_idle_add(perform_queue_move, req);
}

static void on_activate(TrackList *tl, guint position, gpointer user_data) {
    (void)tl;
    QueueView *qv = user_data;
    player_play_index(qv->player, (int)position);
}

QueueView *queue_view_new(Player *player) {
    QueueView *qv = g_new0(QueueView, 1);
    qv->player = player;

    TrackListConfig cfg = {
        .context = TRACK_CONTEXT_QUEUE,
        .subtitle_artist_only = TRUE, /* в очереди подзаголовок — только исполнитель */
        .reorderable = TRUE,
        .on_activate = on_activate,
        .on_reorder = on_reorder,
        .user_data = qv,
    };
    qv->list = track_list_new(player, &cfg);

    AdwHeaderBar *hb = ADW_HEADER_BAR(adw_header_bar_new());
    adw_header_bar_set_show_start_title_buttons(hb, FALSE);
    adw_header_bar_set_title_widget(hb, GTK_WIDGET(adw_window_title_new("Очередь", NULL)));
    /* Панель стоит у правого края окна, поэтому AdwHeaderBar сам покажет
     * нативную кнопку закрытия окна в конце заголовка. */

    AdwToolbarView *tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(tv, GTK_WIDGET(hb));
    adw_toolbar_view_set_content(tv, track_list_get_widget(qv->list));

    qv->widget = GTK_WIDGET(adw_navigation_page_new(GTK_WIDGET(tv), "Очередь"));
    return qv;
}

GtkWidget *queue_view_get_widget(QueueView *qv) {
    return qv->widget;
}

void queue_view_refresh(QueueView *qv) {
    track_list_set_tracks(qv->list, player_get_queue(qv->player));
    track_list_set_playing_index(qv->list, player_get_current_index(qv->player));
}

void queue_view_update_playing(QueueView *qv) {
    track_list_set_playing_index(qv->list, player_get_current_index(qv->player));
}

void queue_view_free(QueueView *qv) {
    g_free(qv);
}
