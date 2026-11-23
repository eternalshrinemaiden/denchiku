#include "tracks_view.h"
#include "album_detail_page.h"
#include "track_list.h"
#include "ui_helpers.h"
#include "view_controls.h"

struct _TracksView {
    Player *player;
    GPtrArray *tracks;
    AdwNavigationView *nav;
    AdwNavigationPage *root;
    TrackList *list;
    ViewControls *controls;
};

static void on_controls_changed(ViewControls *vc, gpointer user_data) {
    (void)vc;
    TracksView *tv = user_data;
    track_list_refresh(tv->list);
}

TracksView *tracks_view_new(Player *player, AdwOverlaySplitView *split_view) {
    TracksView *tv = g_new0(TracksView, 1);
    tv->player = player;
    tv->controls = view_controls_new(TRUE, on_controls_changed, tv);
    TrackListConfig cfg = {
        .context = TRACK_CONTEXT_TRACKS,
        .controls = tv->controls,
    };
    tv->list = track_list_new(player, &cfg);

    GtkWidget *hb = ui_create_content_header(split_view);
    adw_header_bar_pack_end(ADW_HEADER_BAR(hb), view_controls_get_widget(tv->controls));

    AdwToolbarView *toolbar_view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    gtk_widget_add_css_class(GTK_WIDGET(toolbar_view), "view");
    adw_toolbar_view_set_top_bar_style(toolbar_view, ADW_TOOLBAR_FLAT);
    adw_toolbar_view_add_top_bar(toolbar_view, hb);
    adw_toolbar_view_set_content(toolbar_view, track_list_get_widget(tv->list));

    AdwNavigationPage *root = adw_navigation_page_new(GTK_WIDGET(toolbar_view), "Треки");
    /* Список треков участвует в подсветке играющего трека. */
    g_object_set_data(G_OBJECT(root), "playing-tracklist", tv->list);
    tv->root = root;
    tv->nav = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
    adw_navigation_view_add(tv->nav, root);
    return tv;
}

GtkWidget *tracks_view_get_widget(TracksView *tv) {
    return GTK_WIDGET(tv->nav);
}

void tracks_view_set_tracks(TracksView *tv, GPtrArray *tracks) {
    if (tv->tracks) g_ptr_array_unref(tv->tracks);
    tv->tracks = tracks ? g_ptr_array_ref(tracks) : NULL;

    track_list_set_tracks(tv->list, tracks);
    Track *cur = player_get_current_track(tv->player);
    track_list_set_playing_uri(tv->list, cur ? cur->uri : NULL);
}

/* Альбом открывается в этом же стеке: раздел «Треки» не переключается. */
void tracks_view_open_album(TracksView *tv, const char *album, const char *album_artist) {
    if (!tv->tracks || !album || !*album) return;

    GtkListBox *sublist = NULL;
    AdwNavigationPage *page = album_detail_page_create(tv->player, tv->tracks,
                                                       album, album_artist, &sublist);
    if (page) adw_navigation_view_push(tv->nav, page);
}

void tracks_view_refresh_playing(TracksView *tv, const char *playing_uri) {
    AdwNavigationPage *top = adw_navigation_view_get_visible_page(tv->nav);
    if (!top) return;
    GtkWidget *page = GTK_WIDGET(top);
    TrackList *tl = g_object_get_data(G_OBJECT(page), "playing-tracklist");
    if (tl) track_list_set_playing_uri(tl, playing_uri);
    ui_highlight_playing_on_page(page, playing_uri);
}

void tracks_view_free(TracksView *tv) {
    if (tv->tracks) g_ptr_array_unref(tv->tracks);
    if (tv->controls) view_controls_free(tv->controls);
    g_free(tv);
}
