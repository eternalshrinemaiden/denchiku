#include "artist_detail_page.h"
#include "stats.h"
#include "track.h"
#include "track_menu.h"
#include "ui_helpers.h"

#define HERO_COVER_SIZE 180
#define CARD_COVER_SIZE 150

/* Сканер подставляет это название, когда тег альбома отсутствует. Считаем такие
 * треки «синглами» (у них нет альбома). */
#define UNKNOWN_ALBUM "Неизвестный альбом"

static gboolean is_unknown_album(const char *album) {
    return !album || !*album || g_strcmp0(album, UNKNOWN_ALBUM) == 0;
}

/* ===== карточка альбома для горизонтальной полки ===== */

static void on_album_card_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    const char *album = g_object_get_data(G_OBJECT(btn), "album");
    const char *album_artist = g_object_get_data(G_OBJECT(btn), "album-artist");
    ArtistOpenAlbumFn fn = g_object_get_data(G_OBJECT(btn), "open-fn");
    gpointer data = g_object_get_data(G_OBJECT(btn), "open-data");
    if (fn && album && *album) fn(album, album_artist, data);
}

static GtkWidget *create_album_card(const char *album, const char *album_artist,
                                    const char *cover_path, guint year,
                                    ArtistOpenAlbumFn on_open_album, gpointer user_data) {
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_set_valign(btn, GTK_ALIGN_START);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(box, CARD_COVER_SIZE + 12, -1);
    gtk_box_append(GTK_BOX(box), ui_create_cover_widget(cover_path, CARD_COVER_SIZE));

    GtkWidget *title = gtk_label_new(album ? album : "");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title), (CARD_COVER_SIZE + 12) / 8);
    gtk_box_append(GTK_BOX(box), title);

    if (year > 0) {
        char *ys = g_strdup_printf("%u", year);
        GtkWidget *year_lbl = gtk_label_new(ys);
        g_free(ys);
        gtk_label_set_xalign(GTK_LABEL(year_lbl), 0.0f);
        gtk_widget_add_css_class(year_lbl, "dim-label");
        gtk_widget_add_css_class(year_lbl, "caption");
        gtk_box_append(GTK_BOX(box), year_lbl);
    }

    gtk_button_set_child(GTK_BUTTON(btn), box);
    g_object_set_data_full(G_OBJECT(btn), "album", g_strdup(album ? album : ""), g_free);
    g_object_set_data_full(G_OBJECT(btn), "album-artist",
                           g_strdup(album_artist ? album_artist : ""), g_free);
    g_object_set_data(G_OBJECT(btn), "open-fn", (gpointer)on_open_album);
    g_object_set_data(G_OBJECT(btn), "open-data", user_data);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_album_card_clicked), NULL);
    return btn;
}

/* ===== карточка сингла (трека без альбома) ===== */

static void on_single_card_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    Track *t = g_object_get_data(G_OBJECT(btn), "track");
    Player *player = g_object_get_data(G_OBJECT(btn), "player");
    if (!t || !player) return;
    GPtrArray *one = g_ptr_array_new(); /* player копирует треки */
    g_ptr_array_add(one, t);
    player_set_queue_and_play(player, one, 0);
    g_ptr_array_unref(one);
}

static GtkWidget *create_single_card(Track *t, Player *player) {
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_set_valign(btn, GTK_ALIGN_START);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_size_request(box, CARD_COVER_SIZE + 12, -1);
    gtk_box_append(GTK_BOX(box), ui_create_cover_widget(t->cover_path, CARD_COVER_SIZE));

    GtkWidget *title = gtk_label_new(t->title ? t->title : "");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title), (CARD_COVER_SIZE + 12) / 8);
    gtk_box_append(GTK_BOX(box), title);

    GtkWidget *sub = gtk_label_new(t->artist ? t->artist : "");
    gtk_label_set_xalign(GTK_LABEL(sub), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(sub, "dim-label");
    gtk_widget_add_css_class(sub, "caption");
    gtk_box_append(GTK_BOX(box), sub);

    gtk_button_set_child(GTK_BUTTON(btn), box);
    g_object_set_data(G_OBJECT(btn), "track", t);
    g_object_set_data(G_OBJECT(btn), "player", player);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_single_card_clicked), NULL);
    return btn;
}

/* ===== заголовок-раздел с горизонтальной полкой карточек ===== */

static GtkWidget *shelf_new(const char *title, GtkWidget **out_row) {
    GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

    GtkWidget *lbl = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_widget_add_css_class(lbl, "title-4");
    gtk_box_append(GTK_BOX(section), lbl);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_widget_set_vexpand(scrolled, FALSE);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), row);
    gtk_box_append(GTK_BOX(section), scrolled);

    *out_row = row;
    return section;
}

/* ===== популярные треки ===== */

static gint compare_popular(gconstpointer a, gconstpointer b, gpointer user_data) {
    (void)user_data;
    const Track *t1 = *(const Track *const *)a;
    const Track *t2 = *(const Track *const *)b;

    guint p1 = stats_get_plays(t1->uri);
    guint p2 = stats_get_plays(t2->uri);
    if (p1 != p2) return (p1 < p2) ? 1 : -1; /* больше прослушиваний — выше */

    int c = g_strcmp0(t1->album, t2->album);
    if (c != 0) return c;
    if (t1->disc_number != t2->disc_number) return (t1->disc_number < t2->disc_number) ? -1 : 1;
    if (t1->track_number != t2->track_number) return (t1->track_number < t2->track_number) ? -1 : 1;
    return 0;
}

static void on_popular_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    Player *player = user_data;
    GPtrArray *tracks = g_object_get_data(G_OBJECT(box), "context-tracks");
    if (!tracks) return;

    gpointer idx = g_object_get_data(G_OBJECT(row), "context-index");
    if (!idx) return;
    int index = (int)GPOINTER_TO_UINT(idx) - 1;
    if (index < 0 || index >= (int)tracks->len) return;
    player_set_queue_and_play(player, tracks, index);
}

static int index_of_track(GPtrArray *tracks, Track *t) {
    for (guint i = 0; i < tracks->len; i++) {
        if (g_ptr_array_index(tracks, i) == t) return (int)i;
    }
    return -1;
}

static GtkWidget *create_popular_row(Player *player, Track *t, guint number,
                                     guint index_in_bucket, const char *playing_uri) {
    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), t->title ? t->title : "Без названия");
    adw_action_row_set_title_lines(row, 1);

    const char *sub = is_unknown_album(t->album) ? t->artist : t->album;
    if (sub && *sub) {
        adw_action_row_set_subtitle(row, sub);
        adw_action_row_set_subtitle_lines(row, 1);
    }

    char num_str[16];
    g_snprintf(num_str, sizeof num_str, "%u", number);
    GtkWidget *num_lbl = gtk_label_new(num_str);
    gtk_widget_set_size_request(num_lbl, 28, -1);
    gtk_label_set_xalign(GTK_LABEL(num_lbl), 0.5);
    gtk_widget_add_css_class(num_lbl, "dim-label");
    gtk_widget_add_css_class(num_lbl, "numeric");
    adw_action_row_add_prefix(row, num_lbl);

    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    g_object_set_data(G_OBJECT(row), "track", t);
    g_object_set_data(G_OBJECT(row), "context-index", GUINT_TO_POINTER(index_in_bucket + 1));

    if (playing_uri && g_strcmp0(t->uri, playing_uri) == 0) {
        ui_set_row_playing(row, TRUE);
    }

    track_menu_attach(GTK_WIDGET(row), t, TRACK_CONTEXT_ARTIST, player);
    return GTK_WIDGET(row);
}

/* ===== шапка: круглая обложка ===== */

static GtkWidget *create_round_cover_box(const char *cover_path, int size) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    /* Минимальную ширину не задаём (только высоту), чтобы страница не раздувала
     * минимум AdwOverlaySplitView на узком окне. */
    gtk_widget_set_size_request(card, 0, size);
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(card, FALSE);
    gtk_widget_set_vexpand(card, FALSE);
    gtk_widget_set_overflow(card, GTK_OVERFLOW_HIDDEN);
    ui_make_cover_round(card);

    GdkTexture *tex = cover_path ? ui_create_cover_texture(cover_path, size) : NULL;
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

    GtkWidget *icon = gtk_image_new_from_icon_name("system-users-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), size / 2);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(icon, TRUE);
    gtk_widget_add_css_class(icon, "dim-label");
    gtk_box_append(GTK_BOX(card), icon);

    return card;
}

/* ===== вспомогательные данные страницы ===== */

static void on_play_all_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    Player *player = g_object_get_data(G_OBJECT(btn), "player");
    GPtrArray *tracks = g_object_get_data(G_OBJECT(btn), "tracks");
    if (player && tracks && tracks->len > 0) {
        player_set_queue_and_play(player, tracks, 0);
    }
}

typedef struct {
    char *album;
    char *album_artist;
    char *cover;
    guint year;
} AlbumCard;

static void album_card_free(gpointer data) {
    AlbumCard *ac = data;
    g_free(ac->album);
    g_free(ac->album_artist);
    g_free(ac->cover);
    g_free(ac);
}

/* Группирует альбомы по паре (название, album_artist), дозаполняя обложку/год
 * из треков. only_album_artist: TRUE — только альбомы самого исполнителя,
 * FALSE — только альбомы других (участие). */
static GPtrArray *collect_albums(GPtrArray *tracks, const char *artist,
                                 gboolean only_album_artist) {
    GPtrArray *out = g_ptr_array_new_with_free_func(album_card_free);
    GHashTable *by_key = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (guint i = 0; i < tracks->len; i++) {
        Track *t = g_ptr_array_index(tracks, i);
        if (is_unknown_album(t->album)) continue;

        gboolean is_owner = g_strcmp0(t->album_artist, artist) == 0;
        if (is_owner != only_album_artist) continue;

        char *key = g_strdup_printf("%s\x1f%s",
                                    t->album ? t->album : "",
                                    t->album_artist ? t->album_artist : "");
        AlbumCard *ac = g_hash_table_lookup(by_key, key);
        if (!ac) {
            ac = g_new0(AlbumCard, 1);
            ac->album = g_strdup(t->album);
            ac->album_artist = g_strdup(t->album_artist);
            ac->cover = g_strdup(t->cover_path);
            ac->year = t->year;
            g_hash_table_insert(by_key, key, ac);
            g_ptr_array_add(out, ac);
        } else {
            if ((!ac->cover || !*ac->cover) && t->cover_path && *t->cover_path) {
                g_free(ac->cover);
                ac->cover = g_strdup(t->cover_path);
            }
            if (ac->year == 0 && t->year > 0) ac->year = t->year;
            g_free(key);
        }
    }
    g_hash_table_destroy(by_key);
    return out;
}

static const char *best_artist_cover(GPtrArray *tracks, const char *artist) {
    const char *fallback = NULL;
    for (guint i = 0; i < tracks->len; i++) {
        Track *t = g_ptr_array_index(tracks, i);
        if (!t->cover_path || !*t->cover_path) continue;
        if (g_strcmp0(t->album_artist, artist) == 0) return t->cover_path;
        if (!fallback) fallback = t->cover_path;
    }
    return fallback;
}

/* ===== построение страницы ===== */

AdwNavigationPage *artist_detail_page_create(Player *player,
                                             GPtrArray *artist_tracks,
                                             GPtrArray *all_tracks,
                                             const char *artist,
                                             ArtistOpenAlbumFn on_open_album,
                                             gpointer user_data) {
    if (!artist_tracks || artist_tracks->len == 0 || !artist) return NULL;

    GPtrArray *own_albums = collect_albums(artist_tracks, artist, TRUE);
    GPtrArray *features = collect_albums(artist_tracks, artist, FALSE);

    /* Синглы — треки исполнителя без альбома. */
    GPtrArray *singles = g_ptr_array_new();
    for (guint i = 0; i < artist_tracks->len; i++) {
        Track *t = g_ptr_array_index(artist_tracks, i);
        if (is_unknown_album(t->album)) g_ptr_array_add(singles, t);
    }

    /* Популярное: до пяти треков по числу прослушиваний. */
    GPtrArray *popular = g_ptr_array_new();
    for (guint i = 0; i < artist_tracks->len; i++) {
        g_ptr_array_add(popular, g_ptr_array_index(artist_tracks, i));
    }
    g_ptr_array_sort_with_data(popular, compare_popular, NULL);
    if (popular->len > 5) g_ptr_array_set_size(popular, 5);

    GtkWidget *outer_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(outer_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 900);
    adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 500);
    gtk_widget_set_margin_top(clamp, 24);
    gtk_widget_set_margin_bottom(clamp, 24);
    gtk_widget_set_margin_start(clamp, 16);
    gtk_widget_set_margin_end(clamp, 16);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 28);

    /* Шапка. */
    GtkWidget *hero = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 28);
    GtkWidget *meta = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_valign(meta, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(meta, TRUE);

    GtkWidget *kind_lbl = gtk_label_new("Исполнитель");
    gtk_label_set_xalign(GTK_LABEL(kind_lbl), 0.0f);
    gtk_widget_add_css_class(kind_lbl, "dim-label");
    gtk_widget_add_css_class(kind_lbl, "caption-heading");
    gtk_box_append(GTK_BOX(meta), kind_lbl);

    GtkWidget *title_lbl = gtk_label_new(artist);
    gtk_label_set_xalign(GTK_LABEL(title_lbl), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(title_lbl), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(title_lbl), PANGO_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(title_lbl, "title-1");
    gtk_box_append(GTK_BOX(meta), title_lbl);

    char *counts = g_strdup_printf("%u трек(ов) · %u альбом(ов)",
                                   artist_tracks->len, own_albums->len);
    GtkWidget *counts_lbl = gtk_label_new(counts);
    g_free(counts);
    gtk_label_set_xalign(GTK_LABEL(counts_lbl), 0.0f);
    gtk_widget_add_css_class(counts_lbl, "dim-label");
    gtk_box_append(GTK_BOX(meta), counts_lbl);

    GtkWidget *play_btn = gtk_button_new_with_label("Воспроизвести");
    gtk_widget_set_halign(play_btn, GTK_ALIGN_START);
    gtk_widget_set_margin_top(play_btn, 12);
    gtk_widget_add_css_class(play_btn, "suggested-action");
    gtk_widget_add_css_class(play_btn, "pill");
    g_object_set_data(G_OBJECT(play_btn), "player", player);
    g_object_set_data(G_OBJECT(play_btn), "tracks", artist_tracks);
    g_signal_connect(play_btn, "clicked", G_CALLBACK(on_play_all_clicked), NULL);
    gtk_box_append(GTK_BOX(meta), play_btn);

    gtk_box_append(GTK_BOX(hero), meta);
    gtk_box_append(GTK_BOX(hero), create_round_cover_box(best_artist_cover(artist_tracks, artist),
                                                         HERO_COVER_SIZE));
    gtk_box_append(GTK_BOX(vbox), hero);

    /* Списки, в которых обновляется подсветка играющего трека. */
    GPtrArray *playing_lists = g_ptr_array_new(); /* заимствованные GtkListBox* */

    /* Популярное. */
    GtkWidget *pop_heading = gtk_label_new("Популярное");
    gtk_label_set_xalign(GTK_LABEL(pop_heading), 0.0f);
    gtk_widget_add_css_class(pop_heading, "title-4");
    gtk_box_append(GTK_BOX(vbox), pop_heading);

    GtkListBox *pop_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_add_css_class(GTK_WIDGET(pop_list), "boxed-list");
    gtk_list_box_set_selection_mode(pop_list, GTK_SELECTION_NONE);
    g_object_set_data(G_OBJECT(pop_list), "context-tracks", artist_tracks);
    g_signal_connect(pop_list, "row-activated", G_CALLBACK(on_popular_activated), player);
    Track *cur = player_get_current_track(player);
    const char *playing_uri = cur ? cur->uri : NULL;
    for (guint i = 0; i < popular->len; i++) {
        Track *t = g_ptr_array_index(popular, i);
        int bucket_index = index_of_track(artist_tracks, t);
        if (bucket_index < 0) continue;
        gtk_list_box_append(pop_list,
                            create_popular_row(player, t, i + 1, (guint)bucket_index, playing_uri));
    }
    gtk_box_append(GTK_BOX(vbox), GTK_WIDGET(pop_list));
    g_ptr_array_add(playing_lists, pop_list);

    /* Альбомы. */
    if (own_albums->len > 0) {
        GtkWidget *row = NULL;
        GtkWidget *section = shelf_new("Альбомы", &row);
        for (guint i = 0; i < own_albums->len; i++) {
            AlbumCard *ac = g_ptr_array_index(own_albums, i);
            gtk_box_append(GTK_BOX(row),
                           create_album_card(ac->album, ac->album_artist, ac->cover, ac->year, on_open_album, user_data));
        }
        gtk_box_append(GTK_BOX(vbox), section);
    }

    /* Синглы. */
    if (singles->len > 0) {
        GtkWidget *row = NULL;
        GtkWidget *section = shelf_new("Синглы", &row);
        for (guint i = 0; i < singles->len; i++) {
            gtk_box_append(GTK_BOX(row),
                           create_single_card(g_ptr_array_index(singles, i), player));
        }
        gtk_box_append(GTK_BOX(vbox), section);
    }

    /* С участием. */
    if (features->len > 0) {
        GtkWidget *row = NULL;
        GtkWidget *section = shelf_new("С участием", &row);
        for (guint i = 0; i < features->len; i++) {
            AlbumCard *ac = g_ptr_array_index(features, i);
            gtk_box_append(GTK_BOX(row),
                           create_album_card(ac->album, ac->album_artist, ac->cover, ac->year, on_open_album, user_data));
        }
        gtk_box_append(GTK_BOX(vbox), section);
    }

    adw_clamp_set_child(ADW_CLAMP(clamp), vbox);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(outer_scroll), clamp);

    AdwToolbarView *tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    AdwHeaderBar *hb = ADW_HEADER_BAR(adw_header_bar_new());
    adw_header_bar_set_show_start_title_buttons(hb, FALSE);
    adw_toolbar_view_add_top_bar(tv, GTK_WIDGET(hb));
    adw_toolbar_view_set_content(tv, outer_scroll);

    AdwNavigationPage *page = adw_navigation_page_new(GTK_WIDGET(tv), artist);

    /* Держим исходные массивы, пока жива страница: строки ссылаются на Track*. */
    ui_pin_tracks_reference(GTK_WIDGET(page),
                            all_tracks ? all_tracks : artist_tracks);
    /* Корзина треков исполнителя принадлежит индексу artists_view и живёт до
     * пересканирования; страница держит собственную ссылку, чтобы «Популярное»
     * не осталось с висячим указателем после пересканирования. */
    g_object_set_data_full(G_OBJECT(page), "artist-tracks",
                           g_ptr_array_ref(artist_tracks), (GDestroyNotify)g_ptr_array_unref);
    g_object_set_data_full(G_OBJECT(page), "playing-listboxes",
                           playing_lists, (GDestroyNotify)g_ptr_array_unref);

    g_ptr_array_unref(popular);
    g_ptr_array_unref(singles);
    g_ptr_array_unref(own_albums);
    g_ptr_array_unref(features);
    return page;
}
