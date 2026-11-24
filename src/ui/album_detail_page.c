#include "album_detail_page.h"
#include "track.h"
#include "ui_helpers.h"

/* ===== навигация из страницы альбома =====
 * Клик по имени исполнителя и по карточкам «другие альбомы» открывает новую
 * страницу. Обработчики регистрирует окно: страницы исполнителей и альбомов
 * живут в разделах, а сама страница альбома раздел не знает. */
static AlbumOpenAlbumFn open_album_cb;
static gpointer open_album_data;

static AlbumOpenArtistFn open_artist_cb;
static gpointer open_artist_data;

static AlbumOpenGenreFn open_genre_cb;
static gpointer open_genre_data;

static AlbumEditTagsFn edit_tags_cb;
static gpointer edit_tags_data;

void album_detail_page_set_open_album_cb(AlbumOpenAlbumFn cb, gpointer user_data) {
    open_album_cb = cb;
    open_album_data = user_data;
}

void album_detail_page_set_open_artist_cb(AlbumOpenArtistFn cb, gpointer user_data) {
    open_artist_cb = cb;
    open_artist_data = user_data;
}

void album_detail_page_set_open_genre_cb(AlbumOpenGenreFn cb, gpointer user_data) {
    open_genre_cb = cb;
    open_genre_data = user_data;
}

void album_detail_page_set_edit_tags_cb(AlbumEditTagsFn cb, gpointer user_data) {
    edit_tags_cb = cb;
    edit_tags_data = user_data;
}

/* Префикс внутренней ссылки на исполнителя: клик по такому href не открывает
 * внешний URI, а переходит на страницу исполнителя. */
#define ARTIST_HREF_PREFIX "denchiku-artist:"

static gboolean on_artist_link_activated(GtkLabel *label, const char *uri,
                                         gpointer user_data) {
    (void)label;
    (void)user_data;
    if (open_artist_cb && g_str_has_prefix(uri, ARTIST_HREF_PREFIX)) {
        const char *artist = uri + strlen(ARTIST_HREF_PREFIX);
        if (*artist) open_artist_cb(artist, open_artist_data);
    }
    /* TRUE — не открывать внешнюю ссылку. */
    return TRUE;
}

/* Разметка строки исполнителей шапки: каждый исполнитель (с учётом разбиения из
 * настроек) — отдельная ссылка. Возвращает NULL, если исполнителей нет. */
static char *build_artist_markup(const char *album_artist) {
    char **names = track_split_artists(album_artist);
    if (!names) return NULL;

    GString *s = g_string_new(NULL);
    for (guint i = 0; names[i] != NULL; i++) {
        if (i > 0) g_string_append(s, ", ");
        char *link = g_markup_printf_escaped(
            "<a href=\"" ARTIST_HREF_PREFIX "%s\">%s</a>", names[i], names[i]);
        g_string_append(s, link);
        g_free(link);
    }
    g_strfreev(names);

    if (s->len == 0) {
        g_string_free(s, TRUE);
        return NULL;
    }
    return g_string_free(s, FALSE);
}

/* TRUE, если тег album_artist (с учётом разбиения) содержит исполнителя.
 * Используется для привязки альбомов к исполнителю в секциях «другие альбомы»,
 * чтобы совместные альбомы («A & B») попадали и к «A», и к «B». */
static gboolean tag_has_artist(const char *tag, const char *artist) {
    char **names = track_split_artists(tag);
    if (!names) return FALSE;

    gboolean found = FALSE;
    for (guint i = 0; names[i] != NULL; i++) {
        if (g_strcmp0(names[i], artist) == 0) {
            found = TRUE;
            break;
        }
    }
    g_strfreev(names);
    return found;
}

/* ===== Цветной фон страницы: градиент от доминирующего цвета обложки к
 * прозрачному сверху вниз ===== */

#define TYPE_ALBUM_BACKDROP (album_backdrop_get_type())
G_DECLARE_FINAL_TYPE(AlbumBackdrop, album_backdrop, MC, ALBUM_BACKDROP, GtkWidget)

struct _AlbumBackdrop {
    GtkWidget parent_instance;
    GdkRGBA color;
    gboolean has_color;
    /* Смещение прокрутки содержимого: сдвигает градиент вверх ровно на столько,
     * на сколько прокручен список. Так цветной фон «приклеен» к содержимому и не
     * залипает на месте, пока страница прокручивается. */
    double scroll_offset;
    /* Слабая ссылка на AdwToolbarView: по ней берём высоту шапки, чтобы залить
     * её постоянным цветом и не дать градиенту просвечивать в заголовке. */
    GtkWidget *toolbar_view;
};

G_DEFINE_FINAL_TYPE(AlbumBackdrop, album_backdrop, GTK_TYPE_WIDGET)

/* Фон страницы должен быть заметным, но не «грязным». В тёмной теме тёмный
 * насыщенный цвет поверх тёмного читается как глубокий оттенок обложки. В
 * светлой теме тот же цвет поверх белого даёт мыльный серый, поэтому осветляем
 * его до пастельного тона с сохранением оттенка (HUE) и слегка приглушаем
 * насыщенность, чтобы насыщенные обложки не давали ядовитый фон. */
#define BACKDROP_TOP_ALPHA       0.55f
#define BACKDROP_TOP_ALPHA_LIGHT 0.50f
#define BACKDROP_LIGHT_VALUE     0.88f
#define BACKDROP_LIGHT_MIN_SAT   0.30f
#define BACKDROP_LIGHT_MAX_SAT   0.45f

static void album_backdrop_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    AlbumBackdrop *self = MC_ALBUM_BACKDROP(widget);
    if (!self->has_color) return;

    int width = gtk_widget_get_width(widget);
    int height = gtk_widget_get_height(widget);
    if (width <= 0 || height <= 0) return;

    /* Тему читаем при каждой отрисовке: при смене темы GTK перерисовывает фон, и
     * цвет подстраивается без пересоздания страницы. */
    gboolean dark = adw_style_manager_get_dark(adw_style_manager_get_default());

    GdkRGBA top = self->color;
    if (dark) {
        top.alpha = BACKDROP_TOP_ALPHA;
    } else {
        float h = 0, s = 0, v = 0;
        gtk_rgb_to_hsv(top.red, top.green, top.blue, &h, &s, &v);
        /* Пастель: светлое значение и умеренная насыщенность. Цветной обложке
         * даём минимум насыщенности, чтобы оттенок читался, но ограничиваем
         * сверху; серую обложку не «расцвечиваем» случайным HUE. */
        if (s > 0.10f && s < BACKDROP_LIGHT_MIN_SAT) s = BACKDROP_LIGHT_MIN_SAT;
        s = MIN(s, BACKDROP_LIGHT_MAX_SAT);
        v = BACKDROP_LIGHT_VALUE;
        gtk_hsv_to_rgb(h, s, v, &top.red, &top.green, &top.blue);
        top.alpha = BACKDROP_TOP_ALPHA_LIGHT;
    }

    GdkRGBA bottom = top;
    bottom.alpha = 0.0f;

    /* Высота шапки: её заливаем постоянным первым цветом градиента, чтобы
     * заголовок не менял оттенок при прокрутке. */
    int header_h = 0;
    if (self->toolbar_view) {
        header_h = adw_toolbar_view_get_top_bar_height(ADW_TOOLBAR_VIEW(self->toolbar_view));
        header_h = CLAMP(header_h, 0, height);
    }

    /* Однотонная заливка заголовка. Рисуется напрямую по фону страницы тем же
     * цветом, что и первый стоп градиента, поэтому в покое стык не заметен. */
    if (header_h > 0) {
        graphene_rect_t hrect = GRAPHENE_RECT_INIT(0, 0, width, header_h);
        gtk_snapshot_append_color(snapshot, &top, &hrect);
    }

    /* Градиент — только ниже шапки, привязанный к содержимому (сдвиг на
     * scroll_offset). Шапку исключаем клипом, иначе градиент наложился бы на её
     * однотонную заливку и затемнил заголовок. */
    if (height - header_h > 0) {
        graphene_rect_t content = GRAPHENE_RECT_INIT(0, header_h, width, height - header_h);
        gtk_snapshot_push_clip(snapshot, &content);

        graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);
        graphene_point_t start = GRAPHENE_POINT_INIT(0, header_h - self->scroll_offset);
        graphene_point_t end = GRAPHENE_POINT_INIT(0, height - self->scroll_offset);

        GskColorStop stops[2] = {
            { 0.0, top },
            { 1.0, bottom },
        };
        gtk_snapshot_append_linear_gradient(snapshot, &bounds, &start, &end, stops, 2);
        gtk_snapshot_pop(snapshot);
    }
}

static void album_backdrop_dispose(GObject *object) {
    AlbumBackdrop *self = MC_ALBUM_BACKDROP(object);
    if (self->toolbar_view) {
        g_object_remove_weak_pointer(G_OBJECT(self->toolbar_view),
                                     (gpointer *)&self->toolbar_view);
        self->toolbar_view = NULL;
    }
    G_OBJECT_CLASS(album_backdrop_parent_class)->dispose(object);
}

static void album_backdrop_class_init(AlbumBackdropClass *klass) {
    GTK_WIDGET_CLASS(klass)->snapshot = album_backdrop_snapshot;
    G_OBJECT_CLASS(klass)->dispose = album_backdrop_dispose;
}

static void album_backdrop_init(AlbumBackdrop *self) {
    self->has_color = FALSE;
    self->scroll_offset = 0.0;
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
}

static GtkWidget *album_backdrop_new(void) {
    return g_object_new(TYPE_ALBUM_BACKDROP, NULL);
}

static void album_backdrop_set_color(AlbumBackdrop *self, const GdkRGBA *color) {
    self->color = *color;
    self->has_color = TRUE;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

/* Прокрутка содержимого сдвигает градиент: он остаётся привязанным к трекам, а
 * не к окну. */
static void on_backdrop_scroll(GtkAdjustment *adj, gpointer user_data) {
    AlbumBackdrop *self = MC_ALBUM_BACKDROP(user_data);
    self->scroll_offset = gtk_adjustment_get_value(adj);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

/* Светлая тема: держим класс на странице включённым, тёмная — выключенным. Тему
 * берём из AdwStyleManager, а не из CSS media-запроса: GTK определяет
 * prefers-color-scheme по системной схеме, и она может не совпадать с темой
 * приложения (тогда светлый override применялся бы и в тёмной теме). */
static void on_style_manager_dark_changed(AdwStyleManager *sm, GParamSpec *pspec,
                                          gpointer user_data) {
    (void)pspec;
    GtkWidget *page = user_data;
    if (adw_style_manager_get_dark(sm))
        gtk_widget_remove_css_class(page, "album-light");
    else
        gtk_widget_add_css_class(page, "album-light");
}

/* Заголовок страницы (шапка с кнопкой «назад» и названием) по умолчанию
 * непрозрачный и перекрывал бы фон. Делаем его (и контейнер AdwToolbarView)
 * прозрачными, чтобы градиент доходил до самого верху. Список треков оставляем
 * карточкой boxed-list — так строки читаются отдельно от цветного фона. */
static void ensure_album_page_css(void) {
    static gboolean loaded = FALSE;
    if (loaded) return;
    loaded = TRUE;

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        ".album-detail headerbar {"
        "  background: none;"
        "  box-shadow: none;"
        "}"
        ".album-detail toolbarview {"
        "  background: none;"
        "}"
        /* В тёмной теме карточка списка полупрозрачная (@card_bg_color = 8%
         * белого), поэтому градиент сквозь неё виден. В светлой теме
         * @card_bg_color — плотный белый, который полностью скрывал градиент;
         * класс album-light (вешается из кода по AdwStyleManager) делает фон
         * карточки полупрозрачным, чтобы строки лежали на осветлённом градиенте. */
        ".album-detail.album-light list.boxed-list {"
        "  background-color: color-mix(in srgb, #ffffff 30%, transparent);"
        "}"
        ".album-genre {"
        "  padding: 2px 10px;"
        "  min-height: 0;"
        "  min-width: 0;"
        "  border-radius: 999px;"
        "  background: color-mix(in srgb, currentColor 10%, transparent);"
        "}");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

typedef struct {
    GtkWidget *hero_box;
    GtkWidget *meta_box;
    GtkWidget *cover_box;
    GtkWidget *year_lbl;
    GtkWidget *title_lbl;
    GtkWidget *artist_lbl;
    GtkWidget *genre_box;
    GtkWidget *play_btn;
    gboolean is_narrow;
    guint layout_idle_id;
} AlbumHeroLayout;

static void on_play_all_clicked(GtkButton *btn, gpointer user_data) {
    Player *player = user_data;
    GPtrArray *tracks = g_object_get_data(G_OBJECT(btn), "album-tracks");
    if (tracks && tracks->len > 0) {
        player_set_queue_and_play(player, tracks, 0);
    }
}

/* Кнопка-карандаш: правка тегов всех треков альбома. Массив треков берём из
 * данных кнопки (тот же, что у «Воспроизвести»). */
static void on_edit_tags_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    GPtrArray *tracks = g_object_get_data(G_OBJECT(btn), "album-tracks");
    if (edit_tags_cb && tracks && tracks->len > 0) {
        edit_tags_cb(tracks, edit_tags_data);
    }
}

static void on_album_track_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    Player *player = user_data;
    GPtrArray *tracks = g_object_get_data(G_OBJECT(box), "context-tracks");
    if (!tracks) return;

    /* Индекс берём с самой строки, а не через gtk_list_box_row_get_index: между
     * треками вставлены заголовки дисков, которые сдвигают нумерацию строк. */
    gpointer idx = g_object_get_data(G_OBJECT(row), "context-index");
    if (!idx) return;
    int index = (int)GPOINTER_TO_UINT(idx) - 1;
    if (index < 0 || index >= (int)tracks->len) return;

    player_set_queue_and_play(player, tracks, index);
}

/* Квадрат 320x320, внутри которого обложка вписывается без искажений и не растягивает контейнер */
static GtkWidget *create_fixed_cover_box(const char *cover_path) {
    const int box_size = 320;

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    /* Минимальная ширина обложки — 0 (иначе она раздувает минимальную ширину
     * страницы альбома и на узком окне AdwOverlaySplitView уходит в минус), но
     * минимальная высота сохранена, чтобы контейнер не сплющивал шапку. */
    gtk_widget_set_size_request(card, 0, box_size);
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(card, FALSE);
    gtk_widget_set_vexpand(card, FALSE);
    gtk_widget_set_overflow(card, GTK_OVERFLOW_HIDDEN);
    gtk_widget_add_css_class(card, "card");

    /* Обложку декодируем сразу до box_size, а минимальную ширину не задаём:
     * GtkPicture с can_shrink имеет минимум 0, а натуральный размер (box_size)
     * сохраняется. Большой минимум обложки раздувал минимальную ширину страницы
     * альбома и заставлял AdwOverlaySplitView уходить в отрицательные размеры на
     * узком окне. */
    GdkTexture *tex = cover_path ? ui_create_cover_texture(cover_path, box_size) : NULL;
    if (tex) {
        GtkWidget *aspect = gtk_aspect_frame_new(0.5f, 0.5f, 1.0f, FALSE);

        GtkWidget *pic = gtk_picture_new_for_paintable(GDK_PAINTABLE(tex));
        g_object_unref(tex);

        gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_CONTAIN);
        gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);

        gtk_aspect_frame_set_child(GTK_ASPECT_FRAME(aspect), pic);
        gtk_box_append(GTK_BOX(card), aspect);
        return card;
    }

    gtk_widget_set_size_request(card, box_size, box_size);
    GtkWidget *icon = gtk_image_new_from_icon_name("media-optical-cd-audio-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 96);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(icon, TRUE);
    gtk_widget_add_css_class(icon, "dim-label");
    gtk_box_append(GTK_BOX(card), icon);

    return card;
}

static void apply_hero_layout(AlbumHeroLayout *hl, gboolean narrow) {
    if (!hl || narrow == hl->is_narrow) return;
    hl->is_narrow = narrow;

    GtkAlign align = narrow ? GTK_ALIGN_CENTER : GTK_ALIGN_START;
    float xalign = narrow ? 0.5f : 0.0f;
    GtkJustification justify = narrow ? GTK_JUSTIFY_CENTER : GTK_JUSTIFY_LEFT;

    if (narrow) {
        gtk_orientable_set_orientation(GTK_ORIENTABLE(hl->hero_box), GTK_ORIENTATION_VERTICAL);
        gtk_box_set_spacing(GTK_BOX(hl->hero_box), 16);
        /* Сверху обложка, снизу данные */
        gtk_box_reorder_child_after(GTK_BOX(hl->hero_box), hl->meta_box, hl->cover_box);
        gtk_widget_set_halign(hl->meta_box, GTK_ALIGN_CENTER);
    } else {
        gtk_orientable_set_orientation(GTK_ORIENTABLE(hl->hero_box), GTK_ORIENTATION_HORIZONTAL);
        gtk_box_set_spacing(GTK_BOX(hl->hero_box), 28);
        /* Слева данные, справа обложка */
        gtk_box_reorder_child_after(GTK_BOX(hl->hero_box), hl->cover_box, hl->meta_box);
        gtk_widget_set_halign(hl->meta_box, GTK_ALIGN_FILL);
    }

    if (hl->year_lbl) {
        gtk_label_set_xalign(GTK_LABEL(hl->year_lbl), xalign);
        gtk_widget_set_halign(hl->year_lbl, align);
    }

    gtk_label_set_xalign(GTK_LABEL(hl->title_lbl), xalign);
    gtk_label_set_justify(GTK_LABEL(hl->title_lbl), justify);
    gtk_widget_set_halign(hl->title_lbl, align);

    gtk_label_set_xalign(GTK_LABEL(hl->artist_lbl), xalign);
    gtk_label_set_justify(GTK_LABEL(hl->artist_lbl), justify);
    gtk_widget_set_halign(hl->artist_lbl, align);

    if (hl->genre_box) gtk_widget_set_halign(hl->genre_box, align);

    gtk_widget_set_halign(hl->play_btn, align);
}

/* Ориентацией шапки управляет обёртка ниже: в момент раскладки она смотрит на
 * ширину окна и переключает шапку между горизонтальной и вертикальной. */

/* Ширина окна, ниже которой шапка складывается в столбик (обложка сверху). */
#define HERO_NARROW_WIDTH 820

#define DENCHIKU_TYPE_HERO_HOLDER (denchiku_hero_holder_get_type())
G_DECLARE_FINAL_TYPE(DenchikuHeroHolder, denchiku_hero_holder, DENCHIKU, HERO_HOLDER, GtkWidget)

struct _DenchikuHeroHolder {
    GtkWidget parent_instance;
    GtkWidget *child;
    AlbumHeroLayout *hl;
};

G_DEFINE_FINAL_TYPE(DenchikuHeroHolder, denchiku_hero_holder, GTK_TYPE_WIDGET)

/* В GTK4 нет сигнала о ресайзе окна, поэтому раскладку переключаем прямо в
 * allocate — он вызывается только при реальном изменении размера, без опроса
 * кадров. */
static void denchiku_hero_holder_measure(GtkWidget *widget, GtkOrientation orientation,
                                      int for_size, int *minimum, int *natural,
                                      int *minimum_baseline, int *natural_baseline) {
    DenchikuHeroHolder *self = DENCHIKU_HERO_HOLDER(widget);
    if (self->child) {
        gtk_widget_measure(self->child, orientation, for_size,
                           minimum, natural, minimum_baseline, natural_baseline);
    } else {
        if (minimum) *minimum = 0;
        if (natural) *natural = 0;
    }
}

/* Менять ориентацию/порядок нельзя прямо в allocate: вызванный оттуда
 * gtk_widget_queue_resize() GTK молча игнорирует, и шапка осталась бы
 * выделенной по размерам прежней ориентации. Поэтому переключаем раскладку
 * отложенно (в idle), когда раскладка уже завершена. */
static gboolean hero_apply_idle(gpointer data) {
    AlbumHeroLayout *hl = data;
    hl->layout_idle_id = 0;
    GtkRoot *root = gtk_widget_get_root(hl->hero_box);
    int avail = (root && GTK_IS_WINDOW(root)) ? gtk_widget_get_width(GTK_WIDGET(root)) : 0;
    if (avail > 0) apply_hero_layout(hl, avail < HERO_NARROW_WIDTH);
    return G_SOURCE_REMOVE;
}

static void hero_layout_free(gpointer data) {
    AlbumHeroLayout *hl = data;
    if (hl->layout_idle_id) g_source_remove(hl->layout_idle_id);
    g_free(hl);
}

static void denchiku_hero_holder_allocate(GtkWidget *widget, int width, int height, int baseline) {
    DenchikuHeroHolder *self = DENCHIKU_HERO_HOLDER(widget);
    if (!self->child) return;
    /* Решение принимаем по ширине окна: страница альбома лежит в GtkScrolledWindow
     * с горизонтальной политикой NEVER, который выдаёт содержимому его
     * минимальную ширину, поэтому по собственной ширине шапка «залипала» бы в
     * горизонтальном виде. */
    GtkRoot *root = gtk_widget_get_root(widget);
    int avail = (root && GTK_IS_WINDOW(root)) ? gtk_widget_get_width(GTK_WIDGET(root)) : 0;
    if (avail > 0) {
        gboolean narrow = avail < HERO_NARROW_WIDTH;
        if (narrow != self->hl->is_narrow && self->hl->layout_idle_id == 0) {
            self->hl->layout_idle_id = g_idle_add(hero_apply_idle, self->hl);
        }
    }
    gtk_widget_allocate(self->child, width, height, baseline, NULL);
}

static void denchiku_hero_holder_dispose(GObject *object) {
    DenchikuHeroHolder *self = DENCHIKU_HERO_HOLDER(object);
    g_clear_pointer(&self->child, gtk_widget_unparent);
    G_OBJECT_CLASS(denchiku_hero_holder_parent_class)->dispose(object);
}

static void denchiku_hero_holder_class_init(DenchikuHeroHolderClass *klass) {
    G_OBJECT_CLASS(klass)->dispose = denchiku_hero_holder_dispose;
}

static void denchiku_hero_holder_init(DenchikuHeroHolder *self) {
    self->child = NULL;
    self->hl = NULL;
    GtkLayoutManager *layout = gtk_custom_layout_new(NULL,
        denchiku_hero_holder_measure, denchiku_hero_holder_allocate);
    gtk_widget_set_layout_manager(GTK_WIDGET(self), layout);
}

static GtkWidget *denchiku_hero_holder_new(AlbumHeroLayout *hl) {
    DenchikuHeroHolder *holder = g_object_new(DENCHIKU_TYPE_HERO_HOLDER, NULL);
    holder->hl = hl;
    holder->child = hl->hero_box;
    gtk_widget_set_parent(hl->hero_box, GTK_WIDGET(holder));
    return GTK_WIDGET(holder);
}

/* Прозрачный разделитель перед группой треков одного диска. Это не строка
 * списка (list.boxed-list стилизует только row), поэтому фон остаётся фоном
 * карточки, а разделитель читается как подпись секции, а не как узкий трек. */
static GtkWidget *create_disc_separator(guint disc, const char *subtitle) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(box, 14);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);

    char *text = (subtitle && *subtitle)
        ? g_strdup_printf("Диск %u · %s", disc > 0 ? disc : 1, subtitle)
        : g_strdup_printf("Диск %u", disc > 0 ? disc : 1);
    GtkWidget *lbl = gtk_label_new(text);
    g_free(text);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(lbl, "dim-label");
    gtk_widget_add_css_class(lbl, "caption-heading");
    gtk_box_append(GTK_BOX(box), lbl);

    return box;
}

/* Заголовок-разделитель ставится перед первым треком диска и не добавляет
 * отдельную строку списка, поэтому нумерация строк не сдвигается. Показываем
 * только у многодисковых альбомов (или если у дисков есть подзаголовки). */
static void album_track_header_func(GtkListBoxRow *row, GtkListBoxRow *before, gpointer user_data) {
    if (!GPOINTER_TO_INT(user_data)) {
        gtk_list_box_row_set_header(row, NULL);
        return;
    }

    Track *t = g_object_get_data(G_OBJECT(row), "track");
    if (!t) {
        gtk_list_box_row_set_header(row, NULL);
        return;
    }

    Track *prev = before ? g_object_get_data(G_OBJECT(before), "track") : NULL;
    if (prev && prev->disc_number == t->disc_number) {
        gtk_list_box_row_set_header(row, NULL);
        return;
    }

    gtk_list_box_row_set_header(row, create_disc_separator(t->disc_number, t->disc_subtitle));
}

static GtkWidget *create_album_track_row(Player *player, Track *t, const char *playing_uri,
                                         guint index) {
    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), t->title ? t->title : "Без названия");
    adw_action_row_set_title_lines(row, 2);

    if (t->artist && g_strcmp0(t->artist, t->album_artist) != 0) {
        adw_action_row_set_subtitle(row, t->artist);
        adw_action_row_set_subtitle_lines(row, 1);
    }

    char *num_str = (t->track_number > 0)
        ? g_strdup_printf("%u", t->track_number)
        : g_strdup("—");
    GtkWidget *num_lbl = gtk_label_new(num_str);
    g_free(num_str);

    gtk_widget_set_size_request(num_lbl, 28, -1);
    gtk_label_set_xalign(GTK_LABEL(num_lbl), 0.5);
    gtk_widget_add_css_class(num_lbl, "dim-label");
    gtk_widget_add_css_class(num_lbl, "numeric");
    adw_action_row_add_prefix(row, num_lbl);

    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    g_object_set_data(G_OBJECT(row), "track", t);
    /* Индекс в массиве треков (значение — индекс + 1, чтобы 0 значило «нет»). */
    g_object_set_data(G_OBJECT(row), "context-index", GUINT_TO_POINTER(index + 1));

    if (playing_uri && g_strcmp0(t->uri, playing_uri) == 0) {
        ui_set_row_playing(row, TRUE);
    }

    track_menu_attach(GTK_WIDGET(row), t, TRACK_CONTEXT_ALBUM, player);

    return GTK_WIDGET(row);
}

/* ===== секции «другие альбомы» ===== */

#define OTHER_CARD_COVER_SIZE 120

/* Описание альбома для карточки в секции. */
typedef struct {
    char *album;
    char *album_artist;
    char *cover;
    guint year;
} OtherAlbum;

static void other_album_free(gpointer data) {
    OtherAlbum *oa = data;
    g_free(oa->album);
    g_free(oa->album_artist);
    g_free(oa->cover);
    g_free(oa);
}

static void on_other_album_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    const char *album = g_object_get_data(G_OBJECT(btn), "album");
    const char *album_artist = g_object_get_data(G_OBJECT(btn), "album-artist");
    if (open_album_cb && album && *album) {
        open_album_cb(album, album_artist, open_album_data);
    }
}

static GtkWidget *create_other_album_card(OtherAlbum *oa) {
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_set_valign(btn, GTK_ALIGN_START);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(box, OTHER_CARD_COVER_SIZE + 12, -1);
    gtk_box_append(GTK_BOX(box), ui_create_cover_widget(oa->cover, OTHER_CARD_COVER_SIZE));

    GtkWidget *title = gtk_label_new(oa->album ? oa->album : "");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title), (OTHER_CARD_COVER_SIZE + 12) / 8);
    gtk_box_append(GTK_BOX(box), title);

    if (oa->year > 0) {
        char *ys = g_strdup_printf("%u", oa->year);
        GtkWidget *year_lbl = gtk_label_new(ys);
        g_free(ys);
        gtk_label_set_xalign(GTK_LABEL(year_lbl), 0.0f);
        gtk_widget_add_css_class(year_lbl, "dim-label");
        gtk_widget_add_css_class(year_lbl, "caption");
        gtk_box_append(GTK_BOX(box), year_lbl);
    }

    gtk_button_set_child(GTK_BUTTON(btn), box);
    g_object_set_data_full(G_OBJECT(btn), "album", g_strdup(oa->album ? oa->album : ""), g_free);
    g_object_set_data_full(G_OBJECT(btn), "album-artist",
                           g_strdup(oa->album_artist ? oa->album_artist : ""), g_free);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_other_album_clicked), NULL);
    return btn;
}

/* Собирает другие альбомы исполнителя artist: пары (название, album_artist),
 * которые (с учётом разбиения) принадлежат artist, кроме текущего альбома.
 * Возвращает GPtrArray<OtherAlbum*>. */
static GPtrArray *collect_other_albums(GPtrArray *all_tracks, const char *artist,
                                       const char *cur_album, const char *cur_album_artist) {
    GPtrArray *out = g_ptr_array_new_with_free_func(other_album_free);
    GHashTable *by_key = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (guint i = 0; i < all_tracks->len; i++) {
        Track *t = g_ptr_array_index(all_tracks, i);
        if (!t->album || !*t->album) continue;
        if (g_strcmp0(t->album, cur_album) == 0 &&
            g_strcmp0(t->album_artist, cur_album_artist) == 0) {
            continue; /* сам открытый альбом */
        }
        if (!tag_has_artist(t->album_artist, artist)) continue;

        char *key = g_strdup_printf("%s\x1f%s",
                                    t->album, t->album_artist ? t->album_artist : "");
        if (g_hash_table_contains(by_key, key)) {
            g_free(key);
            continue;
        }
        g_hash_table_add(by_key, key);

        OtherAlbum *oa = g_new0(OtherAlbum, 1);
        oa->album = g_strdup(t->album);
        oa->album_artist = g_strdup(t->album_artist);
        oa->cover = g_strdup(t->cover_path);
        oa->year = t->year;
        g_ptr_array_add(out, oa);
    }

    g_hash_table_destroy(by_key);
    return out;
}

/* Заголовок с именем исполнителя и горизонтальная полка карточек. */
static GtkWidget *create_other_albums_section(const char *artist, GPtrArray *albums) {
    GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

    char *heading = g_strdup_printf("Другие альбомы · %s", artist);
    GtkWidget *lbl = gtk_label_new(heading);
    g_free(heading);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(lbl, "title-4");
    gtk_box_append(GTK_BOX(section), lbl);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_widget_set_vexpand(scrolled, FALSE);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    for (guint i = 0; i < albums->len; i++) {
        gtk_box_append(GTK_BOX(row), create_other_album_card(g_ptr_array_index(albums, i)));
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), row);
    gtk_box_append(GTK_BOX(section), scrolled);
    return section;
}

/* Собирает уникальные жанры альбома: у треков одного альбома тег может
 * отличаться, а внутри тега жанры разделяются (см. track_split_genres), поэтому
 * объединяем разбиение всех треков, сохраняя порядок появления. */
static GPtrArray *collect_album_genres(GPtrArray *tracks) {
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (guint i = 0; i < tracks->len; i++) {
        Track *t = g_ptr_array_index(tracks, i);
        char **genres = track_split_genres(t->genre);
        if (!genres) continue;
        for (guint j = 0; genres[j] != NULL; j++) {
            if (g_hash_table_contains(seen, genres[j])) continue;
            g_hash_table_add(seen, g_strdup(genres[j]));
            g_ptr_array_add(out, g_strdup(genres[j]));
        }
        g_strfreev(genres);
    }

    g_hash_table_destroy(seen);
    return out;
}

/* Клик по жанровому чипу: переходим на вкладку «Жанры» с этим жанром (колбэк
 * зарегистрирован окном). */
static void on_genre_chip_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    const char *genre = g_object_get_data(G_OBJECT(btn), "genre");
    if (open_genre_cb && genre && *genre) open_genre_cb(genre, open_genre_data);
}

/* Ряд жанровых «чипов» в шапке. AdwWrapBox переносит чипы как слова в тексте (без
 * выравнивания по сетке), поэтому длинный список жанров не растягивает страницу.
 * Чипы кликабельны и ведут на вкладку «Жанры». */
static GtkWidget *create_genre_chips(GPtrArray *genres) {
    GtkWidget *box = adw_wrap_box_new();
    gtk_orientable_set_orientation(GTK_ORIENTABLE(box), GTK_ORIENTATION_HORIZONTAL);
    adw_wrap_box_set_child_spacing(ADW_WRAP_BOX(box), 6);
    adw_wrap_box_set_line_spacing(ADW_WRAP_BOX(box), 6);

    for (guint i = 0; i < genres->len; i++) {
        const char *name = g_ptr_array_index(genres, i);
        GtkWidget *chip = gtk_button_new_with_label(name);
        gtk_label_set_ellipsize(
            GTK_LABEL(gtk_button_get_child(GTK_BUTTON(chip))), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(chip, "caption");
        gtk_widget_add_css_class(chip, "album-genre");
        g_object_set_data_full(G_OBJECT(chip), "genre", g_strdup(name), g_free);
        g_signal_connect(chip, "clicked", G_CALLBACK(on_genre_chip_clicked), NULL);
        adw_wrap_box_append(ADW_WRAP_BOX(box), chip);
    }
    return box;
}

AdwNavigationPage *album_detail_page_create(Player *player,
                                            GPtrArray *all_tracks,
                                            const char *album_name,
                                            const char *album_artist,
                                            GtkListBox **out_track_list) {
    if (!all_tracks || !album_name) return NULL;

    GPtrArray *filtered = g_ptr_array_new();
    for (guint i = 0; i < all_tracks->len; i++) {
        Track *t = g_ptr_array_index(all_tracks, i);
        if (g_strcmp0(t->album, album_name) == 0 &&
            g_strcmp0(t->album_artist, album_artist) == 0) {
            g_ptr_array_add(filtered, t);
        }
    }

    if (filtered->len == 0) {
        g_ptr_array_unref(filtered);
        return NULL;
    }

    g_ptr_array_sort(filtered, ui_compare_tracks_by_number);
    Track *first = g_ptr_array_index(filtered, 0);

    GtkWidget *outer_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(outer_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 860);
    adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 500);
    gtk_widget_set_margin_top(clamp, 24);
    gtk_widget_set_margin_bottom(clamp, 24);
    gtk_widget_set_margin_start(clamp, 16);
    gtk_widget_set_margin_end(clamp, 16);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);

    /* Адаптивная шапка на стандартном GtkBox (переключается только при ресайзе окна) */
    AlbumHeroLayout *hl = g_new0(AlbumHeroLayout, 1);
    hl->is_narrow = FALSE;

    hl->hero_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 28);
    hl->meta_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_valign(hl->meta_box, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(hl->meta_box, TRUE);

    if (first->year > 0) {
        char *year_str = g_strdup_printf("%u", first->year);
        hl->year_lbl = gtk_label_new(year_str);
        gtk_label_set_xalign(GTK_LABEL(hl->year_lbl), 0.0f);
        gtk_widget_set_halign(hl->year_lbl, GTK_ALIGN_START);
        gtk_widget_add_css_class(hl->year_lbl, "dim-label");
        gtk_widget_add_css_class(hl->year_lbl, "caption-heading");
        gtk_box_append(GTK_BOX(hl->meta_box), hl->year_lbl);
        g_free(year_str);
    }

    hl->title_lbl = gtk_label_new(first->album);
    gtk_label_set_xalign(GTK_LABEL(hl->title_lbl), 0.0f);
    gtk_widget_set_halign(hl->title_lbl, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(hl->title_lbl), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(hl->title_lbl), PANGO_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(hl->title_lbl, "title-1");
    gtk_box_append(GTK_BOX(hl->meta_box), hl->title_lbl);

    hl->artist_lbl = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(hl->artist_lbl), 0.0f);
    gtk_widget_set_halign(hl->artist_lbl, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(hl->artist_lbl), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(hl->artist_lbl), PANGO_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(hl->artist_lbl, "title-4");
    /* Имя исполнителя — ссылка (по одной на каждого исполнителя с учётом
     * разбиения), клик открывает его страницу. */
    char *artist_markup = build_artist_markup(first->album_artist);
    if (artist_markup) {
        gtk_label_set_markup(GTK_LABEL(hl->artist_lbl), artist_markup);
        g_free(artist_markup);
        g_signal_connect(hl->artist_lbl, "activate-link",
                         G_CALLBACK(on_artist_link_activated), NULL);
    } else {
        gtk_label_set_text(GTK_LABEL(hl->artist_lbl),
                           first->album_artist ? first->album_artist : "");
    }
    gtk_box_append(GTK_BOX(hl->meta_box), hl->artist_lbl);

    GPtrArray *genres = collect_album_genres(filtered);
    if (genres->len > 0) {
        hl->genre_box = create_genre_chips(genres);
        gtk_widget_set_margin_top(hl->genre_box, 8);
        gtk_widget_set_halign(hl->genre_box, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(hl->meta_box), hl->genre_box);
    }
    g_ptr_array_unref(genres);

    hl->play_btn = gtk_button_new_with_label("Воспроизвести");
    gtk_widget_set_halign(hl->play_btn, GTK_ALIGN_START);
    gtk_widget_set_margin_top(hl->play_btn, 12);
    gtk_widget_add_css_class(hl->play_btn, "suggested-action");
    gtk_widget_add_css_class(hl->play_btn, "pill");
    g_object_set_data_full(G_OBJECT(hl->play_btn), "album-tracks",
                           g_ptr_array_ref(filtered), (GDestroyNotify)g_ptr_array_unref);
    g_signal_connect(hl->play_btn, "clicked", G_CALLBACK(on_play_all_clicked), player);

    /* Ряд действий шапки: «Воспроизвести» + кнопка-карандаш правки тегов всех
     * треков альбома. */
    GtkWidget *actions_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions_row, GTK_ALIGN_START);
    gtk_widget_set_margin_top(actions_row, 12);
    gtk_box_append(GTK_BOX(actions_row), hl->play_btn);

    GtkWidget *edit_btn = gtk_button_new_from_icon_name("document-edit-symbolic");
    gtk_widget_add_css_class(edit_btn, "pill");
    gtk_widget_add_css_class(edit_btn, "flat");
    gtk_widget_set_valign(edit_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(edit_btn, "Редактировать теги альбома");
    g_object_set_data_full(G_OBJECT(edit_btn), "album-tracks",
                           g_ptr_array_ref(filtered), (GDestroyNotify)g_ptr_array_unref);
    g_signal_connect(edit_btn, "clicked", G_CALLBACK(on_edit_tags_clicked), player);
    gtk_box_append(GTK_BOX(actions_row), edit_btn);

    /* Кнопка-действие шапки должна выравниваться вместе с остальными
     * элементами meta_box при узком окне. */
    hl->play_btn = actions_row;
    gtk_box_append(GTK_BOX(hl->meta_box), actions_row);
    hl->cover_box = create_fixed_cover_box(first->cover_path);
    gtk_box_append(GTK_BOX(hl->hero_box), hl->meta_box);
    gtk_box_append(GTK_BOX(hl->hero_box), hl->cover_box);
    g_object_set_data_full(G_OBJECT(hl->hero_box), "hero-layout", hl, hero_layout_free);
    gtk_box_append(GTK_BOX(vbox), denchiku_hero_holder_new(hl));
/* Список треков альбома */
GtkListBox *sub_list = GTK_LIST_BOX(gtk_list_box_new());
gtk_widget_add_css_class(GTK_WIDGET(sub_list), "boxed-list");
gtk_list_box_set_selection_mode(sub_list, GTK_SELECTION_NONE);
g_object_set_data_full(G_OBJECT(sub_list), "context-tracks",
g_ptr_array_ref(filtered), (GDestroyNotify)g_ptr_array_unref);
ui_pin_tracks_reference(GTK_WIDGET(sub_list), all_tracks);
g_signal_connect(sub_list, "row-activated", G_CALLBACK(on_album_track_activated), player);
Track *cur = player_get_current_track(player);
const char *playing_uri = cur ? cur->uri : NULL;
/* Разделители дисков показываем только у многодисковых альбомов или когда у
 * дисков есть подзаголовок, иначе список однодисковых альбомов получил бы
 * лишний заголовок «Диск 1». */
guint max_disc = 0;
gboolean any_subtitle = FALSE;
for (guint i = 0; i < filtered->len; i++) {
    Track *t = g_ptr_array_index(filtered, i);
    if (t->disc_number > max_disc) max_disc = t->disc_number;
    if (t->disc_subtitle && *t->disc_subtitle) any_subtitle = TRUE;
}
gboolean show_discs = max_disc > 1 || any_subtitle;
gtk_list_box_set_header_func(sub_list, album_track_header_func,
                             GINT_TO_POINTER(show_discs), NULL);
for (guint i = 0; i < filtered->len; i++) {
    Track *t = g_ptr_array_index(filtered, i);
    gtk_list_box_append(sub_list, create_album_track_row(player, t, playing_uri, i));
}
if (out_track_list) *out_track_list = sub_list;
g_ptr_array_unref(filtered);
gtk_box_append(GTK_BOX(vbox), GTK_WIDGET(sub_list));

/* Под списком треков — «другие альбомы» каждого исполнителя альбома. При
 * нескольких исполнителях секций столько же. */
char **album_artists = track_split_artists(album_artist);
if (album_artists) {
    GHashTable *seen_artists = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; album_artists[i] != NULL; i++) {
        const char *artist = album_artists[i];
        if (!*artist || g_hash_table_contains(seen_artists, artist)) continue;
        g_hash_table_add(seen_artists, g_strdup(artist));

        GPtrArray *others = collect_other_albums(all_tracks, artist, album_name, album_artist);
        if (others->len > 0) {
            gtk_box_append(GTK_BOX(vbox), create_other_albums_section(artist, others));
        }
        g_ptr_array_unref(others);
    }
    g_hash_table_destroy(seen_artists);
    g_strfreev(album_artists);
}

adw_clamp_set_child(ADW_CLAMP(clamp), vbox);
gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(outer_scroll), clamp);
AdwToolbarView *tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
AdwHeaderBar *hb = ADW_HEADER_BAR(adw_header_bar_new());
adw_header_bar_set_show_start_title_buttons(hb, FALSE);
adw_toolbar_view_add_top_bar(tv, GTK_WIDGET(hb));
adw_toolbar_view_set_content(tv, outer_scroll);
/* Цветной градиент из обложки кладём под всю страницу, поэтому он закрашивает и
 * область заголовка. Заголовок делаем прозрачным через CSS (см. ensure_album_page_css). */
    GtkWidget *backdrop = album_backdrop_new();
    GdkRGBA accent;
    if (ui_cover_dominant_color(first->cover_path, &accent)) {
        album_backdrop_set_color(MC_ALBUM_BACKDROP(backdrop), &accent);
    }
    /* Слабая ссылка на тулбар: по ней фон берёт высоту шапки, чтобы залить её
     * постоянным цветом. */
    MC_ALBUM_BACKDROP(backdrop)->toolbar_view = GTK_WIDGET(tv);
    g_object_add_weak_pointer(G_OBJECT(tv),
                              (gpointer *)&MC_ALBUM_BACKDROP(backdrop)->toolbar_view);
    /* Градиент сдвигается вместе с содержимым: прокрутка списка треков тянет
     * цветной фон за собой, а не оставляет его неподвижным фоном окна. */
    g_signal_connect(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(outer_scroll)),
                     "value-changed", G_CALLBACK(on_backdrop_scroll), backdrop);
    GtkWidget *page_overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(page_overlay), backdrop);
    gtk_widget_set_hexpand(GTK_WIDGET(tv), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(tv), TRUE);
    gtk_widget_set_halign(GTK_WIDGET(tv), GTK_ALIGN_FILL);
    gtk_widget_set_valign(GTK_WIDGET(tv), GTK_ALIGN_FILL);
    gtk_overlay_add_overlay(GTK_OVERLAY(page_overlay), GTK_WIDGET(tv));
    gtk_widget_add_css_class(page_overlay, "album-detail");
    /* Светлый режим помечаем классом; тему берём из AdwStyleManager и следим за
     * переключением, пока жива страница (g_signal_connect_object сам отключит
     * обработчик при её уничтожении). */
    AdwStyleManager *sm = adw_style_manager_get_default();
    if (!adw_style_manager_get_dark(sm)) {
        gtk_widget_add_css_class(page_overlay, "album-light");
    }
    g_signal_connect_object(sm, "notify::dark",
                            G_CALLBACK(on_style_manager_dark_changed), page_overlay, 0);
    ensure_album_page_css();
    AdwNavigationPage *page = adw_navigation_page_new(page_overlay, album_name);
    /* Регистрируем список треков для подсветки играющего: если страницу открыл
     * другой раздел (исполнители/жанры/треки), его обновление подсветки найдёт
     * список по этому ключу. */
    GPtrArray *lists = g_ptr_array_new();
    g_ptr_array_add(lists, sub_list);
    g_object_set_data_full(G_OBJECT(page), "playing-listboxes", lists,
                           (GDestroyNotify)g_ptr_array_unref);
    return page;
}