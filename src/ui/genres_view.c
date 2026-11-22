#include "genres_view.h"
#include "album_detail_page.h"
#include "track.h"
#include "track_list.h"
#include "ui_helpers.h"

struct _GenresView {
    Player *player;
    GPtrArray *tracks;
    AdwNavigationView *nav;
    AdwNavigationPage *root;
    GtkListBox *list;
    GHashTable *index; /* имя жанра -> GPtrArray* треков (заимствованы) */
};

static void genre_bucket_free(gpointer data) {
    g_ptr_array_unref(data);
}

/* Добавляет трек в корзину, пропуская повтор для того же трека. */
static void bucket_add(GPtrArray *bucket, Track *t) {
    guint n = bucket->len;
    if (n > 0 && g_ptr_array_index(bucket, n - 1) == t) return;
    g_ptr_array_add(bucket, t);
}

/* Альбом открывается в этом же стеке: раздел не переключается, а страница жанра
 * остаётся в истории. */
void genres_view_open_album(GenresView *gv, const char *album, const char *album_artist) {
    if (!gv->tracks || !album || !*album) return;

    GtkListBox *sublist = NULL;
    AdwNavigationPage *page = album_detail_page_create(gv->player, gv->tracks,
                                                       album, album_artist, &sublist);
    if (page) adw_navigation_view_push(gv->nav, page);
}

static void on_genre_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box;
    GenresView *gv = user_data;
    if (!gv->tracks) return;

    const char *genre = adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row));
    /* Треки берём из индекса, построенного при группировке. */
    GPtrArray *matches = gv->index ? g_hash_table_lookup(gv->index, genre) : NULL;
    GPtrArray *filtered = matches ? g_ptr_array_ref(matches) : g_ptr_array_new();

    TrackListConfig sub_cfg = { .context = TRACK_CONTEXT_GENRE };
    TrackList *tl = track_list_new(gv->player, &sub_cfg);
    track_list_set_tracks(tl, filtered);
    Track *cur = player_get_current_track(gv->player);
    track_list_set_playing_uri(tl, cur ? cur->uri : NULL);
    ui_pin_tracks_reference(track_list_get_widget(tl), gv->tracks);
    g_ptr_array_unref(filtered);

    AdwToolbarView *tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    AdwHeaderBar *hb = ADW_HEADER_BAR(adw_header_bar_new());
    adw_header_bar_set_show_start_title_buttons(hb, FALSE);
    adw_toolbar_view_add_top_bar(tv, GTK_WIDGET(hb));
    adw_toolbar_view_set_content(tv, track_list_get_widget(tl));

    AdwNavigationPage *sub_page = adw_navigation_page_new(GTK_WIDGET(tv), genre);
    /* Список треков жанра участвует в подсветке играющего трека. */
    g_object_set_data(G_OBJECT(sub_page), "playing-tracklist", tl);
    adw_navigation_view_push(gv->nav, sub_page);
}

GenresView *genres_view_new(Player *player, AdwOverlaySplitView *split_view) {
    GenresView *gv = g_new0(GenresView, 1);
    gv->player = player;

    GtkWidget *scrolled = ui_create_scrolled_list(&gv->list);
    g_signal_connect(gv->list, "row-activated", G_CALLBACK(on_genre_activated), gv);

    AdwToolbarView *tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(tv, ui_create_content_header(split_view));
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

static void index_genre(GenresView *gv, Track *t, const char *tag) {
    char **genres = track_split_genres(tag);
    if (!genres) return;

    for (guint i = 0; genres[i] != NULL; i++) {
        GPtrArray *bucket = g_hash_table_lookup(gv->index, genres[i]);
        if (!bucket) {
            bucket = g_ptr_array_new();
            g_hash_table_insert(gv->index, g_strdup(genres[i]), bucket);
            GtkWidget *r = ui_create_plain_row(genres[i], NULL);
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(r), TRUE);
            adw_action_row_add_suffix(ADW_ACTION_ROW(r), gtk_image_new_from_icon_name("go-next-symbolic"));
            gtk_list_box_append(gv->list, r);
        }
        bucket_add(bucket, t);
    }
    g_strfreev(genres);
}

void genres_view_set_tracks(GenresView *gv, GPtrArray *tracks) {
    if (gv->tracks) g_ptr_array_unref(gv->tracks);
    gv->tracks = tracks ? g_ptr_array_ref(tracks) : NULL;

    ui_clear_list_box(gv->list);
    if (gv->index) {
        g_hash_table_destroy(gv->index);
        gv->index = NULL;
    }
    if (!gv->tracks) return;

    gv->index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, genre_bucket_free);
    for (guint i = 0; i < gv->tracks->len; i++) {
        Track *t = g_ptr_array_index(gv->tracks, i);
        index_genre(gv, t, t->genre);
    }
}

void genres_view_refresh_playing(GenresView *gv, const char *playing_uri) {
    AdwNavigationPage *top = adw_navigation_view_get_visible_page(gv->nav);
    if (!top || top == gv->root) return;
    GtkWidget *page = GTK_WIDGET(top);
    TrackList *tl = g_object_get_data(G_OBJECT(page), "playing-tracklist");
    if (tl) track_list_set_playing_uri(tl, playing_uri);
    ui_highlight_playing_on_page(page, playing_uri);
}

void genres_view_free(GenresView *gv) {
    if (!gv) return;
    if (gv->tracks) g_ptr_array_unref(gv->tracks);
    if (gv->index) g_hash_table_destroy(gv->index);
    g_free(gv);
}
