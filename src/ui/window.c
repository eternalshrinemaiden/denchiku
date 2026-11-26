#include "window.h"
#include "album_detail_page.h"
#include "library_view.h"
#include "player_bar.h"
#include "playlists_view.h"
#include "queue_view.h"
#include "scanner.h"
#include "settings.h"
#include "settings_dialog.h"
#include "state.h"
#include "stats.h"
#include "tag_editor.h"
#include "track.h"
#include "track_info_dialog.h"
#include "track_menu.h"
#include "ui_helpers.h"
#include "view_controls.h"

typedef struct {
    LibraryView *library_view;
    PlaylistsView *playlists_view;
    QueueView *queue_view;
    PlayerBar *player_bar;
    PlayerBar *full_player;

    AdwBottomSheet *bottom_sheet;
    AdwOverlaySplitView *split_view;
    AdwOverlaySplitView *queue_split_view;
    GtkStack *content_stack;
    GtkListBox *sidebar_list;

    GtkSpinner *scan_spinner;
    GSimpleAction *refresh_action;

    GtkWidget *window;

    Player *player;
    AppState *state;

    GPtrArray *current_tracks;
    guint queue_version;      /* последняя увиденная версия состава очереди */
    char *last_playing_uri;   /* трек, для которого уже обновлена подсветка */
} WindowState;

static void window_state_free(WindowState *ws) {
    library_view_free(ws->library_view);
    playlists_view_free(ws->playlists_view);
    queue_view_free(ws->queue_view);
    player_bar_free(ws->player_bar);
    player_bar_free(ws->full_player);
    if (ws->current_tracks) g_ptr_array_unref(ws->current_tracks);
    g_free(ws->last_playing_uri);
    state_free(ws->state);
    g_free(ws);
}

static gboolean track_equals(const Track *a, const Track *b) {
    return a->track_number == b->track_number &&
           a->disc_number == b->disc_number &&
           g_strcmp0(a->disc_subtitle, b->disc_subtitle) == 0 &&
           a->year == b->year &&
           g_strcmp0(a->uri, b->uri) == 0 &&
           g_strcmp0(a->title, b->title) == 0 &&
           g_strcmp0(a->artist, b->artist) == 0 &&
           g_strcmp0(a->album, b->album) == 0 &&
           g_strcmp0(a->album_artist, b->album_artist) == 0 &&
           g_strcmp0(a->genre, b->genre) == 0 &&
           g_strcmp0(a->cover_path, b->cover_path) == 0;
}

static gboolean tracks_equal(GPtrArray *a, GPtrArray *b) {
    if (a == b) return TRUE;
    if (!a || !b) return (!a && !b);
    if (a->len != b->len) return FALSE;
    for (guint i = 0; i < a->len; i++) {
        Track *ta = g_ptr_array_index(a, i);
        Track *tb = g_ptr_array_index(b, i);
        /* Кэш отдаёт общие объекты: неизменённый трек — тот же указатель, так что
         * в типичном случае (скан вернул ту же медиатеку) сравнение сводится к
         * сравнению указателей, без разбора строк. */
        if (ta == tb) continue;
        if (!track_equals(ta, tb)) return FALSE;
    }
    return TRUE;
}

static void on_player_updated(Player *player, gpointer user_data) {
    WindowState *ws = user_data;
    player_bar_refresh(ws->player_bar);
    player_bar_refresh(ws->full_player);
    if (!player_get_current_track(player)) {
        adw_bottom_sheet_set_open(ws->bottom_sheet, FALSE);
    }
    /* Пустая очередь — панель прячем. */
    GPtrArray *queue = player_get_queue(player);
    if (!queue || queue->len == 0) {
        adw_overlay_split_view_set_show_sidebar(ws->queue_split_view, FALSE);
    }

    /* Полная перестройка строк очереди нужна только при изменении её состава;
     * смена текущего трека (или пауза) обновляет лишь индикатор. На большой
     * очереди это убирает подвисания при каждом переходе между треками. */
    guint version = player_get_queue_version(player);
    if (version != ws->queue_version) {
        ws->queue_version = version;
        queue_view_refresh(ws->queue_view);
    } else {
        queue_view_update_playing(ws->queue_view);
    }

    /* Подсветку играющего трека в списках медиатеки трогаем только когда трек
     * действительно сменился. */
    Track *cur = player_get_current_track(player);
    const char *uri = cur ? cur->uri : NULL;
    if (g_strcmp0(uri, ws->last_playing_uri) != 0) {
        g_free(ws->last_playing_uri);
        ws->last_playing_uri = g_strdup(uri);
        library_view_refresh_playing(ws->library_view);
        playlists_view_refresh_playing(ws->playlists_view, uri);
    }
}

void main_window_set_tracks(GtkWidget *window, GPtrArray *tracks) {
    WindowState *ws = g_object_get_data(G_OBJECT(window), "window-state");
    /* Полная перестройка списков (и перепривязка обложек) стоит заметное время,
     * а фоновое сканирование в типичном запуске возвращает тот же состав, что и
     * загруженный кэш. Пропускаем обновление, если медиатека не изменилась. */
    if (tracks_equal(ws->current_tracks, tracks)) return;
    if (ws->current_tracks) g_ptr_array_unref(ws->current_tracks);
    ws->current_tracks = tracks ? g_ptr_array_ref(tracks) : NULL;
    library_view_set_tracks(ws->library_view, tracks);
    playlists_view_set_tracks(ws->playlists_view, tracks);
}

void main_window_set_scanning(GtkWidget *window, gboolean scanning) {
    WindowState *ws = g_object_get_data(G_OBJECT(window), "window-state");
    gtk_widget_set_visible(GTK_WIDGET(ws->scan_spinner), scanning);
    gtk_spinner_set_spinning(ws->scan_spinner, scanning);
    if (ws->refresh_action) {
        g_simple_action_set_enabled(ws->refresh_action, !scanning);
    }
}

/* ===== сведения о треке и внешний редактор тегов ===== */

/* Пункт меню трека «Сведения»: окно с метаданными трека. */
static void on_track_show_info(Track *track, gpointer user_data) {
    WindowState *ws = user_data;
    if (!track) return;
    track_info_dialog_present(GTK_WINDOW(ws->window), track);
}

/* Кнопка-карандаш в шапке альбома: открывает все треки альбома во внешнем
 * редакторе тегов (выбранном в настройках). */
static void on_album_edit_tags(GPtrArray *tracks, gpointer user_data) {
    WindowState *ws = user_data;
    if (!tracks || tracks->len == 0) return;

    GPtrArray *uris = g_ptr_array_new();
    for (guint i = 0; i < tracks->len; i++) {
        Track *t = g_ptr_array_index(tracks, i);
        if (t->uri && *t->uri) g_ptr_array_add(uris, t->uri);
    }

    const char *editor = settings_tag_editor();
    GError *err = NULL;
    gboolean ok = tag_editor_launch(editor, uris, &err);
    g_ptr_array_unref(uris);

    if (!ok) {
        AdwDialog *alert = adw_alert_dialog_new("Не удалось открыть редактор тегов",
                                                err ? err->message : NULL);
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "ok", "Ок");
        adw_dialog_present(alert, ws->window);
    }
    if (err) g_error_free(err);
}

static void on_manual_scan_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    (void)source;
    GtkWidget *win = GTK_WIDGET(user_data);
    GPtrArray *tracks = scanner_scan_finish(res, NULL);
    if (tracks) {
        main_window_set_tracks(win, tracks);
        g_ptr_array_unref(tracks);
    }
    main_window_set_scanning(win, FALSE);
}

static void on_refresh_action(GSimpleAction *action, GVariant *param, gpointer user_data) {
    (void)action;
    (void)param;
    GtkWidget *win = GTK_WIDGET(user_data);
    WindowState *ws = g_object_get_data(G_OBJECT(win), "window-state");
    /* Заодно перечитываем плейлисты: пользователь мог добавить файлы в папку. */
    if (ws && ws->playlists_view) playlists_view_reload(ws->playlists_view, NULL);
    main_window_set_scanning(win, TRUE);
    scanner_scan_async(NULL, on_manual_scan_done, win);
}

static void on_shortcuts_action(GSimpleAction *action, GVariant *param, gpointer user_data) {
    (void)action;
    (void)param;
    GtkWindow *win = GTK_WINDOW(user_data);

    GtkBuilder *builder = gtk_builder_new_from_string(
        "<interface>"
        "  <object class='GtkShortcutsWindow' id='shortcuts'>"
        "    <property name='modal'>1</property>"
        "    <child>"
        "      <object class='GtkShortcutsSection'>"
        "        <property name='section-name'>shortcuts</property>"
        "        <child>"
        "          <object class='GtkShortcutsGroup'>"
        "            <property name='title'>Общие</property>"
        "            <child>"
        "              <object class='GtkShortcutsShortcut'>"
        "                <property name='title'>Обновить медиатеку</property>"
        "                <property name='accelerator'>F5 &lt;Primary&gt;r</property>"
        "              </object>"
        "            </child>"
        "            <child>"
        "              <object class='GtkShortcutsShortcut'>"
        "                <property name='title'>Комбинации клавиш</property>"
        "                <property name='accelerator'>&lt;Primary&gt;question</property>"
        "              </object>"
        "            </child>"
        "            <child>"
        "              <object class='GtkShortcutsShortcut'>"
        "                <property name='title'>Настройки</property>"
        "                <property name='accelerator'>&lt;Primary&gt;comma</property>"
        "              </object>"
        "            </child>"
        "            <child>"
        "              <object class='GtkShortcutsShortcut'>"
        "                <property name='title'>Выйти из приложения</property>"
        "                <property name='accelerator'>&lt;Primary&gt;q</property>"
        "              </object>"
        "            </child>"
        "          </object>"
        "        </child>"
        "      </object>"
        "    </child>"
        "  </object>"
        "</interface>", -1);

    GtkWidget *shortcuts_win = GTK_WIDGET(gtk_builder_get_object(builder, "shortcuts"));
    gtk_window_set_transient_for(GTK_WINDOW(shortcuts_win), win);
    gtk_window_present(GTK_WINDOW(shortcuts_win));
    g_object_unref(builder);
}

static void on_settings_changed(SettingsCategory category, gpointer user_data) {
    WindowState *ws = user_data;
    /* Настройки tag-splitting влияют только на группировку, поэтому заново
     * наполняем лишь изменившийся вид. */
    if (!ws->current_tracks) return;
    if (category == SETTINGS_CATEGORY_ARTISTS)
        library_view_regroup_artists(ws->library_view, ws->current_tracks);
    else
        library_view_regroup_genres(ws->library_view, ws->current_tracks);
}

static void on_settings_action(GSimpleAction *action, GVariant *param, gpointer user_data) {
    (void)action;
    (void)param;
    GtkWidget *win = GTK_WIDGET(user_data);
    WindowState *ws = g_object_get_data(G_OBJECT(win), "window-state");
    settings_dialog_present(GTK_WINDOW(win), on_settings_changed, ws);
}

static void on_about_action(GSimpleAction *action, GVariant *param, gpointer user_data) {
    (void)action;
    (void)param;
    GtkWidget *win = GTK_WIDGET(user_data);

    AdwDialog *about = adw_about_dialog_new();
    adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), "Denchiku");
    adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about), "folder-music-symbolic");
    adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), "0.2.0");
    adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(about),
        "Быстрый и легкий аудиоплеер для рабочего стола GNOME на GTK 4, Libadwaita и GStreamer.");
    adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(about), GTK_LICENSE_GPL_3_0);

    const char *developers[] = { "Свята Романовская", NULL };
    adw_about_dialog_set_developers(ADW_ABOUT_DIALOG(about), developers);

    adw_dialog_present(about, win);
}

/* Программно выбираем строку сайдбара по id раздела: подсветка и содержимое
 * стека синхронизируются через row-selected. */
static void select_sidebar_page(WindowState *ws, const char *page_id) {
    for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(ws->sidebar_list));
         row != NULL; row = gtk_widget_get_next_sibling(row)) {
        if (!GTK_IS_LIST_BOX_ROW(row)) continue;
        if (g_strcmp0(g_object_get_data(G_OBJECT(row), "page-id"), page_id) == 0) {
            gtk_list_box_select_row(ws->sidebar_list, GTK_LIST_BOX_ROW(row));
            return;
        }
    }
}

static void on_sidebar_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box;
    if (!row) return;

    WindowState *ws = user_data;
    const char *page_id = g_object_get_data(G_OBJECT(row), "page-id");

    if (page_id) {
        gtk_stack_set_visible_child_name(ws->content_stack, page_id);
    }

    if (g_strcmp0(page_id, "playlists") == 0) {
        const char *path = g_object_get_data(G_OBJECT(row), "playlist-path");
        if (path) playlists_view_show(ws->playlists_view, path);
    }

    if (adw_overlay_split_view_get_collapsed(ws->split_view)) {
        adw_overlay_split_view_set_show_sidebar(ws->split_view, FALSE);
    }
}

/* Пункт меню трека «Открыть альбом»: открываем страницу альбома в ТЕКУЩЕМ
 * разделе (вкладка не переключается), чтобы сохранить контекст пользователя. */
static void on_track_open_album(const char *album, const char *album_artist,
                                gpointer user_data) {
    WindowState *ws = user_data;
    if (!album || !*album) return;

    const char *section = gtk_stack_get_visible_child_name(ws->content_stack);
    if (g_strcmp0(section, "playlists") == 0) {
        playlists_view_open_album(ws->playlists_view, album, album_artist);
        return;
    }
    library_view_open_album(ws->library_view, section ? section : "albums",
                            album, album_artist);
}

/* Пункт меню трека «Перейти к исполнителю» и ссылка на исполнителя в шапке
 * альбома: страница открывается в ТЕКУЩЕМ разделе (вкладка не переключается),
 * как и «Открыть альбом». */
static void on_track_open_artist(const char *artist, gpointer user_data) {
    WindowState *ws = user_data;
    if (!artist || !*artist) return;

    const char *section = gtk_stack_get_visible_child_name(ws->content_stack);
    if (g_strcmp0(section, "playlists") == 0) {
        playlists_view_open_artist(ws->playlists_view, artist);
        return;
    }
    library_view_open_artist(ws->library_view, section ? section : "artists", artist);
}

/* Клик по жанру в шапке альбома: переключаемся на вкладку «Жанры» и делаем этот
 * жанр активным фильтром. */
static void on_album_open_genre(const char *genre, gpointer user_data) {
    WindowState *ws = user_data;
    if (!genre || !*genre) return;

    select_sidebar_page(ws, "genres");
    gtk_stack_set_visible_child_name(ws->content_stack, "genres");
    library_view_select_genre(ws->library_view, genre);
}

/* ===== Плейлисты: колбэки для меню трека и сердечек ===== */

static gboolean on_is_favorite(const char *uri, gpointer user_data) {
    WindowState *ws = user_data;
    return playlists_view_is_favorite(ws->playlists_view, uri);
}

static void on_toggle_favorite(const char *uri, gboolean favorite, gpointer user_data) {
    WindowState *ws = user_data;
    playlists_view_set_favorite(ws->playlists_view, uri, favorite);
    track_menu_refresh_favorites();
}

static GPtrArray *on_list_playlists(gpointer user_data) {
    WindowState *ws = user_data;
    return playlists_view_get_playlists(ws->playlists_view);
}

static gboolean on_playlist_has(const char *path, const char *uri, gpointer user_data) {
    WindowState *ws = user_data;
    return playlists_view_contains_uri(ws->playlists_view, path, uri);
}

/* Вопрос, когда трек уже есть в плейлисте: добавить вторую копию или удалить
 * все копии. */
typedef struct {
    WindowState *ws;
    char *path;
    char *uri;
} PlaylistAskCtx;

static void playlist_ask_ctx_free(gpointer data) {
    PlaylistAskCtx *ctx = data;
    g_free(ctx->path);
    g_free(ctx->uri);
    g_free(ctx);
}

static void on_playlist_ask_response(AdwAlertDialog *dialog, const char *response,
                                     gpointer user_data) {
    (void)user_data;
    PlaylistAskCtx *ctx = g_object_get_data(G_OBJECT(dialog), "playlist-ask-ctx");
    if (!ctx) return;

    if (g_strcmp0(response, "add") == 0) {
        playlists_view_add_uri_dup(ctx->ws->playlists_view, ctx->path, ctx->uri);
    } else if (g_strcmp0(response, "remove") == 0) {
        playlists_view_remove_uri(ctx->ws->playlists_view, ctx->path, ctx->uri);
    }
}

static void on_toggle_playlist(const char *path, const char *uri, gpointer user_data) {
    WindowState *ws = user_data;
    if (!path || !uri) return;

    /* Трека в плейлисте нет — просто добавляем. */
    if (!playlists_view_contains_uri(ws->playlists_view, path, uri)) {
        playlists_view_add_uri(ws->playlists_view, path, uri);
        track_menu_refresh_favorites();
        return;
    }

    /* Трек уже в плейлисте: спрашиваем, добавить ещё раз или удалить. */
    Playlist *pl = playlist_find_by_path(playlists_view_get_playlists(ws->playlists_view),
                                         path);
    AdwDialog *dialog = adw_alert_dialog_new("Трек уже в плейлисте",
                                             pl ? pl->name : NULL);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "add", "Добавить ещё раз");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "remove", "Удалить");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "remove",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "add");

    PlaylistAskCtx *ctx = g_new0(PlaylistAskCtx, 1);
    ctx->ws = ws;
    ctx->path = g_strdup(path);
    ctx->uri = g_strdup(uri);
    g_object_set_data_full(G_OBJECT(dialog), "playlist-ask-ctx", ctx,
                           playlist_ask_ctx_free);
    g_signal_connect(dialog, "response", G_CALLBACK(on_playlist_ask_response), NULL);
    adw_dialog_present(dialog, ws->window);
}

static void on_create_playlist_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    WindowState *ws = user_data;
    playlists_view_create_dialog(ws->playlists_view, ws->window);
}

static GtkWidget *create_primary_menu_button(void) {
    GMenu *menu = g_menu_new();

    GMenu *section_lib = g_menu_new();
    g_menu_append(section_lib, "Обновить медиатеку", "win.refresh");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section_lib));

    GMenu *section_app = g_menu_new();
    g_menu_append(section_app, "Настройки", "win.settings");
    g_menu_append(section_app, "Комбинации клавиш", "win.shortcuts");
    g_menu_append(section_app, "О приложении", "win.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section_app));

    GtkWidget *menu_btn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_btn), "open-menu-symbolic");
    gtk_widget_set_tooltip_text(menu_btn, "Главное меню");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn), G_MENU_MODEL(menu));
    gtk_menu_button_set_primary(GTK_MENU_BUTTON(menu_btn), TRUE);

    g_object_unref(section_lib);
    g_object_unref(section_app);
    g_object_unref(menu);

    return menu_btn;
}

static void on_mobile_breakpoint_apply(AdwBreakpoint *bp, gpointer user_data) {
    (void)bp;
    WindowState *ws = user_data;
    adw_overlay_split_view_set_show_sidebar(ws->queue_split_view, FALSE);
}

/* Собирает текущее состояние сессии в s (для сохранения при выходе). */
static void collect_state(WindowState *ws, AppState *s) {
    GtkWindow *win = GTK_WINDOW(ws->window);

    s->maximized = gtk_window_is_maximized(win);
    if (!s->maximized) {
        int w = gtk_widget_get_width(GTK_WIDGET(win));
        int h = gtk_widget_get_height(GTK_WIDGET(win));
        if (w > 0) s->width = w;
        if (h > 0) s->height = h;
    }

    s->shuffle = player_get_shuffle(ws->player);
    s->repeat = (int)player_get_repeat(ws->player);
    s->volume = player_get_volume(ws->player);

    for (int i = 0; i < s->queue_len; i++) g_free(s->queue_uris[i]);
    g_free(s->queue_uris);
    GPtrArray *queue = player_get_queue(ws->player);
    int n = queue ? (int)queue->len : 0;
    s->queue_uris = n > 0 ? g_new0(char *, n + 1) : NULL;
    s->queue_len = n;
    for (int i = 0; i < n; i++) {
        Track *t = g_ptr_array_index(queue, i);
        s->queue_uris[i] = g_strdup(t->uri ? t->uri : "");
    }
    g_free(s->current_uri);
    Track *cur = player_get_current_track(ws->player);
    s->current_uri = cur ? g_strdup(cur->uri) : NULL;
    s->queue_visible = adw_overlay_split_view_get_show_sidebar(ws->queue_split_view);

    ViewControls *vc = library_view_get_view_controls(ws->library_view);
    if (vc) {
        s->view_mode = view_controls_get_mode(vc);
        s->view_zoom = view_controls_get_zoom(vc);
        s->view_sort = view_controls_get_sort_field(vc);
        s->view_sort_desc = view_controls_get_sort_descending(vc);
    }
}

static gboolean on_window_close_request(GtkWindow *win, gpointer user_data) {
    (void)win;
    WindowState *ws = user_data;
    if (ws->state) {
        collect_state(ws, ws->state);
        state_save(ws->state);
    }
    stats_save();
    return FALSE; /* разрешаем закрытие окна */
}

/* Восстанавливает очередь и текущий трек из сохранённого состояния, сопоставляя
 * URI с загруженными треками. Вызывается после загрузки медиатеки. */
void main_window_restore_session(GtkWidget *window, GPtrArray *tracks) {
    WindowState *ws = g_object_get_data(G_OBJECT(window), "window-state");
    AppState *s = ws->state;
    if (!s || s->queue_len <= 0 || !tracks) return;

    /* Индекс по URI для быстрого сопоставления. */
    GHashTable *by_uri = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < tracks->len; i++) {
        Track *t = g_ptr_array_index(tracks, i);
        if (t->uri && !g_hash_table_contains(by_uri, t->uri)) {
            g_hash_table_insert(by_uri, t->uri, t);
        }
    }

    GPtrArray *q = g_ptr_array_new(); /* без free-func: треки общие, player берёт ссылки */
    int current = -1;
    for (int i = 0; i < s->queue_len; i++) {
        Track *t = g_hash_table_lookup(by_uri, s->queue_uris[i]);
        if (!t) continue;
        if (current < 0 && s->current_uri &&
            g_strcmp0(s->queue_uris[i], s->current_uri) == 0) {
            current = (int)q->len;
        }
        g_ptr_array_add(q, t);
    }
    g_hash_table_destroy(by_uri);

    if (q->len > 0) {
        player_restore_queue(ws->player, q, current);
        if (s->queue_visible && current >= 0) {
            adw_overlay_split_view_set_show_sidebar(ws->queue_split_view, TRUE);
        }
    }
    g_ptr_array_unref(q);
}

GtkWidget *main_window_new(AdwApplication *app, Player *player) {
    WindowState *ws = g_new0(WindowState, 1);
    ws->player = player;
    ws->state = state_load();

    AdwApplicationWindow *win = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
    gtk_window_set_default_size(GTK_WINDOW(win), ws->state->width, ws->state->height);
    gtk_window_set_title(GTK_WINDOW(win), "Музыка");
    g_object_set_data_full(G_OBJECT(win), "window-state", ws, (GDestroyNotify)window_state_free);
    ws->window = GTK_WIDGET(win);

    /* Режимы воспроизведения и громкость восстанавливаем до создания панели,
     * чтобы её элементы сразу показывали сохранённые значения. */
    player_set_shuffle(player, ws->state->shuffle);
    player_set_repeat(player, (PlayerRepeatMode)ws->state->repeat);
    player_set_volume(player, ws->state->volume);

    const GActionEntry win_entries[] = {
        { "refresh",   on_refresh_action,   NULL, NULL, NULL, {0} },
        { "settings",  on_settings_action,  NULL, NULL, NULL, {0} },
        { "shortcuts", on_shortcuts_action, NULL, NULL, NULL, {0} },
        { "about",     on_about_action,     NULL, NULL, NULL, {0} },
    };
    g_action_map_add_action_entries(G_ACTION_MAP(win), win_entries, G_N_ELEMENTS(win_entries), win);
    ws->refresh_action = G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(win), "refresh"));

    AdwBreakpoint *bp = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 650sp"));
    ws->split_view = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
    GValue collapsed_val = G_VALUE_INIT;
    g_value_init(&collapsed_val, G_TYPE_BOOLEAN);
    g_value_set_boolean(&collapsed_val, TRUE);
    adw_breakpoint_add_setter(bp, G_OBJECT(ws->split_view), "collapsed", &collapsed_val);
    g_value_unset(&collapsed_val);
    adw_application_window_add_breakpoint(win, bp);

    /* ================= 1. БОКОВАЯ ПАНЕЛЬ ("Музыка") ================= */
    AdwToolbarView *sidebar_tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    AdwHeaderBar *sidebar_hb = ADW_HEADER_BAR(adw_header_bar_new());
    adw_header_bar_set_show_end_title_buttons(sidebar_hb, FALSE);
    adw_header_bar_set_title_widget(sidebar_hb, GTK_WIDGET(adw_window_title_new("Музыка", NULL)));
    
    adw_header_bar_pack_end(sidebar_hb, create_primary_menu_button());

    GtkWidget *spinner = gtk_spinner_new();
    ws->scan_spinner = GTK_SPINNER(spinner);
    adw_header_bar_pack_end(sidebar_hb, spinner);

    adw_toolbar_view_add_top_bar(sidebar_tv, GTK_WIDGET(sidebar_hb));

    GtkWidget *sidebar_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    ws->sidebar_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_add_css_class(GTK_WIDGET(ws->sidebar_list), "navigation-sidebar");

    /* Раздел «Медиатека»: альбомы, исполнители, жанры. */
    gtk_list_box_append(ws->sidebar_list,
                        ui_create_sidebar_section_header("Медиатека", NULL));
    GtkWidget *row_albums = ui_create_sidebar_row("albums", "Альбомы", "media-optical-symbolic");
    gtk_list_box_append(ws->sidebar_list, row_albums);
    gtk_list_box_append(ws->sidebar_list, ui_create_sidebar_row("artists", "Исполнители", "system-users-symbolic"));
    gtk_list_box_append(ws->sidebar_list, ui_create_sidebar_row("genres", "Жанры", "tag-symbolic"));

    /* Раздел «Треки»: «Все треки» и «Любимые треки» (строку «Любимых» вставляет
     * раздел плейлистов прямо под этим подзаголовком). */
    gtk_list_box_append(ws->sidebar_list,
                        ui_create_sidebar_section_header("Треки", NULL));
    gtk_list_box_append(ws->sidebar_list, ui_create_sidebar_row("tracks", "Все треки", "folder-music-symbolic"));

    /* Раздел «Плейлисты» с кнопкой создания справа. Роль строки-подзаголовка
     * отмечена в данных, чтобы раздел плейлистов знал, куда вставлять строки. */
    GtkWidget *create_btn = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_add_css_class(create_btn, "flat");
    gtk_widget_set_valign(create_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(create_btn, "Создать плейлист");
    g_signal_connect(create_btn, "clicked", G_CALLBACK(on_create_playlist_clicked), ws);
    GtkWidget *playlists_header = ui_create_sidebar_section_header("Плейлисты", create_btn);
    g_object_set_data(G_OBJECT(playlists_header), "sidebar-role", "playlists-header");
    gtk_list_box_append(ws->sidebar_list, playlists_header);

g_signal_connect(ws->sidebar_list, "row-selected", G_CALLBACK(on_sidebar_row_selected), ws);
track_menu_set_open_album_cb(on_track_open_album, ws);
track_menu_set_open_artist_cb(on_track_open_artist, ws);
track_menu_set_show_info_cb(on_track_show_info, ws);
/* Карточки «другие альбомы» и ссылки на исполнителей на странице альбома ведут
 * туда же, куда соответствующие пункты меню трека. */
album_detail_page_set_open_album_cb(on_track_open_album, ws);
album_detail_page_set_open_artist_cb(on_track_open_artist, ws);
album_detail_page_set_open_genre_cb(on_album_open_genre, ws);
album_detail_page_set_edit_tags_cb(on_album_edit_tags, ws);
gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar_scroll), GTK_WIDGET(ws->sidebar_list));
gtk_widget_set_vexpand(sidebar_scroll, TRUE);

adw_toolbar_view_set_content(sidebar_tv, sidebar_scroll);
AdwNavigationPage *sidebar_page = adw_navigation_page_new(GTK_WIDGET(sidebar_tv), "Музыка");
adw_overlay_split_view_set_sidebar(ws->split_view, GTK_WIDGET(sidebar_page));
/* ================= 2. ОСНОВНАЯ ОБЛАСТЬ КОНТЕНТА ================= */
ws->content_stack = GTK_STACK(gtk_stack_new());
gtk_stack_set_transition_type(ws->content_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    ws->library_view = library_view_new(player, ws->split_view);
    library_view_attach_to_stack(ws->library_view, ws->content_stack);
    /* Восстанавливаем вид браузера альбомов (размер элементов и сортировку). */
    {
        ViewControls *vc = library_view_get_view_controls(ws->library_view);
        if (vc) {
            view_controls_set_mode(vc, (ViewMode)ws->state->view_mode);
            view_controls_set_zoom(vc, (ViewZoom)ws->state->view_zoom);
            view_controls_set_sort(vc, (ViewSortField)ws->state->view_sort,
                                   ws->state->view_sort_desc);
        }
    }
ws->playlists_view = playlists_view_new(player, ws->split_view, ws->sidebar_list,
                                        ws->content_stack);
track_menu_set_playlist_cbs(on_is_favorite, on_toggle_favorite, on_list_playlists,
                            on_playlist_has, on_toggle_playlist, ws);
adw_overlay_split_view_set_content(ws->split_view, GTK_WIDGET(ws->content_stack));
/* ================= 3. ПЛЕЕР: НИЖНИЙ БАР + ПОЛНОЭКРАННАЯ ШТОРКА ================= */

/* Очередь — отдельная боковая панель справа. Разворачивается рядом с
 * контентом (collapsed=FALSE), сдвигая его, а не перекрывая. */
ws->queue_split_view = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
adw_overlay_split_view_set_sidebar_position(ws->queue_split_view, GTK_PACK_END);
adw_overlay_split_view_set_collapsed(ws->queue_split_view, FALSE);
adw_overlay_split_view_set_max_sidebar_width(ws->queue_split_view, 420);
/* Панель очереди показывается только по кнопке. pin-sidebar запрещает
 * AdwOverlaySplitView самому показывать её при снятии collapsed (иначе при
 * выходе из мобильного режима очередь открывалась бы сама). */
adw_overlay_split_view_set_pin_sidebar(ws->queue_split_view, TRUE);
/* Панель скрыта по умолчанию (в т.ч. при пустой очереди на старте). */
adw_overlay_split_view_set_show_sidebar(ws->queue_split_view, FALSE);
adw_overlay_split_view_set_content(ws->queue_split_view, GTK_WIDGET(ws->split_view));
ws->queue_view = queue_view_new(player);
adw_overlay_split_view_set_sidebar(ws->queue_split_view, queue_view_get_widget(ws->queue_view));

/* На узком окне панель очереди делаем накладной (collapsed). В режиме разделения
 * AdwOverlaySplitView вычитает минимум содержимого из доступной ширины и при
 * окне уже содержимого уходит в отрицательные размеры (предупреждения
 * «attempt to allocate ... with width -N»). Setter добавляем в общий брейкпоинт
 * (bp): он активируется вместе со сворачиванием основной панели. */
GValue queue_collapsed = G_VALUE_INIT;
g_value_init(&queue_collapsed, G_TYPE_BOOLEAN);
g_value_set_boolean(&queue_collapsed, TRUE);
adw_breakpoint_add_setter(bp, G_OBJECT(ws->queue_split_view), "collapsed", &queue_collapsed);
g_value_unset(&queue_collapsed);

/* В мобильном виде очередь прячется (там она раскрывается слишком тесно). */
g_signal_connect(bp, "apply", G_CALLBACK(on_mobile_breakpoint_apply), ws);

ws->bottom_sheet = ADW_BOTTOM_SHEET(adw_bottom_sheet_new());
/* Обёртка обнуляет минимальную ширину содержимого: иначе при открытой очереди
 * на узком окне AdwBottomSheet требует ширины больше доступной и ругается
 * («exceeds AdwApplicationWindow width»), потому что минимумы содержимого,
 * панели «Музыка» и панели очереди складываются. */
adw_bottom_sheet_set_content(ws->bottom_sheet,
                             ui_create_min_width_bin(GTK_WIDGET(ws->queue_split_view)));

/* Нижний бар рисуется поверх контента — сдвигаем контент вверх на его высоту,
 * чтобы бар не перекрывал вид. */
g_object_bind_property(ws->bottom_sheet, "bottom-bar-height",
                       ws->queue_split_view, "margin-bottom",
                       G_BINDING_SYNC_CREATE);

ws->player_bar = player_bar_new(player, PLAYER_BAR_VARIANT_BAR);
adw_bottom_sheet_set_bottom_bar(ws->bottom_sheet, player_bar_get_widget(ws->player_bar));

/* Кнопка в баре открывает/закрывает боковую панель очереди. */
GtkWidget *queue_btn = player_bar_get_queue_button(ws->player_bar);
if (queue_btn) {
    g_object_bind_property(ws->queue_split_view, "show-sidebar",
                           queue_btn, "active",
                           G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
}

ws->full_player = player_bar_new(player, PLAYER_BAR_VARIANT_FULLSCREEN);
adw_bottom_sheet_set_sheet(ws->bottom_sheet, player_bar_get_widget(ws->full_player));

/* Шторку можно развернуть тапом по нижнему бару и на десктопе тоже. */
adw_bottom_sheet_set_can_open(ws->bottom_sheet, TRUE);

/* Плеер переходит в компактный вид ровно тогда же, когда сворачивается сайдбар. */
GValue mobile_val = G_VALUE_INIT;
g_value_init(&mobile_val, G_TYPE_BOOLEAN);
g_value_set_boolean(&mobile_val, TRUE);
adw_breakpoint_add_setter(bp, G_OBJECT(player_bar_get_widget(ws->player_bar)), "mobile", &mobile_val);
g_value_unset(&mobile_val);

adw_application_window_set_content(win, GTK_WIDGET(ws->bottom_sheet));

/* Минимальная высота окна: плеер-шторка не рассчитана на совсем низкие окна.
 * Ширину 360 сохраняем (значение по умолчанию у AdwApplicationWindow). */
gtk_widget_set_size_request(GTK_WIDGET(win), 360, 400);

gtk_list_box_select_row(ws->sidebar_list, GTK_LIST_BOX_ROW(row_albums));
player_set_state_changed_cb(player, on_player_updated, ws);

g_signal_connect(win, "close-request", G_CALLBACK(on_window_close_request), ws);
if (ws->state->maximized) {
    gtk_window_maximize(GTK_WINDOW(win));
}

return GTK_WIDGET(win);
}