#include "track_list.h"
#include "ui_helpers.h"

/* ===== лёгкая обёртка над треком для модели =====
 * Пользуется тем же Track*, что и источник списка (объекты общие), поэтому
 * обёртка владеет только флагом подсветки и не копирует строки. */
#define TRACK_ITEM_TYPE (track_item_get_type())
G_DECLARE_FINAL_TYPE(TrackItem, track_item, KERUU, TRACK_ITEM, GObject)

struct _TrackItem {
    GObject parent_instance;
    Track *track;      /* заимствованный указатель */
    gboolean playing;
};

G_DEFINE_FINAL_TYPE(TrackItem, track_item, G_TYPE_OBJECT)

static void track_item_class_init(TrackItemClass *klass) {
    (void)klass;
}

static void track_item_init(TrackItem *self) {
    (void)self;
}

static TrackItem *track_item_new(Track *track) {
    TrackItem *item = g_object_new(TRACK_ITEM_TYPE, NULL);
    item->track = track;
    return item;
}

struct _TrackList {
    Player *player;
    TrackListConfig cfg;

    GPtrArray *tracks;    /* удерживаемая ссылка на массив треков */
    GHashTable *uri_index;/* uri -> индекс в tracks (значение = индекс + 1) */
    GListStore *store;
    GtkListView *view;
    GtkWidget *widget;    /* ScrolledWindow — корень для встраивания */

    int playing_index;

    GtkWidget *drop_row;  /* строка под индикатором вставки при перетаскивании */
    gboolean drop_above;
};

/* ===== оформление строк =====
 * В GtkListView нельзя использовать AdwActionRow (он рассчитан только на
 * GtkListBox), поэтому строка повторяет ВНУТРЕННЮЮ структуру AdwActionRow
 * (box.header → box.title → label.title/label.subtitle). Благодаря этому отступы,
 * высоту, типографику и разделители задаёт сама тема Adwaita: `.rich-list > row`
 * даёт padding/min-height, а `row > box.header > box.title > .title/.subtitle` —
 * оформление заголовка и подзаголовка. Своего padding у строки быть не должно —
 * иначе он складывается с padding темы. */
static void ensure_row_css(void) {
    static gboolean loaded = FALSE;
    if (loaded) return;
    loaded = TRUE;

    GtkCssProvider *css = gtk_css_provider_new();
    /* Полоска-индикатор места вставки рисуется фоном строки сверху или снизу и не
     * сдвигает вёрстку. Во время перетаскивания гасим подсветку и окантовку строк
     * (без !important: GTK CSS его не поддерживает). */
    gtk_css_provider_load_from_string(css,
        ".keruu-track-row image.keruu-playing-icon {"
        "  color: @accent_color;"
        "}"
        ".keruu-drop-above {"
        "  background-image: linear-gradient(to bottom, @accent_color 3px, transparent 3px);"
        "}"
        ".keruu-drop-below {"
        "  background-image: linear-gradient(to top, @accent_color 3px, transparent 3px);"
        "}"
        "listview.keruu-dragging > row {"
        "  box-shadow: none;"
        "  outline: none;"
        "  border-color: transparent;"
        "}"
        "listview.keruu-dragging > row:hover,"
        "listview.keruu-dragging > row:active {"
        "  background-color: transparent;"
        "}");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* ===== подсветка играющего трека ===== */

static void apply_playing_change(TrackList *tl, int index, gboolean playing) {
    if (index < 0 || index >= (int)g_list_model_get_n_items(G_LIST_MODEL(tl->store))) return;
    GObject *obj = g_list_model_get_item(G_LIST_MODEL(tl->store), index);
    if (!obj) return;
    KERUU_TRACK_ITEM(obj)->playing = playing;
    g_list_model_items_changed(G_LIST_MODEL(tl->store), index, 1, 1);
    g_object_unref(obj);
}

/* Переключает подсветку только двух изменившихся строк, а не всего списка. */
static void set_playing_index_internal(TrackList *tl, int new_index) {
    if (new_index == tl->playing_index) return;
    int old_index = tl->playing_index;
    tl->playing_index = new_index;
    apply_playing_change(tl, old_index, FALSE);
    apply_playing_change(tl, new_index, TRUE);
}

/* ===== перетаскивание для смены порядка ===== */

static void clear_drop_indicator(TrackList *tl) {
    if (!tl->drop_row) return;
    gtk_widget_remove_css_class(tl->drop_row, "keruu-drop-above");
    gtk_widget_remove_css_class(tl->drop_row, "keruu-drop-below");
    tl->drop_row = NULL;
}

static void set_drop_indicator(TrackList *tl, GtkWidget *row, gboolean above) {
    if (tl->drop_row == row && tl->drop_above == above) return;
    clear_drop_indicator(tl);
    if (!row) return;
    tl->drop_row = row;
    tl->drop_above = above;
    gtk_widget_add_css_class(row, above ? "keruu-drop-above" : "keruu-drop-below");
}

/* Позиция вставки (0..N) по координате y внутри ListView. У GtkListView нет
 * аналога gtk_list_box_get_row_at_y, поэтому обходим только реализованные (то
 * есть видимые) строки и сравниваем с их серединой. */
static int drop_before_at(TrackList *tl, double y, GtkWidget **out_mark, gboolean *out_above) {
    int count = (int)g_list_model_get_n_items(G_LIST_MODEL(tl->store));
    GtkWidget *last_row = NULL;
    int last_pos = 0;

    for (GtkWidget *c = gtk_widget_get_first_child(GTK_WIDGET(tl->view));
         c; c = gtk_widget_get_next_sibling(c)) {
        GtkWidget *row = gtk_widget_get_first_child(c);
        if (!row) continue;
        gpointer pv = g_object_get_data(G_OBJECT(row), "pos");
        if (!pv) continue;
        int pos = (int)GPOINTER_TO_UINT(pv) - 1;
        last_row = row;
        last_pos = pos;

        graphene_rect_t b;
        if (!gtk_widget_compute_bounds(row, GTK_WIDGET(tl->view), &b)) continue;
        if (y < b.origin.y + b.size.height / 2.0) {
            if (out_mark) { *out_mark = row; *out_above = TRUE; }
            return pos;
        }
    }

    if (out_mark) { *out_mark = last_row; *out_above = FALSE; }
    return last_row ? last_pos + 1 : count;
}

static GdkContentProvider *on_row_drag_prepare(GtkDragSource *source, double x,
                                               double y, gpointer user_data) {
    (void)source; (void)x; (void)y;
    GtkWidget *row = user_data;
    gpointer pv = g_object_get_data(G_OBJECT(row), "pos");
    if (!pv) return NULL;

    GValue val = G_VALUE_INIT;
    g_value_init(&val, G_TYPE_INT);
    g_value_set_int(&val, (int)GPOINTER_TO_UINT(pv) - 1);
    GdkContentProvider *provider = gdk_content_provider_new_for_value(&val);
    g_value_unset(&val);
    return provider;
}

static void on_row_drag_begin(GtkDragSource *source, GdkDrag *drag, gpointer user_data) {
    (void)source; (void)drag;
    TrackList *tl = user_data;
    gtk_widget_add_css_class(GTK_WIDGET(tl->view), "keruu-dragging");
}

static void on_row_drag_end(GtkDragSource *source, GdkDrag *drag,
                            gboolean delete_data, gpointer user_data) {
    (void)source; (void)drag; (void)delete_data;
    TrackList *tl = user_data;
    gtk_widget_remove_css_class(GTK_WIDGET(tl->view), "keruu-dragging");
    clear_drop_indicator(tl);
}

static GdkDragAction on_drop_motion(GtkDropTarget *target, double x, double y,
                                    gpointer user_data) {
    (void)target; (void)x;
    TrackList *tl = user_data;
    GtkWidget *mark = NULL;
    gboolean above = FALSE;
    drop_before_at(tl, y, &mark, &above);
    set_drop_indicator(tl, mark, above);
    return GDK_ACTION_MOVE;
}

static void on_drop_leave(GtkDropTarget *target, gpointer user_data) {
    (void)target;
    clear_drop_indicator(user_data);
}

static gboolean on_drop(GtkDropTarget *target, const GValue *value,
                        double x, double y, gpointer user_data) {
    (void)target; (void)x;
    TrackList *tl = user_data;
    if (!tl->cfg.on_reorder) return FALSE;

    int from = g_value_get_int(value);
    int before = drop_before_at(tl, y, NULL, NULL);
    /* before — позиция в исходном списке; после извлечения элемента сдвигается. */
    int to = (from < before) ? before - 1 : before;

    clear_drop_indicator(tl);
    tl->cfg.on_reorder(tl, from, to, tl->cfg.user_data);
    return TRUE;
}

static void attach_reorder(GtkWidget *row, TrackList *tl) {
    GtkDragSource *source = gtk_drag_source_new();
    gtk_drag_source_set_actions(source, GDK_ACTION_MOVE);
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(source), GDK_BUTTON_PRIMARY);
    g_signal_connect(source, "prepare", G_CALLBACK(on_row_drag_prepare), row);
    g_signal_connect(source, "drag-begin", G_CALLBACK(on_row_drag_begin), tl);
    g_signal_connect(source, "drag-end", G_CALLBACK(on_row_drag_end), tl);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(source));
}

/* ===== фабрика строк ===== */

static void set_row_cover(GtkWidget *row, const char *cover_path) {
    GtkWidget *slot = g_object_get_data(G_OBJECT(row), "cover-slot");
    if (!slot) return;
    GtkWidget *old = gtk_widget_get_first_child(slot);
    if (old) gtk_box_remove(GTK_BOX(slot), old);
    gtk_box_append(GTK_BOX(slot),
                   ui_create_cover_widget(cover_path ? cover_path : "", UI_ROW_COVER_SIZE));
}

static void row_set_playing(GtkWidget *row, gboolean playing) {
    GtkWidget *icon = g_object_get_data(G_OBJECT(row), "playing-icon");
    if (icon) gtk_widget_set_visible(icon, playing);
    if (playing) {
        gtk_widget_add_css_class(row, "accent");
    } else {
        gtk_widget_remove_css_class(row, "accent");
    }
}

static void on_row_setup(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory;
    TrackList *tl = user_data;

    ensure_row_css();

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    /* class "header" + структура ниже повторяют AdwActionRow, чтобы тему строки
     * рисовала Adwaita (см. комментарий у ensure_row_css). */
    gtk_widget_add_css_class(row, "header");
    gtk_widget_add_css_class(row, "keruu-track-row");
    gtk_widget_set_hexpand(row, TRUE);

    /* Префиксы: обложка и индикатор воспроизведения (как в AdwActionRow). */
    GtkWidget *prefixes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(prefixes, "prefixes");
    gtk_widget_set_valign(prefixes, GTK_ALIGN_CENTER);

    GtkWidget *cover_slot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_valign(cover_slot, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(prefixes), cover_slot);
    g_object_set_data(G_OBJECT(row), "cover-slot", cover_slot);

    GtkWidget *playing_icon = gtk_image_new_from_icon_name("media-playback-start-symbolic");
    gtk_widget_add_css_class(playing_icon, "keruu-playing-icon");
    gtk_widget_set_valign(playing_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(playing_icon, FALSE);
    gtk_box_append(GTK_BOX(prefixes), playing_icon);
    g_object_set_data(G_OBJECT(row), "playing-icon", playing_icon);
    gtk_box_append(GTK_BOX(row), prefixes);

    /* Заголовок и подзаголовок: box.header > box.title > label.title/.subtitle. */
    GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(title_box, "title");
    gtk_widget_set_valign(title_box, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(title_box, TRUE);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_widget_add_css_class(title, "title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);

    GtkWidget *sub = gtk_label_new(NULL);
    gtk_widget_add_css_class(sub, "subtitle");
    gtk_label_set_xalign(GTK_LABEL(sub), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);

    gtk_box_append(GTK_BOX(title_box), title);
    gtk_box_append(GTK_BOX(title_box), sub);
    gtk_box_append(GTK_BOX(row), title_box);
    g_object_set_data(G_OBJECT(row), "title-label", title);
    g_object_set_data(G_OBJECT(row), "sub-label", sub);

    /* Суффиксы: сердечко и меню (как в AdwActionRow). */
    GtkWidget *suffix = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(suffix, "suffixes");
    gtk_widget_set_valign(suffix, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), suffix);
    track_menu_attach_row(row, suffix, tl->cfg.context, tl->player);

    if (tl->cfg.reorderable) attach_reorder(row, tl);

    gtk_list_item_set_child(li, row);
}

static void on_row_bind(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory;
    TrackList *tl = user_data;

    TrackItem *item = gtk_list_item_get_item(li);
    GtkWidget *row = gtk_list_item_get_child(li);
    Track *t = item->track;
    if (!t) return;

    /* Индекс строки нужен при перетаскивании: у GtkListView нет обратного
     * отображения виджета в позицию модели. */
    g_object_set_data(G_OBJECT(row), "pos",
                      GUINT_TO_POINTER(gtk_list_item_get_position(li) + 1));

    GtkWidget *title = g_object_get_data(G_OBJECT(row), "title-label");
    GtkWidget *sub = g_object_get_data(G_OBJECT(row), "sub-label");

    gtk_label_set_text(GTK_LABEL(title), t->title ? t->title : "");

    char *sub_text;
    if (tl->cfg.subtitle_artist_only) {
        sub_text = g_strdup(t->artist ? t->artist : "");
    } else {
        sub_text = g_strdup_printf("%s • %s",
                                   t->artist ? t->artist : "",
                                   t->album ? t->album : "");
    }
    gtk_label_set_text(GTK_LABEL(sub), sub_text);
    g_free(sub_text);

    set_row_cover(row, t->cover_path);
    track_menu_row_set_track(row, t);
    row_set_playing(row, item->playing);
}

static void on_row_unbind(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory;
    (void)user_data;
    GtkWidget *row = gtk_list_item_get_child(li);
    if (!row) return;

    /* Снимаем обложку и данные, чтобы переиспользованная строка не показывала
     * чужой трек, и освобождаем ожидание асинхронной загрузки обложки. */
    GtkWidget *slot = g_object_get_data(G_OBJECT(row), "cover-slot");
    if (slot) {
        GtkWidget *old = gtk_widget_get_first_child(slot);
        if (old) gtk_box_remove(GTK_BOX(slot), old);
    }
    track_menu_row_set_track(row, NULL);
    row_set_playing(row, FALSE);
}

static void on_activate(GtkListView *view, guint position, gpointer user_data) {
    (void)view;
    TrackList *tl = user_data;
    if (tl->cfg.on_activate) {
        tl->cfg.on_activate(tl, position, tl->cfg.user_data);
        return;
    }
    if (!tl->tracks || position >= tl->tracks->len) return;
    player_set_queue_and_play(tl->player, tl->tracks, (int)position);
}

/* ===== публичный интерфейс ===== */

/* Состояние списка освобождается при уничтожении его корневого виджета: так оно
 * живёт ровно столько же, сколько строки-фабрики, ссылающиеся на него. */
static void track_list_destroy(gpointer data) {
    TrackList *tl = data;
    if (tl->tracks) g_ptr_array_unref(tl->tracks);
    if (tl->uri_index) g_hash_table_destroy(tl->uri_index);
    g_clear_object(&tl->store);
    g_free(tl);
}

TrackList *track_list_new(Player *player, const TrackListConfig *config) {
    TrackList *tl = g_new0(TrackList, 1);
    tl->player = player;
    tl->cfg = *config;
    tl->playing_index = -1;
    tl->store = g_list_store_new(TRACK_ITEM_TYPE);

    GtkNoSelection *selection =
        gtk_no_selection_new(G_LIST_MODEL(g_object_ref(tl->store)));
    tl->view = GTK_LIST_VIEW(gtk_list_view_new(GTK_SELECTION_MODEL(selection), NULL));
    gtk_list_view_set_single_click_activate(tl->view, TRUE);
    gtk_list_view_set_show_separators(tl->view, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(tl->view), "rich-list");
    gtk_widget_set_vexpand(GTK_WIDGET(tl->view), TRUE);

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(on_row_setup), tl);
    g_signal_connect(factory, "bind", G_CALLBACK(on_row_bind), tl);
    g_signal_connect(factory, "unbind", G_CALLBACK(on_row_unbind), tl);
    gtk_list_view_set_factory(tl->view, factory);
    g_object_unref(factory);

    g_signal_connect(tl->view, "activate", G_CALLBACK(on_activate), tl);

    if (tl->cfg.reorderable) {
        GtkDropTarget *target = gtk_drop_target_new(G_TYPE_INT, GDK_ACTION_MOVE);
        g_signal_connect(target, "motion", G_CALLBACK(on_drop_motion), tl);
        g_signal_connect(target, "leave", G_CALLBACK(on_drop_leave), tl);
        g_signal_connect(target, "drop", G_CALLBACK(on_drop), tl);
        gtk_widget_add_controller(GTK_WIDGET(tl->view), GTK_EVENT_CONTROLLER(target));
    }

    /* Обёртка как в ui_create_scrolled_list: прокрутка + ограничение ширины.
     * AdwClampScrollable ограничивает ширину ListView, сохраняя его прокрутку и
     * виртуализацию (строки создаются только для видимой области). */
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *clamp = adw_clamp_scrollable_new();
    adw_clamp_scrollable_set_maximum_size(ADW_CLAMP_SCROLLABLE(clamp), 800);
    gtk_widget_set_margin_top(clamp, 16);
    gtk_widget_set_margin_bottom(clamp, 16);
    gtk_widget_set_margin_start(clamp, 12);
    gtk_widget_set_margin_end(clamp, 12);
    adw_clamp_scrollable_set_child(ADW_CLAMP_SCROLLABLE(clamp), GTK_WIDGET(tl->view));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), clamp);

    tl->widget = scrolled;
    g_object_set_data_full(G_OBJECT(tl->widget), "track-list", tl, track_list_destroy);
    return tl;
}

GtkWidget *track_list_get_widget(TrackList *tl) {
    return tl->widget;
}

void track_list_set_tracks(TrackList *tl, GPtrArray *tracks) {
    if (!tl) return;

    if (tl->tracks) g_ptr_array_unref(tl->tracks);
    tl->tracks = tracks ? g_ptr_array_ref(tracks) : NULL;
    tl->playing_index = -1;
    tl->drop_row = NULL; /* строки уничтожаются — ссылка на индикатор недействительна */

    if (tl->uri_index) {
        g_hash_table_destroy(tl->uri_index);
        tl->uri_index = NULL;
    }

    g_list_store_remove_all(tl->store);
    if (!tracks || tracks->len == 0) return;

    /* Индекс по uri для подсветки играющего трека за O(1): линейный поиск по
     * всей медиатеке на каждую смену трека не должен зависеть от её размера. */
    tl->uri_index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < tracks->len; i++) {
        Track *t = g_ptr_array_index(tracks, i);
        if (t->uri && !g_hash_table_contains(tl->uri_index, t->uri)) {
            /* Значение — индекс + 1, чтобы 0 отличать от «не найдено». */
            g_hash_table_insert(tl->uri_index, g_strdup(t->uri),
                                GINT_TO_POINTER((gint)i + 1));
        }
    }

    /* Один splice вместо append на каждый элемент: append дёргает перепривязку
     * вьюх и сортировку заново (квадратичная сложность на больших списках). */
    GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
    for (guint i = 0; i < tracks->len; i++) {
        g_ptr_array_add(items, track_item_new(g_ptr_array_index(tracks, i)));
    }
    g_list_store_splice(tl->store, 0, 0, items->pdata, items->len);
    g_ptr_array_unref(items);
}

void track_list_set_playing_uri(TrackList *tl, const char *uri) {
    if (!tl) return;

    int new_index = -1;
    if (uri && tl->uri_index) {
        gpointer value = g_hash_table_lookup(tl->uri_index, uri);
        if (value) new_index = GPOINTER_TO_INT(value) - 1;
    }
    set_playing_index_internal(tl, new_index);
}

void track_list_set_playing_index(TrackList *tl, int index) {
    if (!tl) return;
    int count = (int)g_list_model_get_n_items(G_LIST_MODEL(tl->store));
    if (index < 0 || index >= count) index = -1;
    set_playing_index_internal(tl, index);
}
