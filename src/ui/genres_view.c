#include "genres_view.h"
#include "album_detail_page.h"
#include "artist_detail_page.h"
#include "track.h"
#include "ui_helpers.h"

/* Размер обложек карточек в секциях «Альбомы» и «Исполнители». */
#define GENRE_CARD_COVER_SIZE 120
/* Сколько карточек рисуем в свёрнутой полосе секции. Если элементов больше,
 * кнопка «Показать все» показывается без измерения раскладки: часть карточек
 * точно скрыта за краем. Так мы не строим тысячи карточек, пока секция свёрнута. */
#define COLLAPSED_CARD_CAP 24
/* Размер обложек на странице «Показать все» (полноразмерная сетка). */
#define GENRE_GRID_COVER_SIZE 160

typedef struct _GenresView GenresView;
typedef struct _Section Section;

/* Какая секция: от неё зависит, что открывать по клику карточки и как строить
 * страницу «Показать все». */
typedef enum {
    SECTION_ALBUMS,
    SECTION_ARTISTS
} SectionKind;

/* Описание карточки альбома, собранное из отфильтрованных треков. */
typedef struct {
    char *album;
    char *artist;
    char *cover;
    guint year;
} AlbumCard;

/* Описание карточки исполнителя. */
typedef struct {
    char *name;
    char *cover;
} ArtistCard;

typedef GtkWidget *(*SectionMakeCardFn)(Section *section, gpointer item);

/* Секция результатов: заголовок с кнопкой «Показать все» и одна горизонтальная
 * полоса карточек. Кнопка открывает полноразмерную сетку на отдельной странице. */
struct _Section {
    GenresView *gv;
    SectionKind kind;
    GtkWidget *box;
    GtkWidget *showall_btn;
    GtkWidget *row_scroll;
    GtkWidget *row_box;
    gboolean overflow;   /* в полосе есть карточки за краем */
    SectionMakeCardFn make_card;
    GPtrArray *cards;    /* заимствованный массив описаний карточек */
};

/* ===== элементы модели списка =====
 * Список результатов — виртуализированный GtkListView, и его первым элементом
 * идёт «шапка» (чипы и секции). Так шапка прокручивается вместе со списком и не
 * живёт отдельным фреймом, а сами треки строятся только для видимой области. */

#define GENRE_HEADER_ITEM_TYPE (genre_header_item_get_type())
G_DECLARE_FINAL_TYPE(GenreHeaderItem, genre_header_item, DENCHIKU, GENRE_HEADER_ITEM, GObject)
struct _GenreHeaderItem { GObject parent_instance; };
G_DEFINE_FINAL_TYPE(GenreHeaderItem, genre_header_item, G_TYPE_OBJECT)
static void genre_header_item_class_init(GenreHeaderItemClass *klass) { (void)klass; }
static void genre_header_item_init(GenreHeaderItem *self) { (void)self; }

#define GENRE_TRACK_ITEM_TYPE (genre_track_item_get_type())
G_DECLARE_FINAL_TYPE(GenreTrackItem, genre_track_item, DENCHIKU, GENRE_TRACK_ITEM, GObject)
struct _GenreTrackItem {
    GObject parent_instance;
    Track *track;      /* заимствованный указатель */
    gboolean playing;
};
enum {
    TRACK_ITEM_PROP_0,
    TRACK_ITEM_PROP_PLAYING,
    TRACK_ITEM_N_PROPS
};
static GParamSpec *track_item_props[TRACK_ITEM_N_PROPS];

G_DEFINE_FINAL_TYPE(GenreTrackItem, genre_track_item, G_TYPE_OBJECT)

static void genre_track_item_set_property(GObject *obj, guint prop_id,
                                          const GValue *value, GParamSpec *pspec) {
    GenreTrackItem *self = DENCHIKU_GENRE_TRACK_ITEM(obj);
    switch (prop_id) {
        case TRACK_ITEM_PROP_PLAYING: self->playing = g_value_get_boolean(value); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
    }
}

static void genre_track_item_get_property(GObject *obj, guint prop_id,
                                          GValue *value, GParamSpec *pspec) {
    GenreTrackItem *self = DENCHIKU_GENRE_TRACK_ITEM(obj);
    switch (prop_id) {
        case TRACK_ITEM_PROP_PLAYING: g_value_set_boolean(value, self->playing); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
    }
}

static void genre_track_item_class_init(GenreTrackItemClass *klass) {
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->set_property = genre_track_item_set_property;
    oc->get_property = genre_track_item_get_property;

    track_item_props[TRACK_ITEM_PROP_PLAYING] =
        g_param_spec_boolean("playing", NULL, NULL, FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_NAME);
    g_object_class_install_properties(oc, TRACK_ITEM_N_PROPS, track_item_props);
}

static void genre_track_item_init(GenreTrackItem *self) { (void)self; }

static GenreTrackItem *genre_track_item_new(Track *t) {
    GenreTrackItem *item = g_object_new(GENRE_TRACK_ITEM_TYPE, NULL);
    item->track = t;
    return item;
}

/* ===== элемент полноразмерной сетки «Показать все» =====
 * Одна карточка альбома или исполнителя. Хранит всё нужное для отображения и
 * открытия страницы. */
#define GENRE_GRID_ITEM_TYPE (genre_grid_item_get_type())
G_DECLARE_FINAL_TYPE(GenreGridItem, genre_grid_item, DENCHIKU, GENRE_GRID_ITEM, GObject)
struct _GenreGridItem {
    GObject parent_instance;
    char *title;
    char *subtitle;      /* исполнитель альбома; NULL для исполнителей */
    char *cover;
    char *album_artist;  /* для открытия альбома */
    gboolean is_artist;
};

G_DEFINE_FINAL_TYPE(GenreGridItem, genre_grid_item, G_TYPE_OBJECT)

static void genre_grid_item_finalize(GObject *object) {
    GenreGridItem *self = DENCHIKU_GENRE_GRID_ITEM(object);
    g_free(self->title);
    g_free(self->subtitle);
    g_free(self->cover);
    g_free(self->album_artist);
    G_OBJECT_CLASS(genre_grid_item_parent_class)->finalize(object);
}

static void genre_grid_item_class_init(GenreGridItemClass *klass) {
    G_OBJECT_CLASS(klass)->finalize = genre_grid_item_finalize;
}

static void genre_grid_item_init(GenreGridItem *self) { (void)self; }

struct _GenresView {
    Player *player;
    GPtrArray *tracks;   /* вся медиатека (владеет ссылкой) */
    AdwNavigationView *nav;
    AdwNavigationPage *root;

    GPtrArray *selected; /* выбранные жанры (char*) в порядке выбора */

    GtkWidget *chips_flow;
    GtkWidget *reset_btn;
    GtkWidget *placeholder;

    Section albums;
    Section artists;
    GtkWidget *tracks_header;
    GtkWidget *tracks_count_lbl;

    /* Шапка списка (чипы и секции) — первый элемент модели, живёт вне списка,
     * чтобы сохранять состояние между привязками. */
    GtkWidget *header_widget;

    GListStore *store;   /* GenreHeaderItem + GenreTrackItem */

    GPtrArray *album_cards;  /* AlbumCard* */
    GPtrArray *artist_cards; /* ArtistCard* */
    GPtrArray *filtered;     /* Track* (заимствованы) */
};

static void apply_filter(GenresView *gv);
static void toggle_genre(GenresView *gv, const char *genre);

/* ================= оформление ================= */

static void ensure_genre_css(void) {
    static gboolean loaded = FALSE;
    if (loaded) return;
    loaded = TRUE;

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        ".genre-chip { border-radius: 999px; }"
        ".genre-chip.genre-chip-on {"
        "  background-color: @accent_bg_color;"
        "  color: @accent_fg_color;"
        "}"
        ".genre-chip.genre-chip-on label { color: @accent_fg_color; }"
        ".genre-chip.genre-chip-on image { color: @accent_fg_color; }"
        ".genre-track-playing { color: @accent_color; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* Удаляет всех детей контейнера-бокса, flow-бокса или wrap-бокса. */
static void container_clear(GtkWidget *container) {
    GtkWidget *child = gtk_widget_get_first_child(container);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        if (GTK_IS_FLOW_BOX(container)) {
            gtk_flow_box_remove(GTK_FLOW_BOX(container), child);
        } else if (ADW_IS_WRAP_BOX(container)) {
            adw_wrap_box_remove(ADW_WRAP_BOX(container), child);
        } else if (GTK_IS_BOX(container)) {
            gtk_box_remove(GTK_BOX(container), child);
        }
        child = next;
    }
}

/* ================= фильтрация ================= */

/* Есть ли у трека жанр name (с учётом разбиения тега на несколько жанров). */
static gboolean track_has_genre(Track *t, const char *name) {
    char **genres = track_split_genres(t->genre);
    if (!genres) return FALSE;

    gboolean found = FALSE;
    for (guint i = 0; genres[i] != NULL; i++) {
        if (g_strcmp0(genres[i], name) == 0) {
            found = TRUE;
            break;
        }
    }
    g_strfreev(genres);
    return found;
}

/* Трек проходит фильтр, если содержит ВСЕ выбранные жанры: каждый следующий
 * клик по жанру только сужает выборку. */
static gboolean track_matches(GenresView *gv, Track *t) {
    for (guint i = 0; i < gv->selected->len; i++) {
        if (!track_has_genre(t, g_ptr_array_index(gv->selected, i))) return FALSE;
    }
    return TRUE;
}

static gboolean is_selected(GenresView *gv, const char *genre) {
    for (guint i = 0; i < gv->selected->len; i++) {
        if (g_strcmp0(g_ptr_array_index(gv->selected, i), genre) == 0) return TRUE;
    }
    return FALSE;
}

/* ================= вспомогательные освобождения и сравнения ================= */

static void album_card_free(gpointer data) {
    AlbumCard *a = data;
    g_free(a->album);
    g_free(a->artist);
    g_free(a->cover);
    g_free(a);
}

static void artist_card_free(gpointer data) {
    ArtistCard *a = data;
    g_free(a->name);
    g_free(a->cover);
    g_free(a);
}

static void artist_bucket_free(gpointer data) {
    g_ptr_array_unref(data);
}

static gint cmp_str(gconstpointer a, gconstpointer b) {
    const char *s1 = *(const char * const *)a;
    const char *s2 = *(const char * const *)b;
    return g_utf8_collate(s1 ? s1 : "", s2 ? s2 : "");
}

static gint cmp_album_card(gconstpointer a, gconstpointer b) {
    const AlbumCard *x = *(const AlbumCard * const *)a;
    const AlbumCard *y = *(const AlbumCard * const *)b;
    int r = g_utf8_collate(x->album ? x->album : "", y->album ? y->album : "");
    if (r == 0) r = g_utf8_collate(x->artist ? x->artist : "", y->artist ? y->artist : "");
    return r;
}

static gint cmp_artist_card(gconstpointer a, gconstpointer b) {
    const ArtistCard *x = *(const ArtistCard * const *)a;
    const ArtistCard *y = *(const ArtistCard * const *)b;
    return g_utf8_collate(x->name ? x->name : "", y->name ? y->name : "");
}

/* ================= жанровые «чипы» ================= */

static void toggle_genre(GenresView *gv, const char *genre) {
    if (!genre || !*genre) return;

    for (guint i = 0; i < gv->selected->len; i++) {
        if (g_strcmp0(g_ptr_array_index(gv->selected, i), genre) == 0) {
            g_ptr_array_remove_index(gv->selected, i);
            apply_filter(gv);
            return;
        }
    }
    g_ptr_array_add(gv->selected, g_strdup(genre));
    apply_filter(gv);
}

static void on_chip_clicked(GtkButton *btn, gpointer user_data) {
    GenresView *gv = user_data;
    toggle_genre(gv, g_object_get_data(G_OBJECT(btn), "genre"));
}

static void on_reset_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GenresView *gv = user_data;
    if (gv->selected->len == 0) return;
    g_ptr_array_set_size(gv->selected, 0);
    apply_filter(gv);
}

/* Кнопка-чип жанра. Активная кнопка окрашена акцентом. */
static GtkWidget *create_chip(GenresView *gv, const char *name, gboolean active) {
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "genre-chip");
    if (active) gtk_widget_add_css_class(btn, "genre-chip-on");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *lbl = gtk_label_new(name);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), lbl);

    gtk_button_set_child(GTK_BUTTON(btn), box);
    g_object_set_data_full(G_OBJECT(btn), "genre", g_strdup(name), g_free);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_chip_clicked), gv);
    return btn;
}

/* ================= карточки ================= */

static void on_album_card_clicked(GtkButton *btn, gpointer user_data) {
    GenresView *gv = user_data;
    const char *album = g_object_get_data(G_OBJECT(btn), "album");
    const char *artist = g_object_get_data(G_OBJECT(btn), "album-artist");
    genres_view_open_album(gv, album, artist);
}

static void on_artist_card_clicked(GtkButton *btn, gpointer user_data) {
    GenresView *gv = user_data;
    genres_view_open_artist(gv, g_object_get_data(G_OBJECT(btn), "artist"));
}

static GtkWidget *make_album_card(Section *s, gpointer item) {
    AlbumCard *a = item;

    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_set_valign(btn, GTK_ALIGN_START);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(box, GENRE_CARD_COVER_SIZE + 12, -1);
    gtk_box_append(GTK_BOX(box),
                   ui_create_fixed_size_bin(
                       ui_create_cover_widget(a->cover, GENRE_CARD_COVER_SIZE),
                       GENRE_CARD_COVER_SIZE));

    GtkWidget *title = gtk_label_new(a->album ? a->album : "");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title), (GENRE_CARD_COVER_SIZE + 12) / 8);
    gtk_box_append(GTK_BOX(box), title);

    if (a->artist && *a->artist) {
        GtkWidget *sub = gtk_label_new(a->artist);
        gtk_label_set_xalign(GTK_LABEL(sub), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(sub, "dim-label");
        gtk_widget_add_css_class(sub, "caption");
        gtk_box_append(GTK_BOX(box), sub);
    }

    gtk_button_set_child(GTK_BUTTON(btn), box);
    g_object_set_data_full(G_OBJECT(btn), "album", g_strdup(a->album ? a->album : ""), g_free);
    g_object_set_data_full(G_OBJECT(btn), "album-artist",
                           g_strdup(a->artist ? a->artist : ""), g_free);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_album_card_clicked), s->gv);
    return btn;
}

static GtkWidget *make_artist_card(Section *s, gpointer item) {
    ArtistCard *a = item;

    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_set_valign(btn, GTK_ALIGN_START);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(box, GENRE_CARD_COVER_SIZE + 12, -1);
    gtk_box_append(GTK_BOX(box),
                   ui_create_fixed_size_bin(
                       ui_create_round_cover_widget(a->cover, GENRE_CARD_COVER_SIZE),
                       GENRE_CARD_COVER_SIZE));

    GtkWidget *title = gtk_label_new(a->name ? a->name : "");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title), (GENRE_CARD_COVER_SIZE + 12) / 8);
    gtk_box_append(GTK_BOX(box), title);

    gtk_button_set_child(GTK_BUTTON(btn), box);
    g_object_set_data_full(G_OBJECT(btn), "artist", g_strdup(a->name ? a->name : ""), g_free);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_artist_card_clicked), s->gv);
    return btn;
}

/* ==== секции ==== */

/* Реальная нехватка места в свёрнутой полосе: карточек больше, чем рисуем
 * (s->overflow), либо содержимое шире области. */
static gboolean section_measured_overflow(Section *s) {
    if (s->overflow) return TRUE;
    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(s->row_scroll));
    if (!adj) return FALSE;
    double page = gtk_adjustment_get_page_size(adj);
    if (page <= 0) return FALSE;
    return gtk_adjustment_get_upper(adj) > page + 0.5;
}

static void section_refresh_button(Section *s) {
    gtk_widget_set_visible(s->showall_btn, section_measured_overflow(s));
}

/* Полоса секции прокручивается без видимой полосы прокрутки, поэтому о скрытых
 * карточках судим по превышению ширины содержимого над шириной области. */
static void on_row_adjustment_changed(GtkAdjustment *adj, gpointer user_data) {
    (void)adj;
    section_refresh_button(user_data);
}

static void section_populate(Section *s) {
    container_clear(s->row_box);

    guint total = s->cards ? s->cards->len : 0;
    gboolean truncated = total > COLLAPSED_CARD_CAP;
    s->overflow = truncated;

    guint n = truncated ? COLLAPSED_CARD_CAP : total;
    for (guint i = 0; i < n; i++) {
        gtk_box_append(GTK_BOX(s->row_box), s->make_card(s, g_ptr_array_index(s->cards, i)));
    }

    section_refresh_button(s);
}

/* ===== страница «Показать все» с полноразмерной сеткой ===== */

static void on_all_grid_setup(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory; (void)user_data;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);

    GtkWidget *cover_slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(cover_slot, GTK_ALIGN_CENTER);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(title, "heading");

    GtkWidget *subtitle = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(subtitle, "caption");
    gtk_widget_add_css_class(subtitle, "dim-label");

    gtk_box_append(GTK_BOX(box), cover_slot);
    gtk_box_append(GTK_BOX(box), title);
    gtk_box_append(GTK_BOX(box), subtitle);
    gtk_list_item_set_child(li, box);
}

static void on_all_grid_bind(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory; (void)user_data;
    GenreGridItem *item = DENCHIKU_GENRE_GRID_ITEM(gtk_list_item_get_item(li));
    GtkWidget *box = gtk_list_item_get_child(li);

    gtk_widget_set_size_request(box, GENRE_GRID_COVER_SIZE + 12, -1);

    GtkWidget *cover_slot = gtk_widget_get_first_child(box);
    GtkWidget *title = gtk_widget_get_next_sibling(cover_slot);
    GtkWidget *subtitle = gtk_widget_get_next_sibling(title);
    int max_chars = (GENRE_GRID_COVER_SIZE + 12) / 8;
    gtk_label_set_max_width_chars(GTK_LABEL(title), max_chars);
    gtk_label_set_max_width_chars(GTK_LABEL(subtitle), max_chars);

    GtkWidget *old = gtk_widget_get_first_child(cover_slot);
    if (old) { ui_detach_cover(old); gtk_box_remove(GTK_BOX(cover_slot), old); }
    GtkWidget *cover = item->is_artist
                           ? ui_create_round_cover_widget(item->cover, GENRE_GRID_COVER_SIZE)
                           : ui_create_cover_widget(item->cover, GENRE_GRID_COVER_SIZE);
    gtk_box_append(GTK_BOX(cover_slot), ui_create_fixed_size_bin(cover, GENRE_GRID_COVER_SIZE));

    gtk_label_set_text(GTK_LABEL(title), item->title ? item->title : "");
    gtk_label_set_text(GTK_LABEL(subtitle), item->subtitle ? item->subtitle : "");
    gtk_widget_set_visible(subtitle, item->subtitle && *item->subtitle);
}

static void on_all_grid_activate(GtkGridView *grid, guint position, gpointer user_data) {
    GenresView *gv = user_data;
    GObject *obj = g_list_model_get_item(G_LIST_MODEL(gtk_grid_view_get_model(grid)), position);
    if (!obj) return;

    GenreGridItem *item = DENCHIKU_GENRE_GRID_ITEM(obj);
    if (item->is_artist) {
        genres_view_open_artist(gv, item->title);
    } else {
        genres_view_open_album(gv, item->title, item->album_artist);
    }
    g_object_unref(obj);
}

/* Собирает полноразмерную сетку из текущей выборки и кладёт её в стек раздела. */
static void genres_view_push_all(GenresView *gv, SectionKind kind) {
    gboolean artists = (kind == SECTION_ARTISTS);
    GListStore *store = g_list_store_new(GENRE_GRID_ITEM_TYPE);

    if (artists) {
        for (guint i = 0; i < gv->artist_cards->len; i++) {
            ArtistCard *a = g_ptr_array_index(gv->artist_cards, i);
            GenreGridItem *it = g_object_new(GENRE_GRID_ITEM_TYPE, NULL);
            it->title = g_strdup(a->name);
            it->cover = g_strdup(a->cover);
            it->is_artist = TRUE;
            g_list_store_append(store, it);
            g_object_unref(it);
        }
    } else {
        for (guint i = 0; i < gv->album_cards->len; i++) {
            AlbumCard *a = g_ptr_array_index(gv->album_cards, i);
            GenreGridItem *it = g_object_new(GENRE_GRID_ITEM_TYPE, NULL);
            it->title = g_strdup(a->album);
            it->subtitle = g_strdup(a->artist);
            it->cover = g_strdup(a->cover);
            it->album_artist = g_strdup(a->artist);
            it->is_artist = FALSE;
            g_list_store_append(store, it);
            g_object_unref(it);
        }
    }

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(on_all_grid_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(on_all_grid_bind), NULL);

    /* gtk_no_selection_new() и gtk_grid_view_new() забирают владение моделью и
     * фабрикой, поэтому отдельно освобождать их не нужно. */
    GtkNoSelection *sel = gtk_no_selection_new(G_LIST_MODEL(store));
    GtkWidget *grid = gtk_grid_view_new(GTK_SELECTION_MODEL(sel), factory);
    gtk_grid_view_set_single_click_activate(GTK_GRID_VIEW(grid), TRUE);
    gtk_grid_view_set_min_columns(GTK_GRID_VIEW(grid), 2);
    gtk_grid_view_set_max_columns(GTK_GRID_VIEW(grid), 8);
    g_signal_connect(grid, "activate", G_CALLBACK(on_all_grid_activate), gv);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), grid);

    AdwToolbarView *tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    AdwHeaderBar *hb = ADW_HEADER_BAR(adw_header_bar_new());
    adw_header_bar_set_show_start_title_buttons(hb, FALSE);
    adw_toolbar_view_add_top_bar(tv, GTK_WIDGET(hb));
    adw_toolbar_view_set_content(tv, scroll);

    const char *title = artists ? "Все исполнители" : "Все альбомы";
    AdwNavigationPage *page = adw_navigation_page_new(GTK_WIDGET(tv), title);
    adw_navigation_view_push(gv->nav, page);
}

static void on_showall_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    Section *s = user_data;
    genres_view_push_all(s->gv, s->kind);
}

static void section_init(Section *s, GenresView *gv, SectionKind kind, const char *title,
                         SectionMakeCardFn make_card) {
    s->gv = gv;
    s->kind = kind;
    s->make_card = make_card;

    s->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *title_lbl = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(title_lbl), 0.0f);
    gtk_widget_add_css_class(title_lbl, "title-4");
    gtk_widget_set_hexpand(title_lbl, TRUE);
    gtk_box_append(GTK_BOX(header), title_lbl);

    /* Серая кнопка «Показать все» со стрелкой вправо. */
    s->showall_btn = gtk_button_new();
    gtk_widget_add_css_class(s->showall_btn, "flat");
    gtk_widget_set_valign(s->showall_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(s->showall_btn, FALSE);

    GtkWidget *showall_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *showall_lbl = gtk_label_new("Показать все");
    gtk_widget_add_css_class(showall_lbl, "dim-label");
    GtkWidget *showall_arrow = gtk_image_new_from_icon_name("go-next-symbolic");
    gtk_widget_add_css_class(showall_arrow, "dim-label");
    gtk_box_append(GTK_BOX(showall_box), showall_lbl);
    gtk_box_append(GTK_BOX(showall_box), showall_arrow);
    gtk_button_set_child(GTK_BUTTON(s->showall_btn), showall_box);

    g_signal_connect(s->showall_btn, "clicked", G_CALLBACK(on_showall_clicked), s);
    gtk_box_append(GTK_BOX(header), s->showall_btn);
    gtk_box_append(GTK_BOX(s->box), header);

    /* Полоса карточек: прокручивается без видимой полосы прокрутки.
     * GTK_POLICY_EXTERNAL не показывает полосу и не раздувает минимальную и
     * натуральную ширину страницы содержимым (иначе длинный ряд альбомов заставлял
     * бы окно расширяться). */
    s->row_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s->row_scroll),
                                   GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(s->row_scroll), TRUE);
    gtk_widget_set_vexpand(s->row_scroll, FALSE);

    s->row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s->row_scroll), s->row_box);
    g_signal_connect(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(s->row_scroll)),
                     "changed", G_CALLBACK(on_row_adjustment_changed), s);

    gtk_box_append(GTK_BOX(s->box), s->row_scroll);
}

/* ================= построение карточек ================= */

static void build_album_cards(GenresView *gv) {
    g_ptr_array_set_size(gv->album_cards, 0);

    GHashTable *by_key = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < gv->filtered->len; i++) {
        Track *t = g_ptr_array_index(gv->filtered, i);
        if (!t->album || !*t->album) continue;

        char *key = g_strdup_printf("%s\x1f%s",
                                    t->album, t->album_artist ? t->album_artist : "");
        if (g_hash_table_contains(by_key, key)) {
            g_free(key);
            continue;
        }
        g_hash_table_add(by_key, key);

        AlbumCard *a = g_new0(AlbumCard, 1);
        a->album = g_strdup(t->album);
        a->artist = g_strdup(t->album_artist);
        a->cover = g_strdup(t->cover_path);
        a->year = t->year;
        g_ptr_array_add(gv->album_cards, a);
    }
    g_hash_table_destroy(by_key);
    g_ptr_array_sort(gv->album_cards, cmp_album_card);
}

/* Добавляет трек в корзину исполнителя name (один раз в рамках трека). */
static void artist_index_tag(GHashTable *index, Track *t, const char *tag, GHashTable *seen) {
    char **names = track_split_artists(tag);
    if (!names) return;

    for (guint i = 0; names[i] != NULL; i++) {
        const char *name = names[i];
        if (!*name || g_hash_table_contains(seen, name)) continue;
        g_hash_table_add(seen, g_strdup(name));

        GPtrArray *bucket = g_hash_table_lookup(index, name);
        if (!bucket) {
            bucket = g_ptr_array_new();
            g_hash_table_insert(index, g_strdup(name), bucket);
        }
        g_ptr_array_add(bucket, t);
    }
    g_strfreev(names);
}

static const char *artist_best_cover(GPtrArray *bucket, const char *name) {
    const char *fallback = NULL;
    for (guint i = 0; i < bucket->len; i++) {
        Track *t = g_ptr_array_index(bucket, i);
        if (!t->cover_path || !*t->cover_path) continue;
        if (g_strcmp0(t->album_artist, name) == 0) return t->cover_path;
        if (!fallback) fallback = t->cover_path;
    }
    return fallback;
}

static void build_artist_cards(GenresView *gv) {
    g_ptr_array_set_size(gv->artist_cards, 0);

    GHashTable *index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, artist_bucket_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (guint i = 0; i < gv->filtered->len; i++) {
        Track *t = g_ptr_array_index(gv->filtered, i);
        g_hash_table_remove_all(seen);
        artist_index_tag(index, t, t->album_artist, seen);
        if (g_strcmp0(t->artist, t->album_artist) != 0) {
            artist_index_tag(index, t, t->artist, seen);
        }
    }
    g_hash_table_destroy(seen);

    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, index);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        const char *name = key;
        GPtrArray *bucket = value;
        ArtistCard *a = g_new0(ArtistCard, 1);
        a->name = g_strdup(name);
        a->cover = g_strdup(artist_best_cover(bucket, name));
        g_ptr_array_add(gv->artist_cards, a);
    }
    g_hash_table_destroy(index);
    g_ptr_array_sort(gv->artist_cards, cmp_artist_card);
}

/* Пересобирает чипы по текущей выборке: остаются только жанры, которые есть в
 * отфильтрованных треках, поэтому лишние кнопки исчезают. */
static void build_chips(GenresView *gv) {
    container_clear(gv->chips_flow);

    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);

    for (guint i = 0; i < gv->filtered->len; i++) {
        Track *t = g_ptr_array_index(gv->filtered, i);
        char **genres = track_split_genres(t->genre);
        if (!genres) continue;
        for (guint j = 0; genres[j] != NULL; j++) {
            if (g_hash_table_contains(seen, genres[j])) continue;
            g_hash_table_add(seen, g_strdup(genres[j]));
            g_ptr_array_add(names, g_strdup(genres[j]));
        }
        g_strfreev(genres);
    }
    g_hash_table_destroy(seen);

    g_ptr_array_sort(names, cmp_str);

    /* Выбранные жанры идут первыми — в порядке выбора, остальные за ними по
     * алфавиту. Так активные чипы не «уезжают» вглубь списка. */
    GPtrArray *ordered = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < gv->selected->len; i++) {
        g_ptr_array_add(ordered, g_strdup(g_ptr_array_index(gv->selected, i)));
    }
    for (guint i = 0; i < names->len; i++) {
        const char *name = g_ptr_array_index(names, i);
        if (is_selected(gv, name)) continue;
        g_ptr_array_add(ordered, g_strdup(name));
    }

    for (guint i = 0; i < ordered->len; i++) {
        const char *name = g_ptr_array_index(ordered, i);
        adw_wrap_box_append(ADW_WRAP_BOX(gv->chips_flow),
                            create_chip(gv, name, is_selected(gv, name)));
    }
    g_ptr_array_unref(ordered);
    g_ptr_array_unref(names);
}

/* ================= строки списка треков ================= */

static char *format_duration(guint seconds) {
    if (seconds == 0) return g_strdup("—");
    guint h = seconds / 3600;
    guint m = (seconds % 3600) / 60;
    guint s = seconds % 60;
    if (h > 0) return g_strdup_printf("%u:%02u:%02u", h, m, s);
    return g_strdup_printf("%u:%02u", m, s);
}

static void row_set_playing(GtkWidget *box, gboolean playing) {
    GtkWidget *title = g_object_get_data(G_OBJECT(box), "title");
    if (!title) return;
    if (playing) {
        gtk_widget_add_css_class(title, "genre-track-playing");
    } else {
        gtk_widget_remove_css_class(title, "genre-track-playing");
    }
}

static void on_item_playing_changed(GObject *obj, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    row_set_playing(user_data, DENCHIKU_GENRE_TRACK_ITEM(obj)->playing);
}

static void row_playing_unbind(GtkWidget *box) {
    GObject *item = g_object_get_data(G_OBJECT(box), "playing-item");
    gulong id = (gulong)GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(box), "playing-handler"));
    if (item && id) g_signal_handler_disconnect(item, id);
    g_object_set_data(G_OBJECT(box), "playing-item", NULL);
    g_object_set_data(G_OBJECT(box), "playing-handler", NULL);
}

static void row_playing_bind(GtkWidget *box, GenreTrackItem *item) {
    row_playing_unbind(box);
    gulong id = g_signal_connect(G_OBJECT(item), "notify::playing",
                                 G_CALLBACK(on_item_playing_changed), box);
    g_object_set_data(G_OBJECT(box), "playing-item", item);
    g_object_set_data(G_OBJECT(box), "playing-handler", GUINT_TO_POINTER((guint)id));
    row_set_playing(box, item->playing);
}

static void set_row_cover(GtkWidget *box, const char *cover_path) {
    GtkWidget *slot = g_object_get_data(G_OBJECT(box), "cover-slot");
    if (!slot) return;
    GtkWidget *old = gtk_widget_get_first_child(slot);
    if (old) { ui_detach_cover(old); gtk_box_remove(GTK_BOX(slot), old); }
    gtk_box_append(GTK_BOX(slot),
                   ui_create_fixed_size_bin(
                       ui_create_cover_widget(cover_path, UI_ROW_COVER_SIZE),
                       UI_ROW_COVER_SIZE));
}

/* Строит содержимое строки трека внутри контейнера box и запоминает его части. */
static void build_track_row(GtkWidget *box, GenresView *gv) {
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(content, 6);
    gtk_widget_set_margin_bottom(content, 6);
    gtk_widget_set_margin_start(content, 12);
    gtk_widget_set_margin_end(content, 12);

    GtkWidget *cover = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(cover, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(content), cover);

    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(text, TRUE);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(text), title);

    GtkWidget *artist = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(artist), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(artist), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(artist, "dim-label");
    gtk_box_append(GTK_BOX(text), artist);
    gtk_box_append(GTK_BOX(content), text);

    GtkWidget *dur = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(dur), 1.0f);
    gtk_widget_set_valign(dur, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(dur, "dim-label");
    gtk_box_append(GTK_BOX(content), dur);

    GtkWidget *suffix = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_valign(suffix, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(content), suffix);

    gtk_box_append(GTK_BOX(box), content);

    g_object_set_data(G_OBJECT(box), "content", content);
    g_object_set_data(G_OBJECT(box), "cover-slot", cover);
    g_object_set_data(G_OBJECT(box), "title", title);
    g_object_set_data(G_OBJECT(box), "artist", artist);
    g_object_set_data(G_OBJECT(box), "duration", dur);

    /* Меню и сердечко вешаются один раз на строку; текущий трек подставляется при
     * каждой привязке через track_menu_row_set_track(). */
    track_menu_attach_row(content, suffix, TRACK_CONTEXT_GENRE, gv->player);
}

static void populate_track_row(GtkWidget *box, GenreTrackItem *item) {
    Track *t = item->track;
    if (!t) return;

    GtkWidget *content = g_object_get_data(G_OBJECT(box), "content");
    set_row_cover(box, t->cover_path);
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(box), "title")),
                       t->title ? t->title : "Без названия");
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(box), "artist")),
                       (t->artist && *t->artist) ? t->artist
                                                 : (t->album ? t->album : ""));
    char *dur = format_duration(t->duration);
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(box), "duration")), dur);
    g_free(dur);
    track_menu_row_set_track(content, t);
    row_playing_bind(box, item);
}

/* ================= фабрика списка ================= */

static void attach_header_widget(GtkWidget *box, GenresView *gv) {
    /* У виджета может уже быть родитель (шапка осталась в ранее привязанном
     * элементе списка) — открепляем, чтобы не осталось двух родителей. */
    GtkWidget *parent = gtk_widget_get_parent(gv->header_widget);
    if (parent == box) return;
    if (parent) gtk_box_remove(GTK_BOX(parent), gv->header_widget);
    gtk_box_append(GTK_BOX(box), gv->header_widget);
}

static void on_list_setup(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory; (void)user_data;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_list_item_set_child(li, box);
    g_object_set_data(G_OBJECT(box), "kind", GINT_TO_POINTER(0));
}

static void on_list_bind(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory;
    GenresView *gv = user_data;
    GObject *item = gtk_list_item_get_item(li);
    GtkWidget *box = gtk_list_item_get_child(li);

    gboolean is_header = G_TYPE_CHECK_INSTANCE_TYPE(item, GENRE_HEADER_ITEM_TYPE);
    int kind = is_header ? 1 : 2;

    /* У строки-шапки убираем интерактивность: иначе Adwaita подсвечивает её как
     * кнопку при наведении и нажатии (стиль row.activatable). */
    gtk_list_item_set_activatable(li, !is_header);

    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(box), "kind")) != kind) {
        container_clear(box);
        if (is_header) {
            attach_header_widget(box, gv);
        } else {
            build_track_row(box, gv);
        }
        g_object_set_data(G_OBJECT(box), "kind", GINT_TO_POINTER(kind));
    }

    if (!is_header) populate_track_row(box, DENCHIKU_GENRE_TRACK_ITEM(item));
}

static void on_list_unbind(GtkListItemFactory *factory, GtkListItem *li, gpointer user_data) {
    (void)factory; (void)user_data;
    GtkWidget *box = gtk_list_item_get_child(li);
    if (!box) return;
    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(box), "kind")) == 2) {
        row_playing_unbind(box);
        GtkWidget *content = g_object_get_data(G_OBJECT(box), "content");
        if (content) track_menu_row_set_track(content, NULL);
        set_row_cover(box, NULL);
    }
}

static void on_list_activate(GtkListView *view, guint position, gpointer user_data) {
    (void)view;
    GenresView *gv = user_data;
    if (position == 0) return; /* шапка */
    guint index = position - 1;
    if (index >= gv->filtered->len) return;
    player_set_queue_and_play(gv->player, gv->filtered, (int)index);
}

/* Пересобирает модель: шапка всегда первая, треки — только при активном фильтре. */
static void rebuild_list_model(GenresView *gv, gboolean active, const char *playing_uri) {
    g_list_store_remove_all(gv->store);

    GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(items, g_object_new(GENRE_HEADER_ITEM_TYPE, NULL));

    if (active) {
        for (guint i = 0; i < gv->filtered->len; i++) {
            Track *t = g_ptr_array_index(gv->filtered, i);
            GenreTrackItem *it = genre_track_item_new(t);
            it->playing = playing_uri && t->uri && g_strcmp0(t->uri, playing_uri) == 0;
            g_ptr_array_add(items, it);
        }
    }

    g_list_store_splice(gv->store, 0, 0, items->pdata, items->len);
    g_ptr_array_unref(items);
}

static void update_playing(GenresView *gv, const char *playing_uri) {
    guint n = g_list_model_get_n_items(G_LIST_MODEL(gv->store));
    for (guint i = 1; i < n; i++) {
        GObject *obj = g_list_model_get_item(G_LIST_MODEL(gv->store), i);
        if (!obj) continue;
        if (DENCHIKU_IS_GENRE_TRACK_ITEM(obj)) {
            GenreTrackItem *it = DENCHIKU_GENRE_TRACK_ITEM(obj);
            gboolean want = playing_uri && it->track && it->track->uri &&
                            g_strcmp0(it->track->uri, playing_uri) == 0;
            if (it->playing != want) g_object_set(obj, "playing", want, NULL);
        }
        g_object_unref(obj);
    }
}

/* ================= применение фильтра ================= */

static void apply_filter(GenresView *gv) {
    gboolean active = gv->selected->len > 0;

    /* Без выбранного жанра выборка — вся медиатека: она нужна только для полного
     * списка чипов, а тяжёлые секции и список треков не строим, пока фильтр не
     * задан. */
    g_ptr_array_set_size(gv->filtered, 0);
    if (gv->tracks) {
        for (guint i = 0; i < gv->tracks->len; i++) {
            Track *t = g_ptr_array_index(gv->tracks, i);
            if (!active || track_matches(gv, t)) g_ptr_array_add(gv->filtered, t);
        }
    }

    build_chips(gv);

    if (active) {
        build_album_cards(gv);
        build_artist_cards(gv);
    } else {
        g_ptr_array_set_size(gv->album_cards, 0);
        g_ptr_array_set_size(gv->artist_cards, 0);
    }

    gv->albums.cards = gv->album_cards;
    gv->artists.cards = gv->artist_cards;
    section_populate(&gv->albums);
    section_populate(&gv->artists);

    Track *cur = player_get_current_track(gv->player);
    const char *playing_uri = cur ? cur->uri : NULL;
    rebuild_list_model(gv, active, playing_uri);

    char *count = g_strdup_printf("Треки · %u", active ? gv->filtered->len : 0);
    gtk_label_set_text(GTK_LABEL(gv->tracks_count_lbl), count);
    g_free(count);

    /* Результаты показываем только при выбранном жанре: до первого клика страница
     * остаётся подсказкой и списком жанров, а не дампом всей медиатеки. */
    gboolean show_tracks = active && gv->filtered->len > 0;

    gtk_widget_set_visible(gv->reset_btn, active);
    gtk_widget_set_visible(gv->placeholder, !active);
    gtk_widget_set_visible(gv->albums.box, active && gv->album_cards->len > 0);
    gtk_widget_set_visible(gv->artists.box, active && gv->artist_cards->len > 0);
    gtk_widget_set_visible(gv->tracks_header, show_tracks);
}

/* ================= навигация ================= */

/* Альбом кладётся в стек этого раздела. Общий помощник для меню трека и карточек. */
static void push_album_page(GenresView *gv, const char *album, const char *album_artist) {
    GtkListBox *sublist = NULL;
    AdwNavigationPage *page = album_detail_page_create(gv->player, gv->tracks,
                                                       album, album_artist, &sublist);
    if (page) adw_navigation_view_push(gv->nav, page);
}

/* Обёртка под ArtistOpenAlbumFn: альбом со страницы исполнителя открывается в
 * этом же стеке, страница исполнителя остаётся в истории. */
static void on_open_album_for_artist(const char *album, const char *album_artist,
                                     gpointer user_data) {
    push_album_page(user_data, album, album_artist);
}

void genres_view_open_album(GenresView *gv, const char *album, const char *album_artist) {
    if (!gv->tracks || !album || !*album) return;
    push_album_page(gv, album, album_artist);
}

void genres_view_open_artist(GenresView *gv, const char *artist) {
    if (!gv || !gv->tracks || !artist || !*artist) return;
    artist_detail_page_open(gv->nav, gv->player, gv->tracks, artist,
                            on_open_album_for_artist, gv);
}

/* ================= публичный интерфейс ================= */

GenresView *genres_view_new(Player *player, AdwOverlaySplitView *split_view) {
    ensure_genre_css();

    GenresView *gv = g_new0(GenresView, 1);
    gv->player = player;
    gv->selected = g_ptr_array_new_with_free_func(g_free);
    gv->album_cards = g_ptr_array_new_with_free_func(album_card_free);
    gv->artist_cards = g_ptr_array_new_with_free_func(artist_card_free);
    gv->filtered = g_ptr_array_new();

    /* Жанровые чипы и кнопка сброса. AdwWrapBox (в отличие от GtkFlowBox) не
     * выстраивает детей в сетку: чипы переносятся как слова в тексте. */
    gv->chips_flow = adw_wrap_box_new();
    gtk_orientable_set_orientation(GTK_ORIENTABLE(gv->chips_flow), GTK_ORIENTATION_HORIZONTAL);
    adw_wrap_box_set_child_spacing(ADW_WRAP_BOX(gv->chips_flow), 6);
    adw_wrap_box_set_line_spacing(ADW_WRAP_BOX(gv->chips_flow), 6);
    gtk_widget_set_hexpand(gv->chips_flow, TRUE);

    /* Кнопка сброса — иконка крестика в начале списка жанров. */
    gv->reset_btn = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(gv->reset_btn, "flat");
    gtk_widget_set_valign(gv->reset_btn, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text(gv->reset_btn, "Сбросить фильтр");
    gtk_widget_set_visible(gv->reset_btn, FALSE);
    g_signal_connect(gv->reset_btn, "clicked", G_CALLBACK(on_reset_clicked), gv);

    GtkWidget *chips_area = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(chips_area), gv->reset_btn);
    gtk_box_append(GTK_BOX(chips_area), gv->chips_flow);

    gv->placeholder = gtk_label_new(
        "Выберите жанр, чтобы отфильтровать треки, альбомы и исполнителей.");
    gtk_label_set_xalign(GTK_LABEL(gv->placeholder), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(gv->placeholder), TRUE);
    gtk_widget_add_css_class(gv->placeholder, "dim-label");

    section_init(&gv->albums, gv, SECTION_ALBUMS, "Альбомы", make_album_card);
    section_init(&gv->artists, gv, SECTION_ARTISTS, "Исполнители", make_artist_card);
    gtk_widget_set_visible(gv->albums.box, FALSE);
    gtk_widget_set_visible(gv->artists.box, FALSE);

    gv->tracks_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *tracks_title = gtk_label_new("Треки");
    gtk_label_set_xalign(GTK_LABEL(tracks_title), 0.0f);
    gtk_widget_add_css_class(tracks_title, "title-4");
    gtk_widget_set_hexpand(tracks_title, TRUE);
    gtk_box_append(GTK_BOX(gv->tracks_header), tracks_title);
    gv->tracks_count_lbl = gtk_label_new("");
    gtk_widget_add_css_class(gv->tracks_count_lbl, "dim-label");
    gtk_box_append(GTK_BOX(gv->tracks_header), gv->tracks_count_lbl);
    gtk_widget_set_visible(gv->tracks_header, FALSE);

    /* Шапка — первый элемент списка: держим на неё ссылку вне списка, чтобы она
     * не разрушалась при перепривязке элемента. */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_top(header, 16);
    gtk_widget_set_margin_bottom(header, 16);
    gtk_widget_set_margin_start(header, 16);
    gtk_widget_set_margin_end(header, 16);
    gtk_box_append(GTK_BOX(header), chips_area);
    gtk_box_append(GTK_BOX(header), gv->placeholder);
    gtk_box_append(GTK_BOX(header), gv->albums.box);
    gtk_box_append(GTK_BOX(header), gv->artists.box);
    gtk_box_append(GTK_BOX(header), gv->tracks_header);
    gv->header_widget = g_object_ref_sink(header);

    /* Виртуализированный список: шапка + треки. */
    gv->store = g_list_store_new(G_TYPE_OBJECT);
    GtkNoSelection *sel = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(gv->store)));

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(on_list_setup), gv);
    g_signal_connect(factory, "bind", G_CALLBACK(on_list_bind), gv);
    g_signal_connect(factory, "unbind", G_CALLBACK(on_list_unbind), gv);

    /* gtk_list_view_new забирает владение моделью и фабрикой. */
    GtkWidget *list_view = gtk_list_view_new(GTK_SELECTION_MODEL(sel), factory);
    gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(list_view), TRUE);
    g_signal_connect(list_view, "activate", G_CALLBACK(on_list_activate), gv);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list_view);

    GtkWidget *hb = ui_create_content_header(split_view);

    AdwToolbarView *tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    gtk_widget_add_css_class(GTK_WIDGET(tv), "view");
    adw_toolbar_view_set_top_bar_style(tv, ADW_TOOLBAR_FLAT);
    adw_toolbar_view_add_top_bar(tv, hb);
    adw_toolbar_view_set_content(tv, scrolled);

    AdwNavigationPage *root = adw_navigation_page_new(GTK_WIDGET(tv), "Жанры");
    gv->root = root;
    gv->nav = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
    adw_navigation_view_add(gv->nav, root);

    return gv;
}

GtkWidget *genres_view_get_widget(GenresView *gv) {
    return GTK_WIDGET(gv->nav);
}

/* Убирает из выбора жанры, которых больше нет в медиатеке (после переразбиения
 * тегов или пересканирования). */
static void prune_selected(GenresView *gv) {
    if (gv->selected->len == 0 || !gv->tracks) return;

    GHashTable *avail = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < gv->tracks->len; i++) {
        Track *t = g_ptr_array_index(gv->tracks, i);
        char **genres = track_split_genres(t->genre);
        if (!genres) continue;
        for (guint j = 0; genres[j] != NULL; j++) {
            if (!g_hash_table_contains(avail, genres[j])) {
                g_hash_table_add(avail, g_strdup(genres[j]));
            }
        }
        g_strfreev(genres);
    }

    for (guint i = gv->selected->len; i > 0; i--) {
        const char *genre = g_ptr_array_index(gv->selected, i - 1);
        if (!g_hash_table_contains(avail, genre)) {
            g_ptr_array_remove_index(gv->selected, i - 1);
        }
    }
    g_hash_table_destroy(avail);
}

void genres_view_set_tracks(GenresView *gv, GPtrArray *tracks) {
    /* Сначала удерживаем новую медиатеку, потом отпускаем старую: массивы могут
     * совпадать (переразбиение тегов), и тогда порядок не важен, но при замене
     * треки не освобождаются раньше, чем список от них отвяжется. */
    GPtrArray *old = gv->tracks;
    gv->tracks = tracks ? g_ptr_array_ref(tracks) : NULL;

    prune_selected(gv);
    apply_filter(gv);

    if (old) g_ptr_array_unref(old);
}

void genres_view_select_genre(GenresView *gv, const char *genre) {
    if (!gv || !genre || !*genre) return;

    /* Заменяем фильтр этим жанром и возвращаемся на корневую страницу раздела,
     * чтобы результат был виден сразу (сюда приходят по ссылке из альбома). */
    g_ptr_array_set_size(gv->selected, 0);
    g_ptr_array_add(gv->selected, g_strdup(genre));
    apply_filter(gv);
    adw_navigation_view_pop_to_page(gv->nav, gv->root);
}

void genres_view_refresh_playing(GenresView *gv, const char *playing_uri) {
    update_playing(gv, playing_uri);

    /* Вложенные страницы (альбом, исполнитель) держат свои списки строк. */
    AdwNavigationPage *top = adw_navigation_view_get_visible_page(gv->nav);
    if (top && top != gv->root) {
        ui_highlight_playing_on_page(GTK_WIDGET(top), playing_uri);
    }
}

void genres_view_free(GenresView *gv) {
    if (!gv) return;
    if (gv->tracks) g_ptr_array_unref(gv->tracks);
    if (gv->selected) g_ptr_array_unref(gv->selected);
    if (gv->album_cards) g_ptr_array_unref(gv->album_cards);
    if (gv->artist_cards) g_ptr_array_unref(gv->artist_cards);
    if (gv->filtered) g_ptr_array_unref(gv->filtered);
    if (gv->header_widget) g_object_unref(gv->header_widget);
    if (gv->store) g_object_unref(gv->store);
    g_free(gv);
}
