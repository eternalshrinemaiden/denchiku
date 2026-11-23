#include "player_bar.h"
#include "track.h"
#include "ui_helpers.h"
#include <gst/gst.h>

#define COVER_SIZE 44            /* обложка в нижнем баре */
#define FULL_COVER_SIZE 256      /* обложка полноэкранного плеера (декод ×2) */

/* Размытый фон бара на основе обложки трека. */
#define BACKGROUND_SIZE 96
#define BACKGROUND_BLUR 42.0
#define BACKGROUND_SCRIM 0.60        /* затемнение/подсветка в тёмной теме */
#define BACKGROUND_SCRIM_LIGHT 0.80  /* белая подсветка в светлой теме */

typedef enum {
    LAYOUT_WIDE,     /* десктоп: три колонки */
    LAYOUT_COMPACT,  /* узко: только строка трека */
} LayoutMode;

struct _PlayerBar {
    Player *player;
    PlayerBarVariant variant;
    GtkWidget *widget;
    GtkWidget *content;

    GtkButton *play_pause_btn;
    GtkWidget *shuffle_btn;   /* переключатель перемешивания */
    GtkWidget *repeat_btn;    /* переключатель режима повтора */

    GtkWidget *info_box;      /* BAR: обложка + название/исполнитель */
    GtkWidget *center_box;    /* BAR: перемотка + кнопки */
    GtkWidget *volume_box;    /* громкость */

    GtkWidget *cover_box;
    char *cover_path;
    GtkLabel *title_label;
    GtkLabel *artist_label;
    GtkWidget *title_menu_btn; /* BAR: меню трека справа от названия */
    GtkWidget *favorite_btn;   /* BAR: сердечко слева от меню трека */

    GtkScale *seek_scale;
    GtkLabel *pos_label;
    GtkLabel *dur_label;

    GtkScale *volume_scale;

    GtkWidget *queue_btn;     /* BAR: переключатель боковой панели очереди */

    guint timer_id;
    gboolean is_seeking;
    gboolean pointer_down;
    gboolean updating_controls;

    int layout_mode;
};

static void apply_mode(PlayerBar *pb, LayoutMode mode);

/* Корневой контейнер бара. GtkWidget в GTK4 вызывает size_allocate только
 * когда у виджета НЕТ менеджера компоновки, поэтому реагируем на ресайз через
 * GtkCustomLayout.allocate (он вызывается всегда). */
#define KERUU_TYPE_PLAYER_BAR_ROOT (keruu_player_bar_root_get_type())
G_DECLARE_FINAL_TYPE(KeruuPlayerBarRoot, keruu_player_bar_root, KERUU, PLAYER_BAR_ROOT, GtkWidget)

struct _KeruuPlayerBarRoot {
    GtkWidget parent_instance;
    PlayerBar *bar;
    GtkWidget *content;
    GdkTexture *background;
};

G_DEFINE_TYPE(KeruuPlayerBarRoot, keruu_player_bar_root, GTK_TYPE_WIDGET)

enum {
    ROOT_PROP_0,
    ROOT_PROP_MOBILE, /* мобильный (компактный) вид; ставится брейкпоинтом вместе
                       * со сворачиванием сайдбара, чтобы они были синхронны */
    ROOT_PROP_LAST,
};
static GParamSpec *root_props[ROOT_PROP_LAST];

static void keruu_player_bar_root_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
    KeruuPlayerBarRoot *self = KERUU_PLAYER_BAR_ROOT(object);
    switch (prop_id) {
    case ROOT_PROP_MOBILE:
        g_value_set_boolean(value, self->bar && self->bar->layout_mode == LAYOUT_COMPACT);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void keruu_player_bar_root_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
    KeruuPlayerBarRoot *self = KERUU_PLAYER_BAR_ROOT(object);
    switch (prop_id) {
    case ROOT_PROP_MOBILE:
        if (self->bar && self->bar->variant == PLAYER_BAR_VARIANT_BAR) {
            LayoutMode mode = g_value_get_boolean(value) ? LAYOUT_COMPACT : LAYOUT_WIDE;
            if ((int)mode != self->bar->layout_mode) {
                self->bar->layout_mode = mode;
                apply_mode(self->bar, mode);
                g_object_notify_by_pspec(object, root_props[ROOT_PROP_MOBILE]);
            }
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void keruu_player_bar_root_measure(GtkWidget *widget, GtkOrientation orientation, int for_size,
                                          int *minimum, int *natural, int *min_baseline, int *nat_baseline) {
    KeruuPlayerBarRoot *self = KERUU_PLAYER_BAR_ROOT(widget);
    /* Бар/шторка не должны задавать минимальную ширину окна: на узкой ширине
     * включается компактный режим, а до этого контент может ужаться. */
    ui_measure_min_width(self->content, orientation, for_size,
                         minimum, natural, min_baseline, nat_baseline);
}

static void keruu_player_bar_root_allocate(GtkWidget *widget, int width, int height, int baseline) {
    KeruuPlayerBarRoot *self = KERUU_PLAYER_BAR_ROOT(widget);
    if (!self->content) return;

    gboolean is_bar = self->bar && self->bar->variant == PLAYER_BAR_VARIANT_BAR;

    if (is_bar) {
        int inset = (self->bar->layout_mode == LAYOUT_WIDE) ? 16 : 8;
        int w = MAX(width - 2 * inset, 0);
        int h = MAX(height - 6 - 8, 0);

        /* gtk_widget_allocate() забирает владение transform (transfer full),
         * поэтому ref освобождать не нужно. */
        graphene_point_t offset = { inset, 6 };
        GskTransform *transform = gsk_transform_translate(NULL, &offset);
        gtk_widget_allocate(self->content, w, h, baseline, transform);
    } else {
        gtk_widget_allocate(self->content, width, height, baseline, NULL);
    }
}

/* Рисуем размытую обложку ПОД дочерними виджетами: сначала фон, затем
 * штатная отрисовка GtkWidget (CSS-фон + дети). */
static void keruu_player_bar_root_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    KeruuPlayerBarRoot *self = KERUU_PLAYER_BAR_ROOT(widget);

    if (self->background) {
        int width = gtk_widget_get_width(widget);
        int height = gtk_widget_get_height(widget);

        if (width > 0 && height > 0) {
            int tw = gdk_texture_get_width(self->background);
            int th = gdk_texture_get_height(self->background);

            graphene_rect_t bounds;
            double tex_ratio = (double)tw / (double)th;
            double area_ratio = (double)width / (double)height;
            if (tex_ratio > area_ratio) {
                double bw = height * tex_ratio;
                bounds = GRAPHENE_RECT_INIT((width - bw) / 2.0, 0, bw, height);
            } else {
                double bh = width / tex_ratio;
                bounds = GRAPHENE_RECT_INIT(0, (height - bh) / 2.0, width, bh);
            }

            gtk_snapshot_push_blur(snapshot, BACKGROUND_BLUR);
            gtk_snapshot_append_texture(snapshot, self->background, &bounds);
            gtk_snapshot_pop(snapshot);

            /* Светлая тема использует тёмный текст, поэтому затемнять фон
             * чёрным нельзя — поверх размытой обложки кладём белую подсветку,
             * чтобы передний план оставался читаемым. */
            gboolean dark = adw_style_manager_get_dark(adw_style_manager_get_default());
            graphene_rect_t full = GRAPHENE_RECT_INIT(0, 0, width, height);
            GdkRGBA scrim = dark ? (GdkRGBA){ 0.0, 0.0, 0.0, BACKGROUND_SCRIM }
                                 : (GdkRGBA){ 1.0, 1.0, 1.0, BACKGROUND_SCRIM_LIGHT };
            gtk_snapshot_append_color(snapshot, &scrim, &full);
        }
    }

    GTK_WIDGET_CLASS(keruu_player_bar_root_parent_class)->snapshot(widget, snapshot);
}

static void keruu_player_bar_root_dispose(GObject *object) {
    KeruuPlayerBarRoot *self = KERUU_PLAYER_BAR_ROOT(object);
    g_clear_pointer(&self->content, gtk_widget_unparent);
    g_clear_object(&self->background);
    G_OBJECT_CLASS(keruu_player_bar_root_parent_class)->dispose(object);
}

static void keruu_player_bar_root_class_init(KeruuPlayerBarRootClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = keruu_player_bar_root_dispose;
    object_class->get_property = keruu_player_bar_root_get_property;
    object_class->set_property = keruu_player_bar_root_set_property;

    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->snapshot = keruu_player_bar_root_snapshot;

    root_props[ROOT_PROP_MOBILE] =
        g_param_spec_boolean("mobile", NULL, NULL, FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
    g_object_class_install_properties(object_class, ROOT_PROP_LAST, root_props);
}

static void keruu_player_bar_root_init(KeruuPlayerBarRoot *self) {
    self->bar = NULL;
    self->content = NULL;
    self->background = NULL;

    GtkLayoutManager *layout = gtk_custom_layout_new(NULL,
        keruu_player_bar_root_measure, keruu_player_bar_root_allocate);
    gtk_widget_set_layout_manager(GTK_WIDGET(self), layout);
}

static char *format_time_ns(gint64 ns) {
    if (ns <= 0) return g_strdup("0:00");
    gint64 total_sec = ns / GST_SECOND;
    gint64 mins = total_sec / 60;
    gint64 secs = total_sec % 60;
    return g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, mins, secs);
}

static void on_prev_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    player_prev(((PlayerBar *)user_data)->player);
}

static void on_play_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    player_toggle_pause(((PlayerBar *)user_data)->player);
}

static void on_next_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    player_next(((PlayerBar *)user_data)->player);
}

static void on_shuffle_toggled(GtkToggleButton *btn, gpointer user_data) {
    PlayerBar *pb = user_data;
    if (pb->updating_controls) return;
    player_set_shuffle(pb->player, gtk_toggle_button_get_active(btn));
}

static void on_repeat_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    PlayerBar *pb = user_data;
    PlayerRepeatMode mode = player_get_repeat(pb->player);
    mode = (PlayerRepeatMode)(((int)mode + 1) % 3);
    player_set_repeat(pb->player, mode);
}

static void on_volume_changed(GtkRange *range, gpointer user_data) {
    PlayerBar *pb = user_data;
    player_set_volume(pb->player, gtk_range_get_value(range));
}

static void commit_seek(PlayerBar *pb) {
    double val_sec = gtk_range_get_value(GTK_RANGE(pb->seek_scale));
    gint64 target_ns = (gint64)(val_sec * (double)GST_SECOND);
    player_seek(pb->player, target_ns);
}

/* Низкоуровневый наблюдатель за физическим нажатием и отпусканием кнопки мыши / пальца */
static gboolean on_scale_legacy_event(GtkEventControllerLegacy *ctrl, GdkEvent *event, gpointer user_data) {
    (void)ctrl;
    PlayerBar *pb = user_data;
    GdkEventType type = gdk_event_get_event_type(event);

    if (type == GDK_BUTTON_PRESS || type == GDK_TOUCH_BEGIN) {
        pb->pointer_down = TRUE;
        pb->is_seeking = TRUE;
    } else if (type == GDK_BUTTON_RELEASE || type == GDK_TOUCH_END || type == GDK_TOUCH_CANCEL) {
        if (pb->is_seeking) {
            commit_seek(pb);
            pb->is_seeking = FALSE;
        }
        pb->pointer_down = FALSE;
    }

    return FALSE;
}

static gboolean on_range_change_value(GtkRange *range, GtkScrollType scroll, double value, gpointer user_data) {
    (void)scroll;
    PlayerBar *pb = user_data;

    GtkAdjustment *adj = gtk_range_get_adjustment(range);
    double clamped = CLAMP(value, gtk_adjustment_get_lower(adj), gtk_adjustment_get_upper(adj));
    gint64 target_ns = (gint64)(clamped * (double)GST_SECOND);

    char *pos_str = format_time_ns(target_ns);
    gtk_label_set_text(pb->pos_label, pos_str);
    g_free(pos_str);

    if (!pb->pointer_down) {
        player_seek(pb->player, target_ns);
    }

    return FALSE;
}

/* Фоновый таймер обновления прогресса (4 раза в секунду) */
static gboolean on_progress_tick(gpointer user_data) {
    PlayerBar *pb = user_data;

    if (pb->is_seeking || !player_get_current_track(pb->player)) {
        return G_SOURCE_CONTINUE;
    }

    gint64 pos = player_get_position(pb->player);
    gint64 dur = player_get_duration(pb->player);

    if (dur > 0) {
        double dur_sec = (double)dur / (double)GST_SECOND;
        double pos_sec = (double)pos / (double)GST_SECOND;

        gtk_range_set_range(GTK_RANGE(pb->seek_scale), 0.0, dur_sec);
        gtk_range_set_value(GTK_RANGE(pb->seek_scale), pos_sec);

        char *pos_str = format_time_ns(pos);
        char *dur_str = format_time_ns(dur);
        gtk_label_set_text(pb->pos_label, pos_str);
        gtk_label_set_text(pb->dur_label, dur_str);
        g_free(pos_str);
        g_free(dur_str);
    }

    return G_SOURCE_CONTINUE;
}

/* Ряд перемотки: время слева/справа и ползунок. */
static GtkWidget *build_seek_row(PlayerBar *pb) {
    GtkWidget *seek_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(seek_box, GTK_ALIGN_CENTER);

    pb->pos_label = GTK_LABEL(gtk_label_new("0:00"));
    gtk_widget_add_css_class(GTK_WIDGET(pb->pos_label), "numeric");
    gtk_widget_add_css_class(GTK_WIDGET(pb->pos_label), "caption");

    pb->seek_scale = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 100.0, 1.0));
    gtk_scale_set_draw_value(pb->seek_scale, FALSE);
    gtk_widget_set_hexpand(GTK_WIDGET(pb->seek_scale), TRUE);
    gtk_widget_set_valign(GTK_WIDGET(pb->seek_scale), GTK_ALIGN_CENTER);
    gtk_widget_set_sensitive(GTK_WIDGET(pb->seek_scale), FALSE);
    g_signal_connect(pb->seek_scale, "change-value", G_CALLBACK(on_range_change_value), pb);

    GtkEventController *legacy = gtk_event_controller_legacy_new();
    gtk_event_controller_set_propagation_phase(legacy, GTK_PHASE_CAPTURE);
    g_signal_connect(legacy, "event", G_CALLBACK(on_scale_legacy_event), pb);
    gtk_widget_add_controller(GTK_WIDGET(pb->seek_scale), legacy);

    pb->dur_label = GTK_LABEL(gtk_label_new("0:00"));
    gtk_widget_add_css_class(GTK_WIDGET(pb->dur_label), "numeric");
    gtk_widget_add_css_class(GTK_WIDGET(pb->dur_label), "caption");

    gtk_box_append(GTK_BOX(seek_box), GTK_WIDGET(pb->pos_label));
    gtk_box_append(GTK_BOX(seek_box), GTK_WIDGET(pb->seek_scale));
    gtk_box_append(GTK_BOX(seek_box), GTK_WIDGET(pb->dur_label));
    return seek_box;
}

/* Ряд кнопок управления. */
static GtkWidget *build_controls_row(PlayerBar *pb, int spacing, int play_width) {
    GtkWidget *controls_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing);
    gtk_widget_set_halign(controls_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(controls_box, GTK_ALIGN_CENTER);

    GtkWidget *prev_btn = gtk_button_new_from_icon_name("media-skip-backward-symbolic");
    GtkWidget *play_btn = gtk_button_new_from_icon_name("media-playback-start-symbolic");
    GtkWidget *next_btn = gtk_button_new_from_icon_name("media-skip-forward-symbolic");

    /* Перемешивание — слева от «предыдущий», повтор — справа от «следующий». */
    GtkWidget *shuffle_btn = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(shuffle_btn), "media-playlist-shuffle-symbolic");
    gtk_widget_set_tooltip_text(shuffle_btn, "Перемешать");
    gtk_widget_add_css_class(shuffle_btn, "flat");
    gtk_widget_set_valign(shuffle_btn, GTK_ALIGN_CENTER);

    GtkWidget *repeat_btn = gtk_button_new_from_icon_name("media-playlist-repeat-symbolic");
    gtk_widget_set_tooltip_text(repeat_btn, "Повтор выключен");
    gtk_widget_add_css_class(repeat_btn, "flat");
    gtk_widget_set_valign(repeat_btn, GTK_ALIGN_CENTER);

    pb->play_pause_btn = GTK_BUTTON(play_btn);
    pb->shuffle_btn = shuffle_btn;
    pb->repeat_btn = repeat_btn;

    /* Кнопка play/pause вытянута по горизонтали, шире боковых. */
    gtk_widget_set_size_request(play_btn, play_width, -1);

    gtk_widget_add_css_class(prev_btn, "flat");
    gtk_widget_add_css_class(play_btn, "circular");
    gtk_widget_add_css_class(next_btn, "flat");

    g_signal_connect(shuffle_btn, "toggled", G_CALLBACK(on_shuffle_toggled), pb);
    g_signal_connect(prev_btn, "clicked", G_CALLBACK(on_prev_clicked), pb);
    g_signal_connect(play_btn, "clicked", G_CALLBACK(on_play_clicked), pb);
    g_signal_connect(next_btn, "clicked", G_CALLBACK(on_next_clicked), pb);
    g_signal_connect(repeat_btn, "clicked", G_CALLBACK(on_repeat_clicked), pb);

    gtk_box_append(GTK_BOX(controls_box), shuffle_btn);
    gtk_box_append(GTK_BOX(controls_box), prev_btn);
    gtk_box_append(GTK_BOX(controls_box), play_btn);
    gtk_box_append(GTK_BOX(controls_box), next_btn);
    gtk_box_append(GTK_BOX(controls_box), repeat_btn);
    return controls_box;
}

/* Ряд громкости. */
static GtkWidget *build_volume_row(PlayerBar *pb, int scale_width) {
    pb->volume_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(pb->volume_box, GTK_ALIGN_CENTER);

    GtkWidget *icon = gtk_image_new_from_icon_name("audio-volume-high-symbolic");
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);

    pb->volume_scale = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.01));
    gtk_scale_set_draw_value(pb->volume_scale, FALSE);
    gtk_widget_set_size_request(GTK_WIDGET(pb->volume_scale), scale_width, -1);
    gtk_widget_set_valign(GTK_WIDGET(pb->volume_scale), GTK_ALIGN_CENTER);
    gtk_range_set_value(GTK_RANGE(pb->volume_scale), player_get_volume(pb->player));
    g_signal_connect(pb->volume_scale, "value-changed", G_CALLBACK(on_volume_changed), pb);

    gtk_box_append(GTK_BOX(pb->volume_box), icon);
    gtk_box_append(GTK_BOX(pb->volume_box), GTK_WIDGET(pb->volume_scale));
    return pb->volume_box;
}

/* ===================== нижний бар (3 колонки / компакт) ===================== */

static void place(GtkWidget *grid, GtkWidget *child, int col, int row, int col_span, gboolean visible) {
    gtk_grid_attach(GTK_GRID(grid), child, col, row, col_span, 1);
    gtk_widget_set_visible(child, visible);
}

static void apply_mode(PlayerBar *pb, LayoutMode mode) {
    GtkWidget *children[3] = { pb->info_box, pb->center_box, pb->volume_box };
    /* gtk_grid_remove() снимает единственную ссылку и уничтожил бы плавающие
     * виджеты — держим свою ссылку на время переноса, иначе повторный attach
     * работал бы с висячими указателями. */
    for (int i = 0; i < 3; i++) {
        if (gtk_widget_get_parent(children[i]) == pb->content) {
            g_object_ref(children[i]);
            gtk_grid_remove(GTK_GRID(pb->content), children[i]);
        } else {
            children[i] = NULL;
        }
    }

    if (mode == LAYOUT_WIDE) {
        place(pb->content, pb->info_box, 0, 0, 1, TRUE);
        place(pb->content, pb->center_box, 1, 0, 1, TRUE);
        place(pb->content, pb->volume_box, 2, 0, 1, TRUE);
    } else {
        place(pb->content, pb->info_box, 0, 0, 3, TRUE);
        place(pb->content, pb->center_box, 0, 1, 3, FALSE);
        place(pb->content, pb->volume_box, 0, 2, 3, FALSE);
    }

    for (int i = 0; i < 3; i++) {
        if (children[i]) {
            g_object_unref(children[i]);
        }
    }
}

static void create_playerbar_popup(GtkMenuButton *button, gpointer user_data) {
    PlayerBar *pb = user_data;
    Track *cur = player_get_current_track(pb->player);
    if (!cur) return;

    GtkWidget *popover = track_menu_popover_new(cur, TRACK_CONTEXT_PLAYER_BAR, pb->player);
    gtk_menu_button_set_popover(button, popover);
    track_menu_detach_on_close(popover, GTK_WIDGET(button));
}

static GtkWidget *build_bar_content(PlayerBar *pb) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_widget_set_hexpand(grid, TRUE);

    /* Обложка + название/исполнитель */
    pb->info_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(pb->info_box, GTK_ALIGN_FILL);
    gtk_widget_set_valign(pb->info_box, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(pb->info_box, TRUE);

    pb->cover_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_valign(pb->cover_box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(pb->info_box), pb->cover_box);

    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(text, TRUE);

    GtkWidget *    title_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_hexpand(title_row, TRUE);

    pb->title_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_set_hexpand(GTK_WIDGET(pb->title_label), TRUE);
    gtk_label_set_xalign(pb->title_label, 0.0);
    gtk_label_set_ellipsize(pb->title_label, PANGO_ELLIPSIZE_END);

    /* Кнопка меню трека сразу справа от названия. Меню строится лениво по
     * текущему треку и пересобирается при каждом открытии. */
    pb->title_menu_btn = track_menu_button_new();
    gtk_widget_set_visible(pb->title_menu_btn, FALSE);
    gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(pb->title_menu_btn),
                                          create_playerbar_popup, pb, NULL);

    /* Сердечко слева от кнопки меню. */
    pb->favorite_btn = track_favorite_button_new(NULL);
    gtk_widget_set_visible(pb->favorite_btn, FALSE);

    gtk_box_append(GTK_BOX(title_row), GTK_WIDGET(pb->title_label));
    gtk_box_append(GTK_BOX(title_row), pb->favorite_btn);
    gtk_box_append(GTK_BOX(title_row), pb->title_menu_btn);

    pb->artist_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(pb->artist_label), "dim-label");
    gtk_widget_add_css_class(GTK_WIDGET(pb->artist_label), "caption");
    gtk_widget_set_hexpand(GTK_WIDGET(pb->artist_label), TRUE);
    gtk_label_set_xalign(pb->artist_label, 0.0);
    gtk_label_set_ellipsize(pb->artist_label, PANGO_ELLIPSIZE_END);

    gtk_box_append(GTK_BOX(text), title_row);
    gtk_box_append(GTK_BOX(text), GTK_WIDGET(pb->artist_label));
    gtk_box_append(GTK_BOX(pb->info_box), text);

    /* Правый клик и долгое нажатие по данным трека открывают то же меню. */
    track_menu_add_triggers(pb->info_box, pb->title_menu_btn);

    /* Перемотка + кнопки */
    pb->center_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_halign(pb->center_box, GTK_ALIGN_FILL);
    gtk_widget_set_valign(pb->center_box, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(pb->center_box, TRUE);
    gtk_box_append(GTK_BOX(pb->center_box), build_seek_row(pb));
    gtk_box_append(GTK_BOX(pb->center_box), build_controls_row(pb, 12, 64));

    /* Громкость + кнопка вызова очереди (правее ползунка). */
    build_volume_row(pb, 120);
    pb->queue_btn = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(pb->queue_btn), "view-list-symbolic");
    gtk_widget_set_tooltip_text(pb->queue_btn, "Очередь");
    gtk_widget_add_css_class(pb->queue_btn, "flat");
    gtk_widget_set_valign(pb->queue_btn, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(pb->volume_box), pb->queue_btn);
    gtk_widget_set_halign(pb->volume_box, GTK_ALIGN_END);

    return grid;
}

/* ===================== полноэкранный плеер ===================== */

static GtkWidget *build_fullscreen_content(PlayerBar *pb) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 34);   /* под ручку-«драгхэндл» */
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);

    /* Большая обложка занимает всё доступное место. */
    pb->cover_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(pb->cover_box, TRUE);
    gtk_widget_set_vexpand(pb->cover_box, TRUE);
    gtk_box_append(GTK_BOX(box), pb->cover_box);

    pb->title_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(pb->title_label), "title-3");
    gtk_label_set_xalign(pb->title_label, 0.5);
    gtk_label_set_justify(pb->title_label, GTK_JUSTIFY_CENTER);
    gtk_label_set_ellipsize(pb->title_label, PANGO_ELLIPSIZE_END);
    gtk_label_set_lines(pb->title_label, 2);
    /* Небольшой отступ между обложкой и названием трека. */
    gtk_widget_set_margin_top(GTK_WIDGET(pb->title_label), 12);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(pb->title_label));

    pb->artist_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(pb->artist_label), "dim-label");
    gtk_label_set_xalign(pb->artist_label, 0.5);
    gtk_label_set_ellipsize(pb->artist_label, PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(pb->artist_label));

    gtk_box_append(GTK_BOX(box), build_seek_row(pb));
    gtk_box_append(GTK_BOX(box), build_controls_row(pb, 24, 88));

    build_volume_row(pb, 220);
    gtk_widget_set_halign(pb->volume_box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), pb->volume_box);

    /* Прокрутка с propagate-natural-height: минимальная высота маленькая (шторка
     * ужимается на низком окне), а натуральная — по содержимому (раскрытие почти
     * на весь экран на высоком). */
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    return scroll;
}

/* ===================== общее ===================== */

static void set_cover(PlayerBar *pb, const char *path) {
    if (g_strcmp0(pb->cover_path, path) == 0) return;

    g_free(pb->cover_path);
    pb->cover_path = g_strdup(path);

    GtkWidget *old = gtk_widget_get_first_child(pb->cover_box);
    if (old) {
        gtk_box_remove(GTK_BOX(pb->cover_box), old);
    }

    GtkWidget *cover = (pb->variant == PLAYER_BAR_VARIANT_FULLSCREEN)
        ? ui_create_cover_picture(path, FULL_COVER_SIZE)
        : ui_create_cover_widget(path, COVER_SIZE);
    gtk_box_append(GTK_BOX(pb->cover_box), cover);

    /* Обновляем размытый фон на основе новой обложки. */
    KeruuPlayerBarRoot *root = KERUU_PLAYER_BAR_ROOT(pb->widget);
    g_clear_object(&root->background);
    if (path && *path) {
        root->background = ui_create_cover_texture(path, BACKGROUND_SIZE);
    }
    gtk_widget_queue_draw(pb->widget);
}

PlayerBar *player_bar_new(Player *player, PlayerBarVariant variant) {
    PlayerBar *pb = g_new0(PlayerBar, 1);
    pb->player = player;
    pb->variant = variant;

    KeruuPlayerBarRoot *root = g_object_new(KERUU_TYPE_PLAYER_BAR_ROOT, NULL);
    root->bar = pb;
    gtk_widget_set_overflow(GTK_WIDGET(root), GTK_OVERFLOW_HIDDEN);

    GtkWidget *content = (variant == PLAYER_BAR_VARIANT_FULLSCREEN)
        ? build_fullscreen_content(pb)
        : build_bar_content(pb);

    root->content = content;
    gtk_widget_set_parent(content, GTK_WIDGET(root));

    pb->content = content;
    pb->widget = GTK_WIDGET(root);

    if (variant == PLAYER_BAR_VARIANT_BAR) {
        apply_mode(pb, LAYOUT_WIDE);
        pb->layout_mode = LAYOUT_WIDE;
        /* Бар показывается только когда есть что играть. */
        gtk_widget_set_visible(pb->widget, FALSE);
    }

    pb->timer_id = g_timeout_add(250, on_progress_tick, pb);
    return pb;
}

GtkWidget *player_bar_get_widget(PlayerBar *pb) {
    return pb->widget;
}

GtkWidget *player_bar_get_queue_button(PlayerBar *pb) {
    return pb->queue_btn;
}



/* Перестраивает меню трека под текущий трек (меню зависит от него). */
static void update_track_menu(PlayerBar *pb) {
    if (!pb->title_menu_btn) return;

    Track *cur = player_get_current_track(pb->player);
    gtk_widget_set_visible(pb->title_menu_btn, cur != NULL);

    if (pb->favorite_btn) {
        track_favorite_button_set_uri(pb->favorite_btn, cur ? cur->uri : NULL);
        gtk_widget_set_visible(pb->favorite_btn, cur != NULL);
    }
}

void player_bar_refresh(PlayerBar *pb) {
    gtk_button_set_icon_name(pb->play_pause_btn,
        player_is_playing(pb->player) ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");

    if (pb->shuffle_btn) {
        pb->updating_controls = TRUE;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(pb->shuffle_btn),
                                     player_get_shuffle(pb->player));
        pb->updating_controls = FALSE;
    }

    if (pb->repeat_btn) {
        PlayerRepeatMode mode = player_get_repeat(pb->player);
        gtk_button_set_icon_name(GTK_BUTTON(pb->repeat_btn),
            mode == PLAYER_REPEAT_ONE ? "media-playlist-repeat-song-symbolic"
                                      : "media-playlist-repeat-symbolic");
        if (mode == PLAYER_REPEAT_OFF) {
            gtk_widget_remove_css_class(pb->repeat_btn, "accent");
            gtk_widget_set_tooltip_text(pb->repeat_btn, "Повтор выключен");
        } else if (mode == PLAYER_REPEAT_ALL) {
            gtk_widget_add_css_class(pb->repeat_btn, "accent");
            gtk_widget_set_tooltip_text(pb->repeat_btn, "Повтор очереди");
        } else {
            gtk_widget_add_css_class(pb->repeat_btn, "accent");
            gtk_widget_set_tooltip_text(pb->repeat_btn, "Повтор трека");
        }
    }

    update_track_menu(pb);

    Track *cur = player_get_current_track(pb->player);
    if (cur) {
        char *markup = g_markup_printf_escaped("<b>%s</b>", cur->title);
        gtk_label_set_markup(pb->title_label, markup);
        g_free(markup);
        gtk_widget_set_tooltip_text(GTK_WIDGET(pb->title_label), cur->title);
        gtk_label_set_text(pb->artist_label, cur->artist);
        set_cover(pb, cur->cover_path);
        gtk_widget_set_sensitive(GTK_WIDGET(pb->seek_scale), TRUE);
        if (pb->variant == PLAYER_BAR_VARIANT_BAR) {
            gtk_widget_set_visible(pb->widget, TRUE);
        }
    } else {
        gtk_label_set_text(pb->title_label, "");
        gtk_widget_set_tooltip_text(GTK_WIDGET(pb->title_label), NULL);
        gtk_label_set_text(pb->artist_label, "");
        set_cover(pb, NULL);
        gtk_widget_set_sensitive(GTK_WIDGET(pb->seek_scale), FALSE);
        gtk_label_set_text(pb->pos_label, "0:00");
        gtk_label_set_text(pb->dur_label, "0:00");
        if (pb->variant == PLAYER_BAR_VARIANT_BAR) {
            gtk_widget_set_visible(pb->widget, FALSE);
        }
    }
}

void player_bar_free(PlayerBar *pb) {
    if (!pb) return;
    if (pb->timer_id > 0) {
        g_source_remove(pb->timer_id);
    }
    g_free(pb->cover_path);
    g_free(pb);
}
