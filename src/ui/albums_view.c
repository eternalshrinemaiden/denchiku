#include "albums_view.h"
#include "album_detail_page.h"
#include "artist_detail_page.h"
#include "favorites.h"
#include "media_browser.h"
#include "track.h"
#include "track_menu.h"
#include "ui_helpers.h"
#include "view_controls.h"

#define TYPE_ALBUM_ITEM (album_item_get_type())
G_DECLARE_FINAL_TYPE(AlbumItem, album_item, MC, ALBUM_ITEM, GObject)

struct _AlbumItem {
    GObject parent_instance;
    char *album;
    char *artist;
    guint year;
    char *cover_path;
};

G_DEFINE_FINAL_TYPE(AlbumItem, album_item, G_TYPE_OBJECT)

static void album_item_finalize(GObject *obj) {
    AlbumItem *self = MC_ALBUM_ITEM(obj);
    g_free(self->album);
    g_free(self->artist);
    g_free(self->cover_path);
    G_OBJECT_CLASS(album_item_parent_class)->finalize(obj);
}

static void album_item_class_init(AlbumItemClass *klass) {
    G_OBJECT_CLASS(klass)->finalize = album_item_finalize;
}

static void album_item_init(AlbumItem *self) {
    (void)self;
}

static AlbumItem *album_item_new(const char *album, const char *artist,
                                 guint year, const char *cover_path) {
    AlbumItem *item = g_object_new(TYPE_ALBUM_ITEM, NULL);
    item->album = g_strdup(album);
    item->artist = g_strdup(artist);
    item->year = year;
    item->cover_path = g_strdup(cover_path);
    return item;
}

static const char *get_item_album(GObject *obj)  { return MC_ALBUM_ITEM(obj)->album; }
static const char *get_item_artist(GObject *obj) { return MC_ALBUM_ITEM(obj)->artist; }
static const char *get_item_cover(GObject *obj)  { return MC_ALBUM_ITEM(obj)->cover_path; }
static guint       get_item_year(GObject *obj)   { return MC_ALBUM_ITEM(obj)->year; }

/* Кнопка-«сердце» на карточке альбома: пометка любимого альбома (см. favorites). */
static GtkWidget *make_album_heart(GObject *obj) {
    AlbumItem *al = MC_ALBUM_ITEM(obj);
    return favorite_album_button_new(al->album, al->artist);
}

struct _AlbumsView {
    Player *player;
    GPtrArray *tracks;
    gboolean liked_only; /* раздел «Любимые альбомы» — только помеченные */

    AdwNavigationView *nav;
    GListStore *store;
    ViewControls *controls;
    MediaBrowser *browser;

    GtkListBox *active_sublist;
    AdwNavigationPage *root; /* корневой список альбомов */
};

static void on_subpage_popped(AdwNavigationView *nav, AdwNavigationPage *page, gpointer user_data) {
    (void)nav; (void)page;
    ((AlbumsView *)user_data)->active_sublist = NULL;
}

/* Страница альбома кладётся в стек этого раздела. Общий помощник для открытия
 * из карточки, меню трека и карточек страницы исполнителя. */
static void push_album_page(AlbumsView *av, const char *album, const char *album_artist) {
    AdwNavigationPage *page = album_detail_page_create(
        av->player, av->tracks, album, album_artist, &av->active_sublist);

    if (page) {
        adw_navigation_view_push(av->nav, page);
    }
}

/* Обёртка под ArtistOpenAlbumFn: карточка альбома на странице исполнителя
 * открывается в этом же разделе, страница исполнителя остаётся в истории. */
static void on_open_album_for_artist(const char *album, const char *album_artist,
                                     gpointer user_data) {
    push_album_page(user_data, album, album_artist);
}

static void on_album_activated(GObject *item, gpointer user_data) {
    AlbumsView *av = user_data;
    AlbumItem *album = MC_ALBUM_ITEM(item);
    push_album_page(av, album->album, album->artist);
}

static void on_controls_changed(ViewControls *vc, gpointer user_data) {
    (void)vc;
    AlbumsView *av = user_data;
    media_browser_refresh(av->browser);
}

/* Перестраивает модель из текущего списка треков. В режиме «Любимые» в модель
 * попадают только помеченные альбомы, поэтому раздел обновляется и по событию
 * смены пометок (см. on_favorites_changed). */
static void rebuild_store(AlbumsView *av) {
    g_list_store_remove_all(av->store);
    if (!av->tracks) return;

    /* Альбом идентифицируется парой (название, album_artist): одноимённые
     * альбомы разных исполнителей не должны сливаться в одну запись. */
    GHashTable *by_key = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    /* Собираем новые записи и добавляем в модель одним splice(): иначе каждый
     * append дёргает GtkSortListModel и перепривязку вьюх (с загрузкой обложек)
     * заново, что даёт квадратичную сложность на больших медиатеках. */
    GPtrArray *new_items = g_ptr_array_new_with_free_func(g_object_unref);

    for (guint i = 0; i < av->tracks->len; i++) {
        Track *t = g_ptr_array_index(av->tracks, i);
        if (av->liked_only && !favorites_album_is_liked(t->album, t->album_artist)) {
            continue;
        }

        char *key = g_strdup_printf("%s\x1f%s",
                                    t->album ? t->album : "",
                                    t->album_artist ? t->album_artist : "");

        AlbumItem *item = g_hash_table_lookup(by_key, key);
        if (!item) {
            item = album_item_new(t->album, t->album_artist, t->year, t->cover_path);
            g_hash_table_insert(by_key, key, item);
            g_ptr_array_add(new_items, item);
        } else {
            /* Дозаполняем недостающие метаданные из других треков альбома. */
            if ((!item->cover_path || !*item->cover_path) && t->cover_path && *t->cover_path) {
                g_free(item->cover_path);
                item->cover_path = g_strdup(t->cover_path);
            }
            if (item->year == 0 && t->year > 0) {
                item->year = t->year;
            }
            g_free(key);
        }
    }

    if (new_items->len > 0) {
        g_list_store_splice(av->store, 0, 0, new_items->pdata, new_items->len);
    }
    g_ptr_array_unref(new_items);
    g_hash_table_destroy(by_key);
}

static void on_favorites_changed(gpointer user_data) {
    AlbumsView *av = user_data;
    if (av->liked_only) rebuild_store(av);
}

AlbumsView *albums_view_new(Player *player, AdwOverlaySplitView *split_view, gboolean liked_only) {
    AlbumsView *av = g_new0(AlbumsView, 1);
    av->player = player;
    av->liked_only = liked_only;
    av->store = g_list_store_new(TYPE_ALBUM_ITEM);
    av->controls = view_controls_new(TRUE, on_controls_changed, av);

    MediaBrowserConfig cfg = {
        .title_col_name    = "Альбом",
        .subtitle_col_name = "Исполнитель",
        .year_col_name     = "Год",
        .get_title         = get_item_album,
        .get_subtitle      = get_item_artist,
        .get_cover_path    = get_item_cover,
        .get_year          = get_item_year,
        .make_heart        = make_album_heart
    };

    av->browser = media_browser_new(av->store, av->controls, &cfg, on_album_activated, av);

    GtkWidget *hb = ui_create_content_header(split_view);
    adw_header_bar_pack_end(ADW_HEADER_BAR(hb), view_controls_get_widget(av->controls));

    AdwToolbarView *tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    gtk_widget_add_css_class(GTK_WIDGET(tv), "view");
    adw_toolbar_view_set_top_bar_style(tv, ADW_TOOLBAR_FLAT);
    adw_toolbar_view_add_top_bar(tv, hb);
    adw_toolbar_view_set_content(tv, media_browser_get_widget(av->browser));

    AdwNavigationPage *root = adw_navigation_page_new(
        GTK_WIDGET(tv), liked_only ? "Любимые альбомы" : "Альбомы");
    av->nav = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
    av->root = root;
    adw_navigation_view_add(av->nav, root);
    g_signal_connect(av->nav, "popped", G_CALLBACK(on_subpage_popped), av);

    /* В разделе «Любимые» лайк с любой карточки должен перестроить список. */
    if (liked_only) favorites_add_listener(on_favorites_changed, av);

    return av;
}

GtkWidget *albums_view_get_widget(AlbumsView *av) {
    return GTK_WIDGET(av->nav);
}

ViewControls *albums_view_get_controls(AlbumsView *av) {
    return av->controls;
}

void albums_view_set_tracks(AlbumsView *av, GPtrArray *tracks) {
    if (av->tracks) g_ptr_array_unref(av->tracks);
    av->tracks = tracks ? g_ptr_array_ref(tracks) : NULL;
    rebuild_store(av);
}

void albums_view_refresh_playing(AlbumsView *av, const char *playing_uri) {
    ui_highlight_playing_in_list(av->active_sublist, playing_uri);
}

void albums_view_open_album(AlbumsView *av, const char *album, const char *album_artist) {
    if (!av || !av->tracks || !album) return;

    /* Возвращаемся к списку, чтобы меню не плодило вложенные страницы. */
    if (av->root) {
        adw_navigation_view_pop_to_page(av->nav, av->root);
    }
    push_album_page(av, album, album_artist);
}

void albums_view_open_artist(AlbumsView *av, const char *artist) {
    if (!av || !av->tracks) return;
    artist_detail_page_open(av->nav, av->player, av->tracks, artist,
                            on_open_album_for_artist, av);
}

void albums_view_free(AlbumsView *av) {
    if (!av) return;
    favorites_remove_listener(on_favorites_changed, av);
    if (av->tracks) g_ptr_array_unref(av->tracks);
    media_browser_free(av->browser);
    view_controls_free(av->controls);
    if (av->store) g_object_unref(av->store);
    g_free(av);
}
