#include "playlists_view.h"
#include "album_detail_page.h"
#include "track.h"
#include "track_list.h"
#include "ui_helpers.h"
#include "view_controls.h"

struct _PlaylistsView {
    Player *player;
    GtkListBox *sidebar_list;

    GPtrArray *playlists; /* Playlist* */
    GPtrArray *library;   /* заимствованная ссылка на медиатеку */

    GtkWidget *page;                 /* корневой виджет для content_stack (nav view) */
    AdwNavigationView *nav;
    AdwNavigationPage *root;
    TrackList *track_list;
    ViewControls *controls;
    GtkWidget *title_widget;
    GtkWidget *play_btn;

    GPtrArray *current_tracks; /* Track* показанного плейлиста */
    char *current_path;

    guint scan_generation; /* защита от устаревших результатов сканирования */
};

/* ===================== область контента ===================== */

static void on_play_all(GtkButton *btn, gpointer user_data) {
    (void)btn;
    PlaylistsView *pv = user_data;
    if (pv->current_tracks && pv->current_tracks->len > 0) {
        player_set_queue_and_play(pv->player, pv->current_tracks, 0);
    }
}

static void on_controls_changed(ViewControls *vc, gpointer user_data) {
    (void)vc;
    PlaylistsView *pv = user_data;
    track_list_refresh(pv->track_list);
}

/* Перестраивает список треков текущего плейлиста. */
static void fill_current(PlaylistsView *pv) {
    Playlist *pl = pv->current_path
        ? playlist_find_by_path(pv->playlists, pv->current_path)
        : NULL;

    GPtrArray *tracks = pl ? playlist_resolve_tracks(pl, pv->library) : NULL;

    adw_window_title_set_title(ADW_WINDOW_TITLE(pv->title_widget),
                               pl ? pl->name : "Плейлист");

    GPtrArray *old = pv->current_tracks;
    pv->current_tracks = tracks
        ? tracks
        : g_ptr_array_new_with_free_func((GDestroyNotify)track_free);

    track_list_set_tracks(pv->track_list, pv->current_tracks);
    Track *cur = player_get_current_track(pv->player);
    track_list_set_playing_uri(pv->track_list, cur ? cur->uri : NULL);
    if (old) g_ptr_array_unref(old);

    guint n = pv->current_tracks->len;
    char *sub = g_strdup_printf("Треков: %u", n);
    adw_window_title_set_subtitle(ADW_WINDOW_TITLE(pv->title_widget), sub);
    g_free(sub);
    gtk_widget_set_sensitive(pv->play_btn, n > 0);
}

/* ===================== боковая панель ===================== */

/* Контекст меню плейлиста: к какому плейлисту и разделу относятся действия. */
typedef struct {
    PlaylistsView *pv;
    char *path;
    GtkWidget *row; /* слабая ссылка: через неё находим окно для диалогов */
} PlaylistMenuCtx;

static void playlist_menu_ctx_free(gpointer data) {
    PlaylistMenuCtx *c = data;
    g_free(c->path);
    g_free(c);
}

static void on_playlist_menu_rename(GSimpleAction *action, GVariant *param,
                                    gpointer user_data) {
    (void)action;
    (void)param;
    PlaylistMenuCtx *c = user_data;
    playlists_view_rename_dialog(c->pv, c->path,
                                 GTK_WIDGET(gtk_widget_get_root(c->row)));
}

static void on_playlist_menu_delete(GSimpleAction *action, GVariant *param,
                                    gpointer user_data) {
    (void)action;
    (void)param;
    PlaylistMenuCtx *c = user_data;
    playlists_view_delete_dialog(c->pv, c->path,
                                 GTK_WIDGET(gtk_widget_get_root(c->row)));
}

static GtkWidget *create_playlist_menu(PlaylistsView *pv, const char *path, GtkWidget *row) {
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Переименовать", "playlist.rename");
    g_menu_append(menu, "Удалить плейлист", "playlist.delete");

    GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    g_object_unref(menu);

    PlaylistMenuCtx *ctx = g_new0(PlaylistMenuCtx, 1);
    ctx->pv = pv;
    ctx->path = g_strdup(path);
    ctx->row = row;
    g_object_set_data_full(G_OBJECT(popover), "playlist-menu-ctx", ctx,
                           playlist_menu_ctx_free);

    const GActionEntry entries[] = {
        { "rename", on_playlist_menu_rename, NULL, NULL, NULL, {0} },
        { "delete", on_playlist_menu_delete, NULL, NULL, NULL, {0} },
    };
    GSimpleActionGroup *group = g_simple_action_group_new();
    g_action_map_add_action_entries(G_ACTION_MAP(group), entries,
                                    G_N_ELEMENTS(entries), ctx);
    gtk_widget_insert_action_group(popover, "playlist", G_ACTION_GROUP(group));
    g_object_unref(group);

    return popover;
}

static void on_playlist_popover_closed(GtkPopover *popover, gpointer user_data) {
    (void)user_data;
    gtk_widget_unparent(GTK_WIDGET(popover));
}

static void show_playlist_menu(GtkWidget *row, double x, double y) {
    PlaylistsView *pv = g_object_get_data(G_OBJECT(row), "playlists-view");
    const char *path = g_object_get_data(G_OBJECT(row), "playlist-path");
    if (!pv || !path) return;

    GtkWidget *popover = create_playlist_menu(pv, path, row);
    gtk_widget_set_parent(popover, row);

    GdkRectangle rect = { .x = (int)x, .y = (int)y, .width = 1, .height = 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    g_signal_connect(popover, "closed", G_CALLBACK(on_playlist_popover_closed), NULL);
    gtk_popover_popup(GTK_POPOVER(popover));
}

static void on_playlist_secondary_click(GtkGestureClick *gesture, int n_press,
                                        double x, double y, gpointer user_data) {
    (void)n_press;
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    show_playlist_menu(user_data, x, y);
}

static void on_playlist_long_press(GtkGestureLongPress *gesture, double x, double y,
                                   gpointer user_data) {
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    show_playlist_menu(user_data, x, y);
}

/* Контекстное меню строки плейлиста: ПКМ или долгое нажатие ЛКМ. К «Любимым
 * трекам» не привязывается (их нельзя ни удалить, ни переименовать). */
static void attach_playlist_menu(PlaylistsView *pv, GtkWidget *row) {
    g_object_set_data(G_OBJECT(row), "playlists-view", pv);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_playlist_secondary_click), row);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(click));

    GtkGesture *long_press = gtk_gesture_long_press_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(long_press), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(long_press),
                                               GTK_PHASE_CAPTURE);
    g_signal_connect(long_press, "pressed", G_CALLBACK(on_playlist_long_press), row);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(long_press));
}

static void clear_playlist_rows(PlaylistsView *pv) {
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(pv->sidebar_list));
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        if (g_object_get_data(G_OBJECT(child), "playlist-row")) {
            gtk_list_box_remove(pv->sidebar_list, child);
        }
        child = next;
    }
}

/* Индекс подзаголовка «Плейлисты» в боковой панели (-1, если его нет): по нему
 * «Любимые треки» вставляются выше, а остальные плейлисты — ниже. */
static int find_playlists_header_index(GtkListBox *list) {
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(list));
    int idx = 0;
    while (child) {
        if (g_strcmp0(g_object_get_data(G_OBJECT(child), "sidebar-role"),
                      "playlists-header") == 0) {
            return idx;
        }
        child = gtk_widget_get_next_sibling(child);
        idx++;
    }
    return -1;
}

static void rebuild_sidebar(PlaylistsView *pv) {
    clear_playlist_rows(pv);

    int header_index = find_playlists_header_index(pv->sidebar_list);
    Playlist *fav = playlist_find_favorite(pv->playlists);

    /* «Любимые треки» относятся к разделу «Треки» и стоят выше подзаголовка
     * «Плейлисты». */
    if (fav) {
        GtkWidget *row = ui_create_sidebar_row("playlists", fav->name,
                                               "emote-love-symbolic");
        g_object_set_data(G_OBJECT(row), "playlist-row", GINT_TO_POINTER(1));
        g_object_set_data_full(G_OBJECT(row), "playlist-path",
                               g_strdup(fav->path), g_free);
        if (header_index >= 0) {
            gtk_list_box_insert(pv->sidebar_list, row, header_index);
        } else {
            gtk_list_box_append(pv->sidebar_list, row);
        }
    }

    for (guint i = 0; i < pv->playlists->len; i++) {
        Playlist *pl = g_ptr_array_index(pv->playlists, i);
        if (pl->favorite) continue;
        GtkWidget *row = ui_create_sidebar_row("playlists", pl->name,
                                               "view-list-symbolic");
        g_object_set_data(G_OBJECT(row), "playlist-row", GINT_TO_POINTER(1));
        g_object_set_data_full(G_OBJECT(row), "playlist-path",
                               g_strdup(pl->path), g_free);
        attach_playlist_menu(pv, row);
        gtk_list_box_append(pv->sidebar_list, row);
    }
}

static void select_playlist_row(PlaylistsView *pv, const char *path) {
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(pv->sidebar_list));
    while (child) {
        if (GTK_IS_LIST_BOX_ROW(child) &&
            g_strcmp0(g_object_get_data(G_OBJECT(child), "playlist-path"), path) == 0) {
            gtk_list_box_select_row(pv->sidebar_list, GTK_LIST_BOX_ROW(child));
            return;
        }
        child = gtk_widget_get_next_sibling(child);
    }
}

/* ===================== сканирование в фоне ===================== */

/* Обход папок «Музыка» и данных может быть заметным на большой медиатеке,
 * поэтому выполняем его в фоновом потоке и применяем результат в главном. */
static void scan_task_func(GTask *task, gpointer source_object, gpointer task_data,
                           GCancellable *cancellable) {
    (void)source_object;
    (void)task_data;
    (void)cancellable;
    g_task_return_pointer(task, playlist_scan_all(),
                          (GDestroyNotify)g_ptr_array_unref);
}

static void on_scan_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    (void)source;
    PlaylistsView *pv = user_data;
    GTask *task = G_TASK(res);

    guint gen = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(task), "generation"));
    char *select_path = g_object_get_data(G_OBJECT(task), "select-path");

    GPtrArray *playlists = g_task_propagate_pointer(task, NULL);
    if (gen != pv->scan_generation) {
        if (playlists) g_ptr_array_unref(playlists);
        return;
    }

    if (pv->playlists) g_ptr_array_unref(pv->playlists);
    pv->playlists = playlists
        ? playlists
        : g_ptr_array_new_with_free_func((GDestroyNotify)playlist_free);

    rebuild_sidebar(pv);

    if (select_path) {
        select_playlist_row(pv, select_path);
    } else if (pv->current_path &&
               playlist_find_by_path(pv->playlists, pv->current_path)) {
        fill_current(pv);
    }

    /* Состояние «любимых» могло стать известно только теперь — обновляем
     * все кнопки-«сердечки». */
    track_menu_refresh_favorites();
}

static void start_scan(PlaylistsView *pv, const char *select_path) {
    pv->scan_generation++;
    GTask *task = g_task_new(NULL, NULL, on_scan_done, pv);
    g_object_set_data(G_OBJECT(task), "generation",
                      GUINT_TO_POINTER(pv->scan_generation));
    if (select_path) {
        g_object_set_data_full(G_OBJECT(task), "select-path",
                               g_strdup(select_path), g_free);
    }
    g_task_run_in_thread(task, scan_task_func);
    g_object_unref(task);
}

/* ===================== публичный интерфейс ===================== */

PlaylistsView *playlists_view_new(Player *player, AdwOverlaySplitView *split_view,
                                  GtkListBox *sidebar_list, GtkStack *content_stack) {
    PlaylistsView *pv = g_new0(PlaylistsView, 1);
    pv->player = player;
    pv->sidebar_list = sidebar_list;

    GtkWidget *hb = ui_create_content_header(split_view);
    pv->title_widget = adw_window_title_new("Плейлист", NULL);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(hb), pv->title_widget);

    pv->play_btn = gtk_button_new_with_label("Воспроизвести");
    gtk_widget_add_css_class(pv->play_btn, "suggested-action");
    gtk_widget_add_css_class(pv->play_btn, "pill");
    gtk_widget_set_valign(pv->play_btn, GTK_ALIGN_CENTER);
    g_signal_connect(pv->play_btn, "clicked", G_CALLBACK(on_play_all), pv);
    adw_header_bar_pack_end(ADW_HEADER_BAR(hb), pv->play_btn);

    pv->controls = view_controls_new(TRUE, on_controls_changed, pv);
    adw_header_bar_pack_end(ADW_HEADER_BAR(hb), view_controls_get_widget(pv->controls));

    TrackListConfig tl_cfg = {
        .context = TRACK_CONTEXT_TRACKS,
        .controls = pv->controls,
    };
    pv->track_list = track_list_new(player, &tl_cfg);

    AdwToolbarView *tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(tv, hb);
    adw_toolbar_view_set_content(tv, track_list_get_widget(pv->track_list));

    AdwNavigationPage *root = adw_navigation_page_new(GTK_WIDGET(tv), "Плейлисты");
    g_object_set_data(G_OBJECT(root), "playing-tracklist", pv->track_list);
    pv->root = root;
    pv->nav = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
    adw_navigation_view_add(pv->nav, root);
    pv->page = GTK_WIDGET(pv->nav);
    gtk_stack_add_named(content_stack, pv->page, "playlists");

    pv->playlists = g_ptr_array_new_with_free_func((GDestroyNotify)playlist_free);
    fill_current(pv);
    start_scan(pv, NULL);
    return pv;
}

void playlists_view_set_tracks(PlaylistsView *pv, GPtrArray *tracks) {
    if (pv->library) g_ptr_array_unref(pv->library);
    pv->library = tracks ? g_ptr_array_ref(tracks) : NULL;
    if (pv->current_path) fill_current(pv);
}

void playlists_view_show(PlaylistsView *pv, const char *path) {
    Playlist *pl = playlist_find_by_path(pv->playlists, path);
    if (!pl) return;
    g_free(pv->current_path);
    pv->current_path = g_strdup(path);
    fill_current(pv);
}

void playlists_view_reload(PlaylistsView *pv, const char *select_path) {
    start_scan(pv, select_path);
}

gboolean playlists_view_is_favorite(PlaylistsView *pv, const char *uri) {
    Playlist *fav = playlist_find_favorite(pv->playlists);
    return fav && uri && playlist_contains_uri(fav, uri);
}

void playlists_view_set_favorite(PlaylistsView *pv, const char *uri, gboolean favorite) {
    Playlist *fav = playlist_find_favorite(pv->playlists);
    if (!fav || !uri) return;

    if (favorite) {
        playlist_add_uri(fav, uri);
    } else {
        playlist_remove_uri(fav, uri);
    }

    if (pv->current_path && g_strcmp0(pv->current_path, fav->path) == 0) {
        fill_current(pv);
    }
}

GPtrArray *playlists_view_get_playlists(PlaylistsView *pv) {
    return pv->playlists;
}

void playlists_view_refresh_playing(PlaylistsView *pv, const char *playing_uri) {
    if (!pv) return;
    AdwNavigationPage *top = adw_navigation_view_get_visible_page(pv->nav);
    if (!top) return;
    GtkWidget *page = GTK_WIDGET(top);
    TrackList *tl = g_object_get_data(G_OBJECT(page), "playing-tracklist");
    if (tl) track_list_set_playing_uri(tl, playing_uri);
    ui_highlight_playing_on_page(page, playing_uri);
}

/* Альбом открывается в этом же стеке: раздел плейлистов не переключается. */
void playlists_view_open_album(PlaylistsView *pv, const char *album, const char *album_artist) {
    if (!pv->library || !album || !*album) return;

    GtkListBox *sublist = NULL;
    AdwNavigationPage *page = album_detail_page_create(pv->player, pv->library,
                                                       album, album_artist, &sublist);
    if (page) adw_navigation_view_push(pv->nav, page);
}

void playlists_view_add_uri(PlaylistsView *pv, const char *path, const char *uri) {
    Playlist *pl = playlist_find_by_path(pv->playlists, path);
    if (!pl || !uri) return;

    playlist_add_uri(pl, uri);
    if (pv->current_path && g_strcmp0(pv->current_path, path) == 0) {
        fill_current(pv);
    }
}

/* ===================== создание плейлиста ===================== */

static void on_create_response(AdwAlertDialog *dialog, const char *response,
                               gpointer user_data) {
    if (g_strcmp0(response, "create") != 0) return;

    PlaylistsView *pv = user_data;
    GtkWidget *entry = g_object_get_data(G_OBJECT(dialog), "entry");
    const char *name = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!name || !*name) return;

    Playlist *pl = playlist_create(name);
    if (!pl) return;

    char *path = g_strdup(pl->path);
    playlist_free(pl);
    playlists_view_reload(pv, path);
    g_free(path);
}

void playlists_view_create_dialog(PlaylistsView *pv, GtkWidget *parent) {
    AdwDialog *dialog = adw_alert_dialog_new("Новый плейлист", NULL);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Отмена");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "create", "Создать");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "create",
                                             ADW_RESPONSE_SUGGESTED);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Название плейлиста");
    gtk_widget_set_margin_top(entry, 6);
    g_object_set_data(G_OBJECT(dialog), "entry", entry);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), entry);

    g_signal_connect(dialog, "response", G_CALLBACK(on_create_response), pv);
    adw_dialog_present(dialog, parent);
}

/* ===================== переименование и удаление ===================== */

static void on_rename_response(AdwAlertDialog *dialog, const char *response,
                               gpointer user_data) {
    if (g_strcmp0(response, "rename") != 0) return;

    PlaylistsView *pv = user_data;
    const char *path = g_object_get_data(G_OBJECT(dialog), "playlist-path");
    GtkWidget *entry = g_object_get_data(G_OBJECT(dialog), "entry");
    const char *name = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!name || !*name) return;

    Playlist *pl = playlist_find_by_path(pv->playlists, path);
    if (!pl) return;

    /* Переключение на переименованный плейлист нужно только если он сейчас
     * открыт; иначе сохраняем текущий раздел. */
    gboolean was_current = pv->current_path &&
        g_strcmp0(pv->current_path, path) == 0;

    char *new_path = playlist_rename(pl, name);
    if (!new_path) return;

    playlists_view_reload(pv, was_current ? new_path : NULL);
    g_free(new_path);
}

void playlists_view_rename_dialog(PlaylistsView *pv, const char *path, GtkWidget *parent) {
    Playlist *pl = playlist_find_by_path(pv->playlists, path);
    if (!pl || pl->favorite) return;

    AdwDialog *dialog = adw_alert_dialog_new("Переименовать плейлист", NULL);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Отмена");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "rename", "Переименовать");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "rename",
                                             ADW_RESPONSE_SUGGESTED);

    GtkWidget *entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), pl->name ? pl->name : "");
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
    gtk_widget_set_margin_top(entry, 6);
    g_object_set_data(G_OBJECT(dialog), "entry", entry);
    g_object_set_data_full(G_OBJECT(dialog), "playlist-path", g_strdup(path), g_free);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), entry);

    g_signal_connect(dialog, "response", G_CALLBACK(on_rename_response), pv);
    adw_dialog_present(dialog, parent);
}

static void on_delete_response(AdwAlertDialog *dialog, const char *response,
                               gpointer user_data) {
    if (g_strcmp0(response, "delete") != 0) return;

    PlaylistsView *pv = user_data;
    const char *path = g_object_get_data(G_OBJECT(dialog), "playlist-path");

    Playlist *pl = playlist_find_by_path(pv->playlists, path);
    if (!pl || pl->favorite) return;
    if (!playlist_delete(pl)) return;

    /* Если удалили открытый плейлист, сбрасываем текущий, иначе он останется
     * указывать на исчезнувший файл. */
    if (pv->current_path && g_strcmp0(pv->current_path, path) == 0) {
        g_free(pv->current_path);
        pv->current_path = NULL;
        /* Показываем пустую страницу вместо удалённого списка. */
        fill_current(pv);
    }
    playlists_view_reload(pv, NULL);
}

void playlists_view_delete_dialog(PlaylistsView *pv, const char *path, GtkWidget *parent) {
    Playlist *pl = playlist_find_by_path(pv->playlists, path);
    if (!pl || pl->favorite) return;

    AdwDialog *dialog = adw_alert_dialog_new("Удалить плейлист?", pl->name);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Отмена");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "delete", "Удалить");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete",
                                             ADW_RESPONSE_DESTRUCTIVE);
    g_object_set_data_full(G_OBJECT(dialog), "playlist-path", g_strdup(path), g_free);

    g_signal_connect(dialog, "response", G_CALLBACK(on_delete_response), pv);
    adw_dialog_present(dialog, parent);
}

void playlists_view_free(PlaylistsView *pv) {
    if (!pv) return;
    if (pv->playlists) g_ptr_array_unref(pv->playlists);
    if (pv->library) g_ptr_array_unref(pv->library);
    if (pv->current_tracks) g_ptr_array_unref(pv->current_tracks);
    if (pv->controls) view_controls_free(pv->controls);
    g_free(pv->current_path);
    g_free(pv);
}
