#include "library_view.h"
#include "albums_view.h"
#include "artists_view.h"
#include "genres_view.h"
#include "tracks_view.h"
#include "track.h"

struct _LibraryView {
    Player *player;
    AlbumsView *albums;
    ArtistsView *artists;
    GenresView *genres;
    TracksView *tracks;
};

LibraryView *library_view_new(Player *player, AdwOverlaySplitView *split_view) {
    LibraryView *lv = g_new0(LibraryView, 1);
    lv->player  = player;
    lv->albums  = albums_view_new(player, split_view);
    lv->artists = artists_view_new(player, split_view);
    lv->genres  = genres_view_new(player, split_view);
    lv->tracks  = tracks_view_new(player, split_view);
    return lv;
}

void library_view_attach_to_stack(LibraryView *lv, GtkStack *stack) {
    gtk_stack_add_named(stack, albums_view_get_widget(lv->albums),   "albums");
    gtk_stack_add_named(stack, artists_view_get_widget(lv->artists), "artists");
    gtk_stack_add_named(stack, genres_view_get_widget(lv->genres),   "genres");
    gtk_stack_add_named(stack, tracks_view_get_widget(lv->tracks),   "tracks");
}

void library_view_set_tracks(LibraryView *lv, GPtrArray *tracks) {
    albums_view_set_tracks(lv->albums, tracks);
    artists_view_set_tracks(lv->artists, tracks);
    genres_view_set_tracks(lv->genres, tracks);
    tracks_view_set_tracks(lv->tracks, tracks);
}

void library_view_regroup_artists(LibraryView *lv, GPtrArray *tracks) {
    artists_view_set_tracks(lv->artists, tracks);
}

void library_view_regroup_genres(LibraryView *lv, GPtrArray *tracks) {
    genres_view_set_tracks(lv->genres, tracks);
}

void library_view_select_genre(LibraryView *lv, const char *genre) {
    genres_view_select_genre(lv->genres, genre);
}

void library_view_refresh_playing(LibraryView *lv) {
    Track *cur = player_get_current_track(lv->player);
    const char *uri = cur ? cur->uri : NULL;

    albums_view_refresh_playing(lv->albums, uri);
    artists_view_refresh_playing(lv->artists, uri);
    genres_view_refresh_playing(lv->genres, uri);
    tracks_view_refresh_playing(lv->tracks, uri);
}

void library_view_open_album(LibraryView *lv, const char *section,
                             const char *album, const char *album_artist) {
    /* Открываем альбом в том же разделе, где пользователь вызвал действие. */
    if (g_strcmp0(section, "artists") == 0) {
        artists_view_open_album(lv->artists, album, album_artist);
    } else if (g_strcmp0(section, "genres") == 0) {
        genres_view_open_album(lv->genres, album, album_artist);
    } else if (g_strcmp0(section, "tracks") == 0) {
        tracks_view_open_album(lv->tracks, album, album_artist);
    } else {
        albums_view_open_album(lv->albums, album, album_artist);
    }
}

void library_view_open_artist(LibraryView *lv, const char *section, const char *artist) {
    /* Открываем страницу исполнителя в том же разделе, где пользователь вызвал
     * действие: вкладка не переключается, страница ложится в стек раздела. */
    if (g_strcmp0(section, "albums") == 0) {
        albums_view_open_artist(lv->albums, artist);
    } else if (g_strcmp0(section, "genres") == 0) {
        genres_view_open_artist(lv->genres, artist);
    } else if (g_strcmp0(section, "tracks") == 0) {
        tracks_view_open_artist(lv->tracks, artist);
    } else {
        artists_view_open_artist(lv->artists, artist);
    }
}

ViewControls *library_view_get_view_controls(LibraryView *lv) {
    return albums_view_get_controls(lv->albums);
}

void library_view_free(LibraryView *lv) {
    if (!lv) return;
    albums_view_free(lv->albums);
    artists_view_free(lv->artists);
    genres_view_free(lv->genres);
    tracks_view_free(lv->tracks);
    g_free(lv);
}
