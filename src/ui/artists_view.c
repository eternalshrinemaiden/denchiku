#include "artists_view.h"
#include "album_detail_page.h"
#include "artist_detail_page.h"
#include "favorites.h"
#include "media_browser.h"
#include "track.h"
#include "track_menu.h"
#include "ui_helpers.h"
#include "view_controls.h"

#define TYPE_ARTIST_ITEM (artist_item_get_type())
G_DECLARE_FINAL_TYPE(ArtistItem, artist_item, MC, ARTIST_ITEM, GObject)

struct _ArtistItem {
    GObject parent_instance;
    char *name;
    char *cover_path;
};

G_DEFINE_FINAL_TYPE(ArtistItem, artist_item, G_TYPE_OBJECT)

static void artist_item_finalize(GObject *obj) {
    ArtistItem *self = MC_ARTIST_ITEM(obj);
    g_free(self->name);
    g_free(self->cover_path);
    G_OBJECT_CLASS(artist_item_parent_class)->finalize(obj);
}

static void artist_item_class_init(ArtistItemClass *klass) {
    G_OBJECT_CLASS(klass)->finalize = artist_item_finalize;
}

static void artist_item_init(ArtistItem *self) {
    (void)self;
}

static ArtistItem *artist_item_new(const char *name, const char *cover_path) {
    ArtistItem *item = g_object_new(TYPE_ARTIST_ITEM, NULL);
    item->name = g_strdup(name);
    item->cover_path = g_strdup(cover_path);
    return item;
}

static const char *get_item_name(GObject *obj)  { return MC_ARTIST_ITEM(obj)->name; }
static const char *get_item_cover(GObject *obj) { return MC_ARTIST_ITEM(obj)->cover_path; }

/* Кнопка-«сердце» на карточке исполнителя: пометка любимого исполнителя. */
static GtkWidget *make_artist_heart(GObject *obj) {
    return favorite_artist_button_new(MC_ARTIST_ITEM(obj)->name);
}

struct _ArtistsView {
    Player *player;
    GPtrArray *tracks;
    gboolean liked_only; /* раздел «Любимые артисты» — только помеченные */

    AdwNavigationView *nav;
    AdwNavigationPage *root;
    GListStore *store;
    ViewControls *controls;
    MediaBrowser *browser;

    GHashTable *index; /* имя -> GPtrArray<Track*> (заимствованы) */
};

static void artist_bucket_free(gpointer data) {
    g_ptr_array_unref(data);
}

/* Альбом (со страницы исполнителя или со строки трека) открывается в этом же
 * стеке навигации: раздел не переключается, а страница исполнителя остаётся в
 * истории. */
void artists_view_open_album(ArtistsView *arv, const char *album, const char *album_artist) {
    if (!arv->tracks || !album || !*album) return;

    GtkListBox *sublist = NULL;
    AdwNavigationPage *page = album_detail_page_create(arv->player, arv->tracks,
                                                       album, album_artist, &sublist);
    if (page) adw_navigation_view_push(arv->nav, page);
}

/* Обёртка под ArtistOpenAlbumFn: альбом открывается в стеке этого раздела. */
static void on_open_album_for_artist(const char *album, const char *album_artist,
                                     gpointer user_data) {
    artists_view_open_album(user_data, album, album_artist);
}

static void on_artist_activated(GObject *item, gpointer user_data) {
    ArtistsView *arv = user_data;
    const char *artist = get_item_name(item);
    if (!artist || !*artist) return;
    artists_view_open_artist(arv, artist);
}

/* Открывает страницу исполнителя по имени. Используется и активацией карточки в
 * списке, и пунктом меню трека «Перейти к исполнителю». */
void artists_view_open_artist(ArtistsView *arv, const char *artist) {
    if (!arv->tracks || !artist || !*artist) return;

    GPtrArray *bucket = arv->index ? g_hash_table_lookup(arv->index, artist) : NULL;
    if (!bucket || bucket->len == 0) return;

    AdwNavigationPage *page = artist_detail_page_create(arv->player, bucket, arv->tracks,
                                                        artist, on_open_album_for_artist, arv);
    if (page) adw_navigation_view_push(arv->nav, page);
}

static void on_controls_changed(ViewControls *vc, gpointer user_data) {
    (void)vc;
    ArtistsView *arv = user_data;
    media_browser_refresh(arv->browser);
}

/* Добавляет трек в корзину исполнителя name (если ещё не добавлен в рамках
 * обработки этого трека — см. seen). */
static void index_tag(ArtistsView *arv, Track *t, const char *tag, GHashTable *seen) {
    char **names = track_split_artists(tag);
    if (!names) return;

    for (guint i = 0; names[i] != NULL; i++) {
        const char *name = names[i];
        if (!*name || g_hash_table_contains(seen, name)) continue;
        g_hash_table_add(seen, g_strdup(name));

        GPtrArray *bucket = g_hash_table_lookup(arv->index, name);
        if (!bucket) {
            bucket = g_ptr_array_new();
            g_hash_table_insert(arv->index, g_strdup(name), bucket);
        }
        g_ptr_array_add(bucket, t);
    }
    g_strfreev(names);
}

/* Обложка исполнителя: предпочитаем альбом, где он указан как исполнитель
 * альбома, иначе берём любую обложку из его треков. */
static const char *best_cover(GPtrArray *bucket, const char *name) {
    const char *fallback = NULL;
    for (guint i = 0; i < bucket->len; i++) {
        Track *t = g_ptr_array_index(bucket, i);
        if (!t->cover_path || !*t->cover_path) continue;
        if (g_strcmp0(t->album_artist, name) == 0) return t->cover_path;
        if (!fallback) fallback = t->cover_path;
    }
    return fallback;
}

/* Перестраивает индекс исполнителей и модель списка. В режиме «Любимые» в модель
 * попадают только помеченные, поэтому раздел обновляется и по событию смены
 * пометок (см. on_favorites_changed). Индекс всегда содержит всех исполнителей:
 * на него опирается открытие страницы исполнителя. */
static void rebuild_store(ArtistsView *arv) {
    g_list_store_remove_all(arv->store);
    if (arv->index) {
        g_hash_table_destroy(arv->index);
        arv->index = NULL;
    }
    if (!arv->tracks) return;

    /* Группируем по исполнителям (album_artist и artist, если отличаются).
     * Один трек попадает в корзину исполнителя не более одного раза. */
    arv->index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, artist_bucket_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < arv->tracks->len; i++) {
        Track *t = g_ptr_array_index(arv->tracks, i);
        g_hash_table_remove_all(seen);
        index_tag(arv, t, t->album_artist, seen);
        if (g_strcmp0(t->artist, t->album_artist) != 0) {
            index_tag(arv, t, t->artist, seen);
        }
    }
    g_hash_table_destroy(seen);

    /* Модель списка исполнителей — одним splice (как в albums_view). */
    GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, arv->index);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        const char *name = key;
        GPtrArray *bucket = value;
        if (arv->liked_only && !favorites_artist_is_liked(name)) continue;
        g_ptr_array_add(items, artist_item_new(name, best_cover(bucket, name)));
    }
    if (items->len > 0) {
        g_list_store_splice(arv->store, 0, 0, items->pdata, items->len);
    }
    g_ptr_array_unref(items);
}

static void on_favorites_changed(gpointer user_data) {
    ArtistsView *arv = user_data;
    if (arv->liked_only) rebuild_store(arv);
}

ArtistsView *artists_view_new(Player *player, AdwOverlaySplitView *split_view, gboolean liked_only) {
    ArtistsView *arv = g_new0(ArtistsView, 1);
    arv->player = player;
    arv->liked_only = liked_only;
    arv->store = g_list_store_new(TYPE_ARTIST_ITEM);
    arv->controls = view_controls_new(FALSE, on_controls_changed, arv);

    MediaBrowserConfig cfg = {
        .title_col_name = "Исполнитель",
        .subtitle_col_name = NULL,
        .year_col_name = NULL,
        .get_title = get_item_name,
        .get_subtitle = NULL,
        .get_cover_path = get_item_cover,
        .get_year = NULL,
        .make_heart = make_artist_heart,
        .round_covers = TRUE,
    };

    arv->browser = media_browser_new(arv->store, arv->controls, &cfg, on_artist_activated, arv);

    GtkWidget *hb = ui_create_content_header(split_view);
    adw_header_bar_pack_end(ADW_HEADER_BAR(hb), view_controls_get_widget(arv->controls));

    AdwToolbarView *tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    gtk_widget_add_css_class(GTK_WIDGET(tv), "view");
    adw_toolbar_view_set_top_bar_style(tv, ADW_TOOLBAR_FLAT);
    adw_toolbar_view_add_top_bar(tv, hb);
    adw_toolbar_view_set_content(tv, media_browser_get_widget(arv->browser));

    AdwNavigationPage *root = adw_navigation_page_new(
        GTK_WIDGET(tv), liked_only ? "Любимые артисты" : "Исполнители");
    arv->nav = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
    arv->root = root;
    adw_navigation_view_add(arv->nav, root);

    /* В разделе «Любимые» лайк с любой карточки должен перестроить список. */
    if (liked_only) favorites_add_listener(on_favorites_changed, arv);

    return arv;
}

GtkWidget *artists_view_get_widget(ArtistsView *arv) {
    return GTK_WIDGET(arv->nav);
}

void artists_view_set_tracks(ArtistsView *arv, GPtrArray *tracks) {
    if (arv->tracks) g_ptr_array_unref(arv->tracks);
    arv->tracks = tracks ? g_ptr_array_ref(tracks) : NULL;
    rebuild_store(arv);
}

void artists_view_refresh_playing(ArtistsView *arv, const char *playing_uri) {
    /* Верхняя страница стека: если это страница исполнителя или альбома, у неё
     * зарегистрированы списки строк для подсветки. Корневой список исполнителей
     * строк-треков не содержит. */
    AdwNavigationPage *top = adw_navigation_view_get_visible_page(arv->nav);
    if (top && top != arv->root) {
        ui_highlight_playing_on_page(GTK_WIDGET(top), playing_uri);
    }
}

void artists_view_free(ArtistsView *arv) {
    if (!arv) return;
    favorites_remove_listener(on_favorites_changed, arv);
    if (arv->tracks) g_ptr_array_unref(arv->tracks);
    if (arv->index) g_hash_table_destroy(arv->index);
    media_browser_free(arv->browser);
    view_controls_free(arv->controls);
    if (arv->store) g_object_unref(arv->store);
    g_free(arv);
}
