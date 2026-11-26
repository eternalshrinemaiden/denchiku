#include "track_list.h"
#include "ui_helpers.h"

/* ===== лёгкая обёртка над треком для модели =====
 * Пользуется тем же Track*, что и источник списка (объекты общие), поэтому
 * обёртка владеет только флагом подсветки и не копирует строки.
 *
 * Флаг подсветки — GObject-свойство: ячейка связывается с ним и обновляет вид
 * без полной перепривязки списка (в GTK 4.12+ items-changed для того же элемента
 * не вызывает повторный bind). */
#define TRACK_ITEM_TYPE (track_item_get_type())
G_DECLARE_FINAL_TYPE(TrackItem, track_item, DENCHIKU, TRACK_ITEM, GObject)

struct _TrackItem {
    GObject parent_instance;
    Track *track;      /* заимствованный указатель */
    gboolean playing;
};

enum {
    ITEM_PROP_0,
    ITEM_PROP_PLAYING,
    ITEM_N_PROPS
};
static GParamSpec *item_props[ITEM_N_PROPS];

G_DEFINE_FINAL_TYPE(TrackItem, track_item, G_TYPE_OBJECT)

static void track_item_set_property(GObject *obj, guint prop_id,
                                    const GValue *value, GParamSpec *pspec) {
    TrackItem *self = DENCHIKU_TRACK_ITEM(obj);
    switch (prop_id) {
        case ITEM_PROP_PLAYING: self->playing = g_value_get_boolean(value); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
    }
}

static void track_item_get_property(GObject *obj, guint prop_id,
                                    GValue *value, GParamSpec *pspec) {
    TrackItem *self = DENCHIKU_TRACK_ITEM(obj);
    switch (prop_id) {
        case ITEM_PROP_PLAYING: g_value_set_boolean(value, self->playing); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
    }
}

static void track_item_class_init(TrackItemClass *klass) {
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->set_property = track_item_set_property;
    oc->get_property = track_item_get_property;

    item_props[ITEM_PROP_PLAYING] =
        g_param_spec_boolean("playing", NULL, NULL, FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_NAME);
    g_object_class_install_properties(oc, ITEM_N_PROPS, item_props);
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
    GtkColumnView *view;
    GtkWidget *widget;    /* ScrolledWindow — корень для встраивания */

    GtkColumnViewColumn *cover_col;  /* безымянный столбец обложек (фикс. ширина) */
    GtkColumnViewColumn *title_col;  /* для синхронизации сортировки с контролем */
    GtkColumnViewColumn *artist_col;
    GtkColumnViewColumn *year_col;

    int playing_index;

    ViewZoom last_zoom;
    gboolean syncing_sort;

    GtkWidget *drop_row;  /* строка под индикатором вставки при перетаскивании */
    gboolean drop_above;
};

/* Размер обложки строки берём из панели управления видом (как в списках
 * альбомов/исполнителей), иначе — общее значение по умолчанию. */
static int track_cover_size(TrackList *tl) {
    if (tl->cfg.controls) return view_controls_get_list_cover_size(tl->cfg.controls);
    return UI_ROW_COVER_SIZE;
}

/* Класс размера строк для CSS: высота строки задаётся через min-height, поэтому
 * она меняется без перепривязки ячеек. */
static const char *zoom_class(ViewZoom z) {
    switch (z) {
        case VIEW_ZOOM_SMALL: return "denchiku-zoom-small";
        case VIEW_ZOOM_LARGE: return "denchiku-zoom-large";
        default:              return "denchiku-zoom-medium";
    }
}

/* ===== оформление =====
 * Подробный список — таблица (.data-table), как у прочих разделов медиатеки.
 * Компактный список — «богатые» строки (.rich-list). Индикатор места вставки при
 * перетаскивании рисуется фоном строки сверху/снизу и не сдвигает вёрстку.
 * У компактного списка заголовок столбца полностью убирается (в ColumnView нельзя
 * отключить заголовок API, поэтому обнуляем его и содержимое через CSS). */
static void ensure_row_css(void) {
    static gboolean loaded = FALSE;
    if (loaded) return;
    loaded = TRUE;

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        ".denchiku-playing-icon { color: @accent_color; }"
        ".denchiku-playing-title { color: @accent_color; }"
        ".denchiku-drop-above {"
        "  background-image: linear-gradient(to bottom, @accent_color 3px, transparent 3px);"
        "}"
        ".denchiku-drop-below {"
        "  background-image: linear-gradient(to top, @accent_color 3px, transparent 3px);"
        "}"
        "columnview.denchiku-dragging row {"
        "  box-shadow: none;"
        "  outline: none;"
        "  border-color: transparent;"
        "}"
        "columnview.denchiku-dragging row:hover,"
        "columnview.denchiku-dragging row:active {"
        "  background-color: transparent;"
        "}"
        "columnview.denchiku-compact > header {"
        "  min-height: 0; padding: 0; margin: 0;"
        "  border: none; background: none; box-shadow: none;"
        "}"
        /* Высота строки: базовая для очереди и по зуму для подробного списка.
         * Кнопки в суффиксе выше строки-минимума по умолчанию, поэтому задаём
         * нижнюю границу, иначе содержимое обрезается (у ячейки overflow:hidden). */
        "columnview.denchiku-tracks row { min-height: 48px; }"
        "columnview.denchiku-zoom-small row { min-height: 48px; }"
        "columnview.denchiku-zoom-medium row { min-height: 80px; }"
        "columnview.denchiku-zoom-large row { min-height: 112px; }");
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
    /* Свойство уведомляет только привязанные в данный момент ячейки. */
    g_object_set(obj, "playing", playing, NULL);
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

/* ===== обложка и подсветка строки ===== */

static void set_cover(GtkWidget *slot, const char *cover_path, int size) {
    if (!slot) return;
    GtkWidget *old = gtk_widget_get_first_child(slot);
    if (old) { ui_detach_cover(old); gtk_box_remove(GTK_BOX(slot), old); }
    gtk_box_append(GTK_BOX(slot),
                   ui_create_cover_widget(cover_path ? cover_path : "", size));
}

static void row_set_playing(GtkWidget *title, GtkWidget *icon, gboolean playing) {
    if (icon) gtk_widget_set_visible(icon, playing);
    if (title) {
        if (playing) {
            gtk_widget_add_css_class(title, "denchiku-playing-title");
        } else {
            gtk_widget_remove_css_class(title, "denchiku-playing-title");
        }
    }
}

/* Подсветку обновляем через свойство элемента ("playing"): items-changed в
 * GTK 4.12+ не вызывает повторный bind для того же элемента. Связь снимаем при
 * отвязке ячейки (модель отвязывает элементы раньше, чем освобождает их). */
static void on_item_playing_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    GtkWidget *box = user_data;
    row_set_playing(g_object_get_data(G_OBJECT(box), "title-label"),
                    g_object_get_data(G_OBJECT(box), "playing-icon"),
                    DENCHIKU_TRACK_ITEM(obj)->playing);
}

static void playing_handler_unbind(GtkWidget *box) {
    GObject *item = g_object_get_data(G_OBJECT(box), "playing-item");
    gulong id = (gulong)GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(box), "playing-handler"));
    if (item && id) g_signal_handler_disconnect(item, id);
    g_object_set_data(G_OBJECT(box), "playing-item", NULL);
    g_object_set_data(G_OBJECT(box), "playing-handler", NULL);
}

static void playing_handler_bind(GtkWidget *box, TrackItem *item) {
    playing_handler_unbind(box);
    gulong id = g_signal_connect_object(G_OBJECT(item), "notify::playing",
                                        G_CALLBACK(on_item_playing_changed), box, 0);
    g_object_set_data(G_OBJECT(box), "playing-item", item);
    g_object_set_data(G_OBJECT(box), "playing-handler", GUINT_TO_POINTER((guint)id));
}

static char *format_duration(guint seconds) {
    if (seconds == 0) return g_strdup("—");
    guint h = seconds / 3600;
    guint m = (seconds % 3600) / 60;
    guint s = seconds % 60;
    if (h > 0) return g_strdup_printf("%u:%02u:%02u", h, m, s);
    return g_strdup_printf("%u:%02u", m, s);
}

/* ===== сортировка ===== */

static int cmp_title(gconstpointer a, gconstpointer b, gpointer user_data) {
    (void)user_data;
    const Track *t1 = DENCHIKU_TRACK_ITEM((gpointer)a)->track;
    const Track *t2 = DENCHIKU_TRACK_ITEM((gpointer)b)->track;
    return g_utf8_collate(t1->title ? t1->title : "", t2->title ? t2->title : "");
}

static int cmp_artist(gconstpointer a, gconstpointer b, gpointer user_data) {
    (void)user_data;
    const Track *t1 = DENCHIKU_TRACK_ITEM((gpointer)a)->track;
    const Track *t2 = DENCHIKU_TRACK_ITEM((gpointer)b)->track;
    return g_utf8_collate(t1->artist ? t1->artist : "", t2->artist ? t2->artist : "");
}

static int cmp_album(gconstpointer a, gconstpointer b, gpointer user_data) {
    (void)user_data;
    const Track *t1 = DENCHIKU_TRACK_ITEM((gpointer)a)->track;
    const Track *t2 = DENCHIKU_TRACK_ITEM((gpointer)b)->track;
    return g_utf8_collate(t1->album ? t1->album : "", t2->album ? t2->album : "");
}

static int cmp_year(gconstpointer a, gconstpointer b, gpointer user_data) {
    (void)user_data;
    guint y1 = DENCHIKU_TRACK_ITEM((gpointer)a)->track->year;
    guint y2 = DENCHIKU_TRACK_ITEM((gpointer)b)->track->year;
    if (y1 == y2) return 0;
    if (y1 == 0) return 1;  /* неизвестный год — в конец */
    if (y2 == 0) return -1;
    return y1 < y2 ? -1 : 1;
}

static int cmp_duration(gconstpointer a, gconstpointer b, gpointer user_data) {
    (void)user_data;
    guint d1 = DENCHIKU_TRACK_ITEM((gpointer)a)->track->duration;
    guint d2 = DENCHIKU_TRACK_ITEM((gpointer)b)->track->duration;
    if (d1 == d2) return 0;
    if (d1 == 0) return 1;  /* неизвестная длительность — в конец */
    if (d2 == 0) return -1;
    return d1 < d2 ? -1 : 1;
}

/* ===== перетаскивание для смены порядка ===== */

static void clear_drop_indicator(TrackList *tl) {
    if (!tl->drop_row) return;
    gtk_widget_remove_css_class(tl->drop_row, "denchiku-drop-above");
    gtk_widget_remove_css_class(tl->drop_row, "denchiku-drop-below");
    tl->drop_row = NULL;
}

/* Индикатор вставки вешаем на собственное содержимое строки: фон внутренней
 * строки GtkColumnView перекрыт ячейками и был бы невидим. */
static void set_drop_indicator(TrackList *tl, GtkWidget *row, gboolean above) {
    GtkWidget *target = row ? g_object_get_data(G_OBJECT(row), "content-box") : NULL;
    if (!target) target = row;
    if (tl->drop_row == target && tl->drop_above == above) return;
    clear_drop_indicator(tl);
    if (!target) return;
    tl->drop_row = target;
    tl->drop_above = above;
    gtk_widget_add_css_class(target, above ? "denchiku-drop-above" : "denchiku-drop-below");
}

/* Контейнер со строками: внутри GtkColumnView это встроенный GtkListView
 * (заголовок — отдельный ребёнок, его пропускаем). */
static GtkWidget *rows_container(TrackList *tl) {
    for (GtkWidget *c = gtk_widget_get_first_child(GTK_WIDGET(tl->view));
         c; c = gtk_widget_get_next_sibling(c)) {
        if (GTK_IS_LIST_VIEW(c)) return c;
    }
    return NULL;
}

/* Заголовок столбцов — отдельный ребёнок ColumnView (не встроенный ListView). */
static GtkWidget *column_header_widget(GtkColumnView *view) {
    for (GtkWidget *c = gtk_widget_get_first_child(GTK_WIDGET(view));
         c; c = gtk_widget_get_next_sibling(c)) {
        if (!GTK_IS_LIST_VIEW(c)) return c;
    }
    return NULL;
}

/* У компактного списка заголовок не нужен: у GtkColumnView нет API, чтобы его
 * убрать, а CSS не может обнулить высоту пустой подписи столбца, поэтому скрываем
 * сами виджеты заголовков (тогда заголовок ужимается до нуля). */
static void hide_column_headers(GtkColumnView *view) {
    GtkWidget *header = column_header_widget(view);
    for (GtkWidget *h = header ? gtk_widget_get_first_child(header) : NULL;
         h; h = gtk_widget_get_next_sibling(h)) {
        gtk_widget_set_visible(h, FALSE);
    }
}

/* В шапке столбца показываем значок вместо текста. Публичного API для виджета
 * заголовка нет, поэтому находим сам виджет подписи столбца во внутренней строке
 * заголовка и подставляем значок (при неудаче текст остаётся). Сам заголовок всё
 * равно остаётся кликабельным для сортировки. */
static void set_column_header_icon(GtkColumnView *view, GtkColumnViewColumn *column,
                                   const char *icon_name) {
    GtkWidget *header = column_header_widget(view);
    if (!header) return;

    GListModel *cols = gtk_column_view_get_columns(view);
    guint n = g_list_model_get_n_items(cols);
    guint idx = G_MAXUINT;
    for (guint i = 0; i < n; i++) {
        GObject *o = g_list_model_get_item(cols, i);
        if (o == G_OBJECT(column)) idx = i;
        g_object_unref(o);
    }
    if (idx == G_MAXUINT) return;

    GtkWidget *title = gtk_widget_get_first_child(header);
    for (guint i = 0; i < idx && title; i++) {
        title = gtk_widget_get_next_sibling(title);
    }
    if (!title) return;

    GtkWidget *box = gtk_widget_get_first_child(title);
    if (!box || !GTK_IS_BOX(box)) return;

    GtkWidget *label = NULL;
    for (GtkWidget *w = gtk_widget_get_first_child(box); w; w = gtk_widget_get_next_sibling(w)) {
        if (GTK_IS_LABEL(w)) { label = w; break; }
    }
    if (!label) return;

    GtkWidget *img = gtk_image_new_from_icon_name(icon_name);
    gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), img);
    gtk_column_view_column_set_title(column, ""); /* пустой текст, остаётся значок */
}

/* Содержимое строки (наш виджет) — первый ребёнок первой ячейки строки: через
 * него надёжно достаём позицию строки (ключ "pos", значение = позиция + 1), не
 * полагаясь на gtk_widget_pick (тот во время перетаскивания может не сработать). */
static GtkWidget *row_content_box(GtkWidget *row) {
    GtkWidget *cell = gtk_widget_get_first_child(row);
    return cell ? gtk_widget_get_first_child(cell) : NULL;
}

static gpointer row_pos(GtkWidget *row) {
    GtkWidget *box = row_content_box(row);
    if (box) {
        gpointer p = g_object_get_data(G_OBJECT(box), "pos");
        if (p) return p;
    }
    return g_object_get_data(G_OBJECT(row), "pos");
}

/* Позиция вставки (0..N) по координате y. Обходим реализованные строки и
 * сравниваем y с их серединой; позиция берётся из содержимого строки. */
static int drop_before_at(TrackList *tl, double x, double y,
                          GtkWidget **out_mark, gboolean *out_above) {
    (void)x;
    int count = (int)g_list_model_get_n_items(G_LIST_MODEL(tl->store));
    GtkWidget *list = rows_container(tl);

    GtkWidget *last_box = NULL;
    int last_pos = -1;
    for (GtkWidget *row = list ? gtk_widget_get_first_child(list) : NULL;
         row; row = gtk_widget_get_next_sibling(row)) {
        gpointer pv = row_pos(row);
        if (!pv) continue;
        int pos = (int)GPOINTER_TO_UINT(pv) - 1;
        GtkWidget *box = row_content_box(row);
        last_box = box ? box : row;
        last_pos = pos;

        graphene_rect_t b;
        if (!gtk_widget_compute_bounds(row, GTK_WIDGET(tl->view), &b)) continue;
        if (y < b.origin.y + b.size.height / 2.0) {
            if (out_mark) { *out_mark = last_box; *out_above = TRUE; }
            return pos;
        }
    }

    if (out_mark) { *out_mark = last_box; *out_above = FALSE; }
    return last_pos >= 0 ? last_pos + 1 : count;
}

static GdkContentProvider *on_row_drag_prepare(GtkDragSource *source, double x,
                                               double y, gpointer user_data) {
    (void)source; (void)x; (void)y;
    GtkWidget *box = user_data;
    gpointer pv = g_object_get_data(G_OBJECT(box), "pos");
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
    gtk_widget_add_css_class(GTK_WIDGET(tl->view), "denchiku-dragging");
}

static void on_row_drag_end(GtkDragSource *source, GdkDrag *drag,
                            gboolean delete_data, gpointer user_data) {
    (void)source; (void)drag; (void)delete_data;
    TrackList *tl = user_data;
    gtk_widget_remove_css_class(GTK_WIDGET(tl->view), "denchiku-dragging");
    clear_drop_indicator(tl);
}

static GdkDragAction on_drop_motion(GtkDropTarget *target, double x, double y,
                                    gpointer user_data) {
    (void)target;
    TrackList *tl = user_data;
    GtkWidget *mark = NULL;
    gboolean above = FALSE;
    drop_before_at(tl, x, y, &mark, &above);
    set_drop_indicator(tl, mark, above);
    return GDK_ACTION_MOVE;
}

static void on_drop_leave(GtkDropTarget *target, gpointer user_data) {
    (void)target;
    clear_drop_indicator(user_data);
}

static gboolean on_drop(GtkDropTarget *target, const GValue *value,
                        double x, double y, gpointer user_data) {
    (void)target;
    TrackList *tl = user_data;
    if (!tl->cfg.on_reorder) return FALSE;

    int from = g_value_get_int(value);
    int before = drop_before_at(tl, x, y, NULL, NULL);
    /* before — позиция в исходном списке; после извлечения элемента сдвигается. */
    int to = (from < before) ? before - 1 : before;

    clear_drop_indicator(tl);
    tl->cfg.on_reorder(tl, from, to, tl->cfg.user_data);
    return TRUE;
}

/* Источник перетаскивания вешаем на наше содержимое строки (box): сама
 * внутренняя строка GtkColumnView нам не принадлежит. */
static void attach_reorder(GtkWidget *box, TrackList *tl) {
    GtkDragSource *source = gtk_drag_source_new();
    gtk_drag_source_set_actions(source, GDK_ACTION_MOVE);
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(source), GDK_BUTTON_PRIMARY);
    g_signal_connect(source, "prepare", G_CALLBACK(on_row_drag_prepare), box);
    g_signal_connect(source, "drag-begin", G_CALLBACK(on_row_drag_begin), tl);
    g_signal_connect(source, "drag-end", G_CALLBACK(on_row_drag_end), tl);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(source));
}

/* ===== фабрики ячеек ===== */

/* Внутренняя строка GtkColumnView (её виджет — прямой ребёнок встроенного
 * GtkListView) не принадлежит приложению, но её можно найти, поднимаясь от
 * нашего содержимого ячейки: box → виджет ячейки → виджет строки. Нужна только
 * для того, чтобы повесить триггеры меню (ПКМ и долгое нажатие) на всю ширину
 * строки. В момент первого bind строка может быть ещё не вставлена в дерево, так
 * что вызывать её отсюда нельзя — иначе часть строк останется без кнопок. */
static GtkWidget *row_widget_of(GtkWidget *child) {
    for (GtkWidget *w = child; w; w = gtk_widget_get_parent(w)) {
        GtkWidget *parent = gtk_widget_get_parent(w);
        if (parent && GTK_IS_LIST_VIEW(parent)) return w;
    }
    return NULL;
}

/* Триггеры меню по всей ширине строки вешаем на внутреннюю строку GtkColumnView,
 * но она не принадлежит приложению и в момент первого bind может ещё не быть
 * вставлена в дерево. Поэтому недостающие триггеры доставляет обработчик первого
 * отображения содержимого строки: к этому моменту строка гарантированно в списке. */
static void sync_row_triggers(GtkWidget *box) {
    GtkWidget *menu_btn = g_object_get_data(G_OBJECT(box), "menu-button");
    if (!menu_btn) return;

    GtkWidget *row = row_widget_of(box);
    if (!row || g_object_get_data(G_OBJECT(row), "denchiku-row-triggers")) return;

    track_menu_add_triggers(row, menu_btn);
    g_object_set_data(G_OBJECT(row), "denchiku-row-triggers", GINT_TO_POINTER(1));
}

static void on_row_content_mapped(GtkWidget *box, gpointer user_data) {
    (void)user_data;
    sync_row_triggers(box);
}

/* Кнопки лайка и меню создаём на нашем содержимом ячейки (оно привязано всегда),
 * а триггеры по всей строке добавляем отдельно и один раз — когда внутреннюю
 * строку удаётся найти. Так кнопки не пропадают, даже если строка ещё не в дереве. */
static void attach_row_menu(GtkWidget *box, GtkWidget *suffix, TrackList *tl) {
    if (!suffix) return;

    GtkWidget *menu_btn = g_object_get_data(G_OBJECT(box), "menu-button");
    if (!menu_btn) {
        menu_btn = track_menu_attach_row(box, suffix, tl->cfg.context, tl->player);
        g_object_set_data(G_OBJECT(box), "menu-button", menu_btn);
    }

    /* Первый bind может пройти до вставки строки в дерево — тогда триггеры
     * доставит обработчик первого отображения содержимого строки. */
    if (!g_object_get_data(G_OBJECT(box), "denchiku-map-watch")) {
        g_signal_connect(box, "map", G_CALLBACK(on_row_content_mapped), NULL);
        g_object_set_data(G_OBJECT(box), "denchiku-map-watch", GINT_TO_POINTER(1));
    }
    sync_row_triggers(box);
}

static GtkWidget *make_playing_icon(GtkWidget *box) {
    GtkWidget *icon = gtk_image_new_from_icon_name("media-playback-start-symbolic");
    gtk_widget_add_css_class(icon, "denchiku-playing-icon");
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(icon, FALSE);
    gtk_box_append(GTK_BOX(box), icon);
    return icon;
}

static GtkWidget *make_cover_slot(GtkWidget *box) {
    GtkWidget *slot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_valign(slot, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), slot);
    return slot;
}

static void remember_position(GtkListItem *li, GtkWidget *box) {
    guint pos = gtk_column_view_cell_get_position(GTK_COLUMN_VIEW_CELL(li));
    g_object_set_data(G_OBJECT(box), "pos", GUINT_TO_POINTER(pos + 1));
    GtkWidget *row = row_widget_of(box);
    if (row) {
        g_object_set_data(G_OBJECT(row), "pos", GUINT_TO_POINTER(pos + 1));
        g_object_set_data(G_OBJECT(row), "content-box", box);
    }
}

/* --- безымянный столбец обложек (фиксированная ширина) --- */

static void cover_setup(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory; (void)user_data;
    GtkWidget *slot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_valign(slot, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(slot, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(slot, 6);
    gtk_widget_set_margin_bottom(slot, 6);
    gtk_list_item_set_child(li, slot);
}

static void cover_bind(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory;
    TrackList *tl = user_data;
    Track *t = DENCHIKU_TRACK_ITEM(gtk_list_item_get_item(li))->track;
    set_cover(gtk_list_item_get_child(li), t ? t->cover_path : NULL, track_cover_size(tl));
}

static void cover_unbind(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory; (void)user_data;
    GtkWidget *slot = gtk_list_item_get_child(li);
    if (!slot) return;
    GtkWidget *old = gtk_widget_get_first_child(slot);
    if (old) { ui_detach_cover(old); gtk_box_remove(GTK_BOX(slot), old); }
}

/* Пересобирает фабрику столбца обложек при смене размера: у GtkColumnView замена
 * фабрики перепривязывает видимые ячейки (items-changed для того же элемента
 * повторный bind не вызывает). */
static void cover_column_rebuild(TrackList *tl) {
    if (!tl->cover_col) return;
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(cover_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(cover_bind), tl);
    g_signal_connect(factory, "unbind", G_CALLBACK(cover_unbind), NULL);
    gtk_column_view_column_set_factory(tl->cover_col, factory);
    gtk_column_view_column_set_fixed_width(tl->cover_col, track_cover_size(tl) + 16);
}

/* --- столбец «Название» --- */

static void title_setup(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory;
    (void)user_data;
    ensure_row_css();

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(box, TRUE);

    GtkWidget *icon = make_playing_icon(box);

    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), label);

    g_object_set_data(G_OBJECT(box), "playing-icon", icon);
    g_object_set_data(G_OBJECT(box), "title-label", label);
    gtk_list_item_set_child(li, box);
}

static void title_bind(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory;
    (void)user_data;
    TrackItem *item = gtk_list_item_get_item(li);
    GtkWidget *box = gtk_list_item_get_child(li);
    Track *t = item->track;
    if (!t) return;

    remember_position(li, box);
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(box), "title-label")),
                       t->title ? t->title : "");
    row_set_playing(g_object_get_data(G_OBJECT(box), "title-label"),
                    g_object_get_data(G_OBJECT(box), "playing-icon"), item->playing);
    playing_handler_bind(box, item);
}

static void title_unbind(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory;
    (void)user_data;
    GtkWidget *box = gtk_list_item_get_child(li);
    if (!box) return;
    playing_handler_unbind(box);
    row_set_playing(g_object_get_data(G_OBJECT(box), "title-label"),
                    g_object_get_data(G_OBJECT(box), "playing-icon"), FALSE);
}

/* --- текстовые столбцы --- */

static void label_setup(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory; (void)user_data;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_add_css_class(label, "dim-label");
    gtk_list_item_set_child(li, label);
}

static void artist_bind(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory; (void)user_data;
    Track *t = DENCHIKU_TRACK_ITEM(gtk_list_item_get_item(li))->track;
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)), t && t->artist ? t->artist : "");
}

static void album_bind(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory; (void)user_data;
    Track *t = DENCHIKU_TRACK_ITEM(gtk_list_item_get_item(li))->track;
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)), t && t->album ? t->album : "");
}

static void year_bind(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory; (void)user_data;
    Track *t = DENCHIKU_TRACK_ITEM(gtk_list_item_get_item(li))->track;
    char *text = (t && t->year > 0) ? g_strdup_printf("%u", t->year) : g_strdup("");
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)), text);
    g_free(text);
}

static void duration_setup(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory; (void)user_data;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 1.0f);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(label, "dim-label");
    gtk_list_item_set_child(li, label);
}

static void duration_bind(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory; (void)user_data;
    Track *t = DENCHIKU_TRACK_ITEM(gtk_list_item_get_item(li))->track;
    char *text = format_duration(t ? t->duration : 0);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)), text);
    g_free(text);
}

/* --- столбец кнопок --- */

static void actions_setup(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory;
    TrackList *tl = user_data;
    ensure_row_css();

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(box, GTK_ALIGN_END);

    GtkWidget *suffix = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_valign(suffix, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), suffix);
    g_object_set_data(G_OBJECT(box), "suffix-box", suffix);
    gtk_list_item_set_child(li, box);
    (void)tl;
}

/* Меню/триггеры вешаем на привязке: строку можно найти только когда ячейка уже
 * вставлена в дерево виджетов. */
static void actions_bind(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory;
    TrackList *tl = user_data;
    TrackItem *item = gtk_list_item_get_item(li);
    GtkWidget *box = gtk_list_item_get_child(li);
    attach_row_menu(box, g_object_get_data(G_OBJECT(box), "suffix-box"), tl);
    track_menu_row_set_track(box, item->track);
}

/* --- компактная строка (очередь): обложка, две строки текста, длительность, кнопки --- */

static void compact_setup(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory;
    TrackList *tl = user_data;
    ensure_row_css();

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(box, TRUE);

    GtkWidget *cover = make_cover_slot(box);
    GtkWidget *icon = make_playing_icon(box);

    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(text, TRUE);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(text), title);

    GtkWidget *artist = gtk_label_new(NULL);
    gtk_widget_add_css_class(artist, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(artist), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(artist), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(text), artist);
    gtk_box_append(GTK_BOX(box), text);

    GtkWidget *dur = gtk_label_new(NULL);
    gtk_widget_add_css_class(dur, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(dur), 1.0f);
    gtk_widget_set_valign(dur, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), dur);

    GtkWidget *suffix = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_valign(suffix, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), suffix);

    g_object_set_data(G_OBJECT(box), "cover-slot", cover);
    g_object_set_data(G_OBJECT(box), "playing-icon", icon);
    g_object_set_data(G_OBJECT(box), "title-label", title);
    g_object_set_data(G_OBJECT(box), "artist-label", artist);
    g_object_set_data(G_OBJECT(box), "duration-label", dur);
    g_object_set_data(G_OBJECT(box), "suffix-box", suffix);

    gtk_list_item_set_child(li, box);
    if (tl->cfg.reorderable) attach_reorder(box, tl);
}

static void compact_bind(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory;
    TrackList *tl = user_data;
    TrackItem *item = gtk_list_item_get_item(li);
    GtkWidget *box = gtk_list_item_get_child(li);
    Track *t = item->track;
    if (!t) return;

    attach_row_menu(box, g_object_get_data(G_OBJECT(box), "suffix-box"), tl);
    remember_position(li, box);
    set_cover(g_object_get_data(G_OBJECT(box), "cover-slot"), t->cover_path,
              track_cover_size(tl));
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(box), "title-label")),
                       t->title ? t->title : "");
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(box), "artist-label")),
                       t->artist ? t->artist : "");
    char *dur = format_duration(t->duration);
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(box), "duration-label")), dur);
    g_free(dur);
    row_set_playing(g_object_get_data(G_OBJECT(box), "title-label"),
                    g_object_get_data(G_OBJECT(box), "playing-icon"), item->playing);
    playing_handler_bind(box, item);
    track_menu_row_set_track(box, t);
}

static void compact_unbind(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory;
    (void)user_data;
    GtkWidget *box = gtk_list_item_get_child(li);
    if (!box) return;
    GtkWidget *slot = g_object_get_data(G_OBJECT(box), "cover-slot");
    if (slot) {
        GtkWidget *old = gtk_widget_get_first_child(slot);
        if (old) gtk_box_remove(GTK_BOX(slot), old);
    }
    playing_handler_unbind(box);
    row_set_playing(g_object_get_data(G_OBJECT(box), "title-label"),
                    g_object_get_data(G_OBJECT(box), "playing-icon"), FALSE);
    track_menu_row_set_track(box, NULL);
}

/* ===== активация ===== */

guint track_list_get_visible_count(TrackList *tl) {
    GtkSelectionModel *sel = gtk_column_view_get_model(tl->view);
    if (!sel) return 0;
    return g_list_model_get_n_items(G_LIST_MODEL(sel));
}

/* Играем весь список в видимом (в т.ч. отсортированном) порядке, начиная с
 * выбранного трека. */
void track_list_play_position(TrackList *tl, guint position) {
    GtkSelectionModel *sel = gtk_column_view_get_model(tl->view);
    if (!sel) return;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(sel));
    if (position >= n) return;

    GPtrArray *tracks = g_ptr_array_new();
    for (guint i = 0; i < n; i++) {
        GObject *obj = g_list_model_get_item(G_LIST_MODEL(sel), i);
        if (!obj) continue;
        g_ptr_array_add(tracks, DENCHIKU_TRACK_ITEM(obj)->track);
        g_object_unref(obj);
    }
    player_set_queue_and_play(tl->player, tracks, (int)position);
    g_ptr_array_unref(tracks);
}

static void on_activate(GtkColumnView *view, guint position, gpointer user_data) {
    (void)view;
    TrackList *tl = user_data;
    if (tl->cfg.on_activate) {
        tl->cfg.on_activate(tl, position, tl->cfg.user_data);
        return;
    }
    track_list_play_position(tl, position);
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

static GtkColumnViewColumn *add_text_column(TrackList *tl, const char *title,
                                            GCallback bind, GtkSorter *sorter) {
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(label_setup), NULL);
    g_signal_connect(factory, "bind", bind, NULL);
    GtkColumnViewColumn *col = gtk_column_view_column_new(title, factory);
    gtk_column_view_column_set_expand(col, TRUE);
    gtk_column_view_column_set_resizable(col, TRUE);
    if (sorter) gtk_column_view_column_set_sorter(col, sorter);
    gtk_column_view_append_column(tl->view, col);
    g_object_unref(col);
    return col;
}

/* Клик по шапке столбца меняет сортировку таблицы — отражаем это в панели
 * управления видом, чтобы меню и состояние оставались согласованными. Столбцы,
 * которых нет в меню (альбом, длительность), сортировку в меню не трогают. */
static void on_column_sorter_changed(GtkSorter *sorter, GtkSorterChange change,
                                     gpointer user_data) {
    (void)change;
    TrackList *tl = user_data;
    if (tl->syncing_sort || !tl->cfg.controls) return;

    GtkColumnViewSorter *cvs = GTK_COLUMN_VIEW_SORTER(sorter);
    GtkColumnViewColumn *col = gtk_column_view_sorter_get_primary_sort_column(cvs);
    if (col != tl->title_col && col != tl->artist_col && col != tl->year_col) return;

    GtkSortType order = gtk_column_view_sorter_get_primary_sort_order(cvs);
    ViewSortField field = VIEW_SORT_TITLE;
    if (col == tl->artist_col) field = VIEW_SORT_ARTIST;
    else if (col == tl->year_col) field = VIEW_SORT_YEAR;
    view_controls_set_sort(tl->cfg.controls, field, order == GTK_SORT_DESCENDING);
}

/* Применяем состояние панели управления к сортировке таблицы. Сортировку по
 * столбцам вне меню (альбом/длительность) не перебиваем. */
static void apply_sort(TrackList *tl) {
    if (!tl->cfg.controls || !tl->title_col) return;

    GtkColumnViewSorter *cvs = GTK_COLUMN_VIEW_SORTER(gtk_column_view_get_sorter(tl->view));
    GtkColumnViewColumn *cur = gtk_column_view_sorter_get_primary_sort_column(cvs);
    if (cur && cur != tl->title_col && cur != tl->artist_col && cur != tl->year_col) return;

    ViewSortField field = view_controls_get_sort_field(tl->cfg.controls);
    GtkColumnViewColumn *col = tl->title_col;
    if (field == VIEW_SORT_ARTIST && tl->artist_col) col = tl->artist_col;
    else if (field == VIEW_SORT_YEAR && tl->year_col) col = tl->year_col;
    GtkSortType order = view_controls_get_sort_descending(tl->cfg.controls)
                            ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING;

    if (gtk_column_view_sorter_get_primary_sort_column(cvs) == col &&
        gtk_column_view_sorter_get_primary_sort_order(cvs) == order) {
        return;
    }

    tl->syncing_sort = TRUE;
    gtk_column_view_sort_by_column(tl->view, col, order);
    tl->syncing_sort = FALSE;
}

void track_list_refresh(TrackList *tl) {
    if (!tl || !tl->cfg.controls) return;

    /* Размер элементов изменился — обновляем ширину столбца обложек и
     * перепривязываем видимые строки, чтобы обложки и высота строк пересчитались
     * единообразно. Сортировку при этом сохраняем. */
    ViewZoom zoom = view_controls_get_zoom(tl->cfg.controls);
    if (zoom != tl->last_zoom) {
        tl->last_zoom = zoom;
        gtk_widget_remove_css_class(GTK_WIDGET(tl->view), "denchiku-zoom-small");
        gtk_widget_remove_css_class(GTK_WIDGET(tl->view), "denchiku-zoom-medium");
        gtk_widget_remove_css_class(GTK_WIDGET(tl->view), "denchiku-zoom-large");
        gtk_widget_add_css_class(GTK_WIDGET(tl->view), zoom_class(zoom));
        cover_column_rebuild(tl);
    }

    apply_sort(tl);
}

static void build_compact(TrackList *tl) {
    gtk_widget_add_css_class(GTK_WIDGET(tl->view), "rich-list");
    gtk_widget_add_css_class(GTK_WIDGET(tl->view), "denchiku-compact");
    gtk_widget_add_css_class(GTK_WIDGET(tl->view), "denchiku-tracks");

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(compact_setup), tl);
    g_signal_connect(factory, "bind", G_CALLBACK(compact_bind), tl);
    g_signal_connect(factory, "unbind", G_CALLBACK(compact_unbind), tl);
    GtkColumnViewColumn *col = gtk_column_view_column_new("", factory);
    gtk_column_view_column_set_expand(col, TRUE);
    gtk_column_view_append_column(tl->view, col);
    g_object_unref(col);

    GtkNoSelection *sel = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(tl->store)));
    gtk_column_view_set_model(tl->view, GTK_SELECTION_MODEL(sel));
    g_object_unref(sel);

    hide_column_headers(tl->view);
}

static void build_detailed(TrackList *tl) {
    gtk_widget_add_css_class(GTK_WIDGET(tl->view), "data-table");
    gtk_widget_add_css_class(GTK_WIDGET(tl->view), "denchiku-tracks");
    gtk_widget_add_css_class(GTK_WIDGET(tl->view), zoom_class(tl->last_zoom));

    /* Обложка — отдельный безымянный несортируемый столбец фиксированной ширины
     * (как в списках альбомов/исполнителей): размер обложек одинаков для всех
     * строк и меняется вместе с зумом. */
    tl->cover_col = gtk_column_view_column_new("", NULL);
    gtk_column_view_column_set_resizable(tl->cover_col, FALSE);
    gtk_column_view_append_column(tl->view, tl->cover_col);
    cover_column_rebuild(tl);

    /* Название. */
    GtkListItemFactory *ft = gtk_signal_list_item_factory_new();
    g_signal_connect(ft, "setup", G_CALLBACK(title_setup), tl);
    g_signal_connect(ft, "bind", G_CALLBACK(title_bind), tl);
    g_signal_connect(ft, "unbind", G_CALLBACK(title_unbind), tl);
    GtkColumnViewColumn *title_col = gtk_column_view_column_new("Название", ft);
    gtk_column_view_column_set_expand(title_col, TRUE);
    gtk_column_view_column_set_resizable(title_col, TRUE);
    GtkSorter *s_title = GTK_SORTER(gtk_custom_sorter_new(cmp_title, NULL, NULL));
    gtk_column_view_column_set_sorter(title_col, s_title);
    g_object_unref(s_title);
    gtk_column_view_append_column(tl->view, title_col);
    tl->title_col = title_col;

    GtkSorter *s_artist = GTK_SORTER(gtk_custom_sorter_new(cmp_artist, NULL, NULL));
    tl->artist_col = add_text_column(tl, "Исполнитель", G_CALLBACK(artist_bind), s_artist);
    g_object_unref(s_artist);

    GtkSorter *s_album = GTK_SORTER(gtk_custom_sorter_new(cmp_album, NULL, NULL));
    add_text_column(tl, "Альбом", G_CALLBACK(album_bind), s_album);
    g_object_unref(s_album);

    /* Год. */
    GtkSorter *s_year = GTK_SORTER(gtk_custom_sorter_new(cmp_year, NULL, NULL));
    tl->year_col = add_text_column(tl, "Год", G_CALLBACK(year_bind), s_year);
    g_object_unref(s_year);

    /* Длительность — в шапке значок часов вместо слова. */
    GtkListItemFactory *fd = gtk_signal_list_item_factory_new();
    g_signal_connect(fd, "setup", G_CALLBACK(duration_setup), NULL);
    g_signal_connect(fd, "bind", G_CALLBACK(duration_bind), NULL);
    GtkColumnViewColumn *dur_col = gtk_column_view_column_new("Длительность", fd);
    gtk_column_view_column_set_fixed_width(dur_col, 88);
    gtk_column_view_column_set_resizable(dur_col, TRUE);
    GtkSorter *s_dur = GTK_SORTER(gtk_custom_sorter_new(cmp_duration, NULL, NULL));
    gtk_column_view_column_set_sorter(dur_col, s_dur);
    g_object_unref(s_dur);
    gtk_column_view_append_column(tl->view, dur_col);
    set_column_header_icon(tl->view, dur_col, "preferences-system-time-symbolic");
    g_object_unref(dur_col);

    /* Кнопки лайка и меню. Ширина — по содержимому, чтобы не обрезать кнопки. */
    GtkListItemFactory *fx = gtk_signal_list_item_factory_new();
    g_signal_connect(fx, "setup", G_CALLBACK(actions_setup), tl);
    g_signal_connect(fx, "bind", G_CALLBACK(actions_bind), tl);
    GtkColumnViewColumn *act_col = gtk_column_view_column_new("", fx);
    gtk_column_view_column_set_resizable(act_col, FALSE);
    gtk_column_view_append_column(tl->view, act_col);
    g_object_unref(act_col);

    /* Сортер таблицы — на общую модель; по умолчанию сортируем по названию. */
    GtkSorter *view_sorter = gtk_column_view_get_sorter(tl->view);
    g_signal_connect(view_sorter, "changed", G_CALLBACK(on_column_sorter_changed), tl);
    GtkSortListModel *sort = gtk_sort_list_model_new(
        G_LIST_MODEL(g_object_ref(tl->store)), g_object_ref(view_sorter));
    GtkNoSelection *sel = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(sort)));
    g_object_unref(sort);
    gtk_column_view_set_model(tl->view, GTK_SELECTION_MODEL(sel));
    g_object_unref(sel);

    gtk_column_view_sort_by_column(tl->view, title_col, GTK_SORT_ASCENDING);
    g_object_unref(title_col);
}

TrackList *track_list_new(Player *player, const TrackListConfig *config) {
    TrackList *tl = g_new0(TrackList, 1);
    tl->player = player;
    tl->cfg = *config;
    tl->playing_index = -1;
    tl->last_zoom = tl->cfg.controls ? view_controls_get_zoom(tl->cfg.controls)
                                     : VIEW_ZOOM_MEDIUM;
    tl->store = g_list_store_new(TRACK_ITEM_TYPE);

    tl->view = GTK_COLUMN_VIEW(gtk_column_view_new(NULL));
    gtk_column_view_set_single_click_activate(tl->view, TRUE);
    gtk_column_view_set_reorderable(tl->view, FALSE);
    g_signal_connect(tl->view, "activate", G_CALLBACK(on_activate), tl);

    if (tl->cfg.compact) {
        build_compact(tl);
    } else {
        build_detailed(tl);
    }

    if (tl->cfg.reorderable) {
        GtkDropTarget *target = gtk_drop_target_new(G_TYPE_INT, GDK_ACTION_MOVE);
        g_signal_connect(target, "motion", G_CALLBACK(on_drop_motion), tl);
        g_signal_connect(target, "leave", G_CALLBACK(on_drop_leave), tl);
        g_signal_connect(target, "drop", G_CALLBACK(on_drop), tl);
        gtk_widget_add_controller(GTK_WIDGET(tl->view), GTK_EVENT_CONTROLLER(target));
    }

    /* Список растягивается на всю область содержимого (как в разделе альбомов):
     * обычная прокрутка без ограничения ширины и отступов. */
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), GTK_WIDGET(tl->view));

    tl->widget = scrolled;
    g_object_set_data_full(G_OBJECT(tl->widget), "track-list", tl, track_list_destroy);
    return tl;
}

GtkWidget *track_list_get_widget(TrackList *tl) {
    return tl->widget;
}

/* Индекс uri -> позиция в списке строится лениво: он нужен только для подсветки
 * по uri (медиатека/плейлисты), а очередь подсвечивает по индексу и в нём не
 * нуждается — не тратим тысячи g_strdup на каждую пересборку очереди.
 * Значение — индекс + 1, чтобы 0 отличать от «не найдено». */
static void ensure_uri_index(TrackList *tl) {
    if (tl->uri_index || !tl->tracks) return;
    tl->uri_index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < tl->tracks->len; i++) {
        Track *t = g_ptr_array_index(tl->tracks, i);
        if (t->uri && !g_hash_table_contains(tl->uri_index, t->uri))
            g_hash_table_insert(tl->uri_index, g_strdup(t->uri),
                                GINT_TO_POINTER((gint)i + 1));
    }
}

/* Трек элемента модели по его позиции (заимствованный указатель). */
static Track *store_track_at(TrackList *tl, guint index) {
    GObject *obj = g_list_model_get_item(G_LIST_MODEL(tl->store), index);
    if (!obj) return NULL;
    Track *t = DENCHIKU_TRACK_ITEM(obj)->track;
    g_object_unref(obj);
    return t;
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

    /* Один splice вместо append на каждый элемент: append дёргает перепривязку
     * вьюх и сортировку заново (квадратичная сложность на больших списках). */
    GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
    for (guint i = 0; i < tracks->len; i++) {
        g_ptr_array_add(items, track_item_new(g_ptr_array_index(tracks, i)));
    }
    g_list_store_splice(tl->store, 0, 0, items->pdata, items->len);
    g_ptr_array_unref(items);
}

/* Инкрементальная синхронизация с новым содержимым: переиспользует общий префикс
 * и суффикс строк и пересобирает только изменившийся «серединный» участок.
 * Порядок модели после вызова совпадает с tracks. Так смена состава очереди не
 * пересоздаёт все строки и не сбрасывает прокрутку, в отличие от полного
 * track_list_set_tracks. */
void track_list_sync_tracks(TrackList *tl, GPtrArray *tracks) {
    if (!tl) return;

    guint n = g_list_model_get_n_items(G_LIST_MODEL(tl->store));
    guint m = tracks ? tracks->len : 0;

    guint p = 0;
    while (p < n && p < m && store_track_at(tl, p) == g_ptr_array_index(tracks, p))
        p++;
    guint s = 0;
    while (s < (n - p) && s < (m - p) &&
           store_track_at(tl, n - 1 - s) == g_ptr_array_index(tracks, m - 1 - s))
        s++;

    guint remove_n = n - p - s;
    guint insert_n = m - p - s;

    /* Снимаем подсветку ДО правок: флаг живёт на элементе, поэтому снимется с
     * нужной строки, даже если её позиция затем сместится. Повторно подсветит
     * очередь вызовом track_list_set_playing_index после синхронизации. */
    if (tl->playing_index >= 0) apply_playing_change(tl, tl->playing_index, FALSE);
    tl->playing_index = -1;
    tl->drop_row = NULL;

    if (remove_n > 0 || insert_n > 0) {
        GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
        for (guint i = p; i < p + insert_n; i++)
            g_ptr_array_add(items, track_item_new(g_ptr_array_index(tracks, i)));
        g_list_store_splice(tl->store, p, remove_n, items->pdata, items->len);
        g_ptr_array_unref(items);
    }

    if (tl->tracks) g_ptr_array_unref(tl->tracks);
    tl->tracks = tracks ? g_ptr_array_ref(tracks) : NULL;

    if (tl->uri_index) {
        g_hash_table_destroy(tl->uri_index);
        tl->uri_index = NULL;
    }
}

void track_list_set_playing_uri(TrackList *tl, const char *uri) {
    if (!tl) return;

    ensure_uri_index(tl);
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
