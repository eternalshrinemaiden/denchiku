#include "track_menu.h"

#include "favorites.h"

/* Состояние, привязанное к конкретному меню: над каким треком и в каком
 * проигрывателе выполняются действия. */
typedef struct {
    Track *track;
    Player *player;
} TrackMenuActions;

static TrackOpenAlbumFn open_album_cb;
static gpointer open_album_data;

static TrackOpenArtistFn open_artist_cb;
static gpointer open_artist_data;

static TrackShowInfoFn show_info_cb;
static gpointer show_info_data;

void track_menu_set_open_album_cb(TrackOpenAlbumFn cb, gpointer user_data) {
    open_album_cb = cb;
    open_album_data = user_data;
}

void track_menu_set_open_artist_cb(TrackOpenArtistFn cb, gpointer user_data) {
    open_artist_cb = cb;
    open_artist_data = user_data;
}

void track_menu_set_show_info_cb(TrackShowInfoFn cb, gpointer user_data) {
    show_info_cb = cb;
    show_info_data = user_data;
}

/* Колбэки работы с плейлистами и реестр кнопок-«сердечек»: состояние кнопок
 * должно синхронно обновляться во всех списках и плеер-баре. */
static TrackIsFavoriteFn     playlist_is_favorite_cb;
static TrackToggleFavoriteFn playlist_toggle_favorite_cb;
static TrackListPlaylistsFn  playlist_list_cb;
static TrackPlaylistHasFn    playlist_has_cb;
static TrackTogglePlaylistFn playlist_toggle_cb;
static gpointer              playlist_cb_data;

static GPtrArray *favorite_buttons; /* GtkWidget* (weak, без владения) */

void track_menu_set_playlist_cbs(TrackIsFavoriteFn is_favorite,
                                 TrackToggleFavoriteFn toggle_favorite,
                                 TrackListPlaylistsFn list_playlists,
                                 TrackPlaylistHasFn playlist_has,
                                 TrackTogglePlaylistFn toggle_playlist,
                                 gpointer user_data) {
    playlist_is_favorite_cb = is_favorite;
    playlist_toggle_favorite_cb = toggle_favorite;
    playlist_list_cb = list_playlists;
    playlist_has_cb = playlist_has;
    playlist_toggle_cb = toggle_playlist;
    playlist_cb_data = user_data;
}

static void ensure_favorite_css(void) {
    static gboolean loaded = FALSE;
    if (loaded) return;
    loaded = TRUE;

    GtkCssProvider *css = gtk_css_provider_new();
    /* Закрашенное сердечко (трек в «Любимых») — красное. */
    gtk_css_provider_load_from_string(css,
        ".denchiku-favorite, .denchiku-favorite image {"
        "  color: #e01b24;"
        "}");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* Виды «сердечек»: трек (в «Любимых треках»), альбом и исполнитель (пометка
 * сущности в модуле favorites, как в Spotify). */
enum { FAV_TRACK, FAV_ALBUM, FAV_ARTIST };

static gboolean favorite_is_active(GtkWidget *btn) {
    int kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "fav-kind"));
    if (kind == FAV_ALBUM) {
        return favorites_album_is_liked(g_object_get_data(G_OBJECT(btn), "fav-album"),
                                        g_object_get_data(G_OBJECT(btn), "fav-album-artist"));
    }
    if (kind == FAV_ARTIST) {
        return favorites_artist_is_liked(g_object_get_data(G_OBJECT(btn), "fav-artist"));
    }
    const char *uri = g_object_get_data(G_OBJECT(btn), "fav-uri");
    return uri && playlist_is_favorite_cb && playlist_is_favorite_cb(uri, playlist_cb_data);
}

static void update_favorite_button(GtkWidget *btn) {
    int kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "fav-kind"));
    gboolean favorite = favorite_is_active(btn);

    /* Для трека кнопка активна только когда известен URI. */
    gtk_widget_set_sensitive(btn, kind != FAV_TRACK ||
                                  g_object_get_data(G_OBJECT(btn), "fav-uri") != NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), favorite);
    /* Закрашенное красное сердечко для «Любимых», контурное — для остальных. */
    gtk_button_set_icon_name(GTK_BUTTON(btn),
        favorite ? "denchiku-heart-filled-symbolic" : "denchiku-heart-outline-symbolic");
    if (favorite) {
        gtk_widget_add_css_class(btn, "denchiku-favorite");
    } else {
        gtk_widget_remove_css_class(btn, "denchiku-favorite");
    }
    gtk_widget_set_tooltip_text(btn, favorite ? "Убрать из любимых"
                                               : "Добавить в любимые");
}

static void on_fav_button_destroyed(gpointer data, GObject *where) {
    (void)data;
    if (favorite_buttons) g_ptr_array_remove(favorite_buttons, where);
}

static void on_favorite_button_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    int kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "fav-kind"));
    gboolean desired = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(btn));

    /* Обработчик может перестроить списки и уничтожить эту кнопку (например,
     * при показе «Любимых треков» или перестройке полки «Любимые»), поэтому
     * держим ссылку на время вызова. */
    g_object_ref(btn);
    if (kind == FAV_ALBUM) {
        favorites_album_set_liked(g_object_get_data(G_OBJECT(btn), "fav-album"),
                                  g_object_get_data(G_OBJECT(btn), "fav-album-artist"),
                                  desired);
        /* Могли измениться полки «Любимые» и другие сердца-сущности. */
        track_menu_refresh_favorites();
    } else if (kind == FAV_ARTIST) {
        favorites_artist_set_liked(g_object_get_data(G_OBJECT(btn), "fav-artist"), desired);
        track_menu_refresh_favorites();
    } else {
        const char *uri = g_object_get_data(G_OBJECT(btn), "fav-uri");
        if (uri && playlist_toggle_favorite_cb) {
            playlist_toggle_favorite_cb(uri, desired, playlist_cb_data);
        }
        update_favorite_button(GTK_WIDGET(btn));
    }
    g_object_unref(btn);
}

GtkWidget *track_favorite_button_new(const char *uri) {
    if (!favorite_buttons) favorite_buttons = g_ptr_array_new();

    ensure_favorite_css();

    GtkWidget *btn = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(btn), "denchiku-heart-outline-symbolic");
    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(btn, "Добавить в любимые");
    g_object_set_data_full(G_OBJECT(btn), "fav-uri",
                           uri ? g_strdup(uri) : NULL, g_free);

    g_signal_connect(btn, "clicked", G_CALLBACK(on_favorite_button_clicked), NULL);
    g_object_weak_ref(G_OBJECT(btn), on_fav_button_destroyed, NULL);
    g_ptr_array_add(favorite_buttons, btn);
    update_favorite_button(btn);
    return btn;
}

/* Общая часть фабрик «сердечек» сущностей (альбом/исполнитель). */
static GtkWidget *create_entity_favorite_button(int kind) {
    if (!favorite_buttons) favorite_buttons = g_ptr_array_new();
    ensure_favorite_css();

    GtkWidget *btn = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(btn), "denchiku-heart-outline-symbolic");
    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(btn, "Добавить в любимые");
    g_object_set_data(G_OBJECT(btn), "fav-kind", GINT_TO_POINTER(kind));

    g_signal_connect(btn, "clicked", G_CALLBACK(on_favorite_button_clicked), NULL);
    g_object_weak_ref(G_OBJECT(btn), on_fav_button_destroyed, NULL);
    g_ptr_array_add(favorite_buttons, btn);
    return btn;
}

GtkWidget *favorite_album_button_new(const char *album, const char *album_artist) {
    GtkWidget *btn = create_entity_favorite_button(FAV_ALBUM);
    g_object_set_data_full(G_OBJECT(btn), "fav-album",
                           g_strdup(album ? album : ""), g_free);
    g_object_set_data_full(G_OBJECT(btn), "fav-album-artist",
                           g_strdup(album_artist ? album_artist : ""), g_free);
    update_favorite_button(btn);
    return btn;
}

GtkWidget *favorite_artist_button_new(const char *artist) {
    GtkWidget *btn = create_entity_favorite_button(FAV_ARTIST);
    g_object_set_data_full(G_OBJECT(btn), "fav-artist",
                           g_strdup(artist ? artist : ""), g_free);
    update_favorite_button(btn);
    return btn;
}

void track_favorite_button_set_uri(GtkWidget *button, const char *uri) {
    if (!button) return;

    /* Ничего не делаем, если трек не изменился: обновление вызывается на каждое
     * обновление плеера. */
    const char *old_uri = g_object_get_data(G_OBJECT(button), "fav-uri");
    if (g_strcmp0(old_uri, uri) == 0) return;

    g_object_set_data_full(G_OBJECT(button), "fav-uri",
                           uri ? g_strdup(uri) : NULL, g_free);
    update_favorite_button(button);
}

void track_menu_refresh_favorites(void) {
    if (!favorite_buttons) return;
    for (guint i = 0; i < favorite_buttons->len; i++) {
        update_favorite_button(g_ptr_array_index(favorite_buttons, i));
    }
}

static gboolean detach_popup_idle(gpointer data) {
    GtkWidget *button = data;
    if (GTK_IS_MENU_BUTTON(button)) {
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), NULL);
    }
    g_object_unref(button);
    return G_SOURCE_REMOVE;
}

static void on_popup_closed(GtkPopover *popover, gpointer user_data) {
    (void)popover;
    g_idle_add(detach_popup_idle, g_object_ref(user_data));
}

void track_menu_detach_on_close(GtkWidget *popover, GtkWidget *button) {
    if (!popover || !button) return;
    g_signal_connect(popover, "closed", G_CALLBACK(on_popup_closed), button);
}

static void on_play_next(GSimpleAction *action, GVariant *param, gpointer user_data) {
    (void)action;
    (void)param;
    TrackMenuActions *a = user_data;
    player_insert_next(a->player, a->track);
}

static void on_append(GSimpleAction *action, GVariant *param, gpointer user_data) {
    (void)action;
    (void)param;
    TrackMenuActions *a = user_data;
    player_add_to_queue(a->player, a->track, FALSE);
}

static void on_remove_from_queue(GSimpleAction *action, GVariant *param, gpointer user_data) {
    (void)action;
    (void)param;
    TrackMenuActions *a = user_data;
    player_remove_from_queue(a->player, a->track);
}

static void on_open_album(GSimpleAction *action, GVariant *param, gpointer user_data) {
    (void)action;
    (void)param;
    TrackMenuActions *a = user_data;
    if (open_album_cb && a->track->album && *a->track->album) {
        open_album_cb(a->track->album, a->track->album_artist, open_album_data);
    }
}

static void on_go_to_artist(GSimpleAction *action, GVariant *param, gpointer user_data) {
    (void)action;
    (void)user_data;
    const char *artist = param ? g_variant_get_string(param, NULL) : NULL;
    if (open_artist_cb && artist && *artist) {
        open_artist_cb(artist, open_artist_data);
    }
}

static void on_show_info(GSimpleAction *action, GVariant *param, gpointer user_data) {
    (void)action;
    (void)param;
    TrackMenuActions *a = user_data;
    if (show_info_cb && a->track) {
        show_info_cb(a->track, show_info_data);
    }
}

static void on_favorite_change_state(GSimpleAction *action, GVariant *value,
                                     gpointer user_data) {
    TrackMenuActions *a = user_data;
    gboolean favorite = g_variant_get_boolean(value);

    /* Обработчик может перестроить список «Любимых» и уничтожить popover вместе
     * с этим действием — держим ссылку. */
    g_object_ref(action);
    if (a->track && playlist_toggle_favorite_cb) {
        playlist_toggle_favorite_cb(a->track->uri, favorite, playlist_cb_data);
    }
    g_simple_action_set_state(action, value);
    g_object_unref(action);
}

static void on_duplicate(GSimpleAction *action, GVariant *param, gpointer user_data) {
    (void)action;
    (void)param;
    TrackMenuActions *a = user_data;
    player_duplicate_in_queue(a->player, a->track);
}

/* Контекст пункта плейлиста: к какому треку и плейлисту относится нажатие.
 * Хранится в данных действия и освобождается вместе с ним. */
typedef struct {
    TrackMenuActions *actions;
    char *playlist_path;
} PlaylistItemCtx;

static void playlist_item_ctx_free(gpointer data) {
    PlaylistItemCtx *ctx = data;
    g_free(ctx->playlist_path);
    g_free(ctx);
}

/* Пункт плейлиста — состоятельное действие: галочка слева показывает, есть ли
 * трек в плейлисте. Смена состояния означает либо добавление (трека не было),
 * либо нажатие на уже отмеченный пункт; что именно делать, решает обработчик. */
static void on_toggle_playlist(GSimpleAction *action, GVariant *value,
                               gpointer user_data) {
    (void)value;
    (void)user_data;
    PlaylistItemCtx *ctx = g_object_get_data(G_OBJECT(action), "playlist-item-ctx");
    if (ctx && ctx->actions->track && ctx->playlist_path && playlist_toggle_cb) {
        playlist_toggle_cb(ctx->playlist_path, ctx->actions->track->uri,
                           playlist_cb_data);
    }
}

/* Собирает исполнителей трека для пункта «Перейти к исполнителю»: сначала
 * исполнитель альбома, затем исполнители трека в порядке тега. Разбиение тегов
 * берётся из настроек (track_split_artists), поэтому список совпадает с
 * группировкой раздела «Исполнители». Повторы (один и тот же исполнитель в обоих
 * тегах) отбрасываются. Возвращает GPtrArray<char*> (элементы освобождаются
 * g_free) либо NULL, если исполнителей нет. */
static GPtrArray *collect_artists(const Track *track) {
    char **album_artists = track_split_artists(track->album_artist);
    char **track_artists = track_split_artists(track->artist);

    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    char ***sources[] = { &album_artists, &track_artists };
    for (guint s = 0; s < G_N_ELEMENTS(sources); s++) {
        char **names = *sources[s];
        if (!names) continue;
        for (guint i = 0; names[i] != NULL; i++) {
            const char *name = names[i];
            if (!*name || g_hash_table_contains(seen, name)) continue;
            g_hash_table_add(seen, g_strdup(name));
            g_ptr_array_add(out, g_strdup(name));
        }
    }

    g_strfreev(album_artists);
    g_strfreev(track_artists);
    g_hash_table_destroy(seen);

    if (out->len == 0) {
        g_ptr_array_free(out, TRUE);
        return NULL;
    }
    return out;
}

/* Максимальная длина названия плейлиста в подменю «Добавить в плейлисты».
 * Пункты GMenu не умеют усекаться многоточием, поэтому длинные названия
 * растягивают popover по ширине — обрезаем подпись заранее. */
#define PLAYLIST_MENU_NAME_MAX 30

/* Обрезает UTF-8-название до max_chars символов, добавляя многоточие при
 * превышении. Возвращает новую строку (освобождать g_free). Обрезка по
 * символам, а не байтам, чтобы не порвать многобайтные буквы. */
static char *truncate_menu_name(const char *name, guint max_chars) {
    if (!name) return g_strdup("");
    if (g_utf8_strlen(name, -1) <= max_chars) return g_strdup(name);

    const char *end = g_utf8_offset_to_pointer(name, max_chars);
    char *head = g_strndup(name, end - name);
    char *out = g_strconcat(head, "…", NULL);
    g_free(head);
    return out;
}

GtkWidget *track_menu_popover_new(Track *track, TrackContext context, Player *player) {
    /* Состояние действий нужно уже при построении меню (пункты плейлистов
     * ссылаются на него), поэтому создаём его до модели. Владеет им popover —
     * освобождается вместе с меню. */
    TrackMenuActions *actions = g_new0(TrackMenuActions, 1);
    actions->track = track;
    actions->player = player;

    GSimpleActionGroup *group = g_simple_action_group_new();
    const GActionEntry entries[] = {
        { "play-next",         on_play_next,         NULL, NULL, NULL, {0} },
        { "append",            on_append,            NULL, NULL, NULL, {0} },
        { "duplicate",         on_duplicate,         NULL, NULL, NULL, {0} },
        { "remove-from-queue", on_remove_from_queue, NULL, NULL, NULL, {0} },
        { "open-album",        on_open_album,        NULL, NULL, NULL, {0} },
        { "go-to-artist",      on_go_to_artist,      "s",  NULL, NULL, {0} },
        { "show-info",         on_show_info,         NULL, NULL, NULL, {0} },
    };
    g_action_map_add_action_entries(G_ACTION_MAP(group), entries,
                                    G_N_ELEMENTS(entries), actions);

    GMenu *menu = g_menu_new();

    /* Удаление доступно только для элементов очереди и стоит в самом верху. */
    if (context == TRACK_CONTEXT_QUEUE) {
        GMenu *queue = g_menu_new();
        g_menu_append(queue, "Удалить из очереди", "track.remove-from-queue");
        g_menu_append_section(menu, NULL, G_MENU_MODEL(queue));
        g_object_unref(queue);
    }

    /* Навигация: «Открыть альбом» и «Перейти к исполнителю». На странице самого
     * альбома открывать его же незачем. */
    GMenu *nav = g_menu_new();
    if (context != TRACK_CONTEXT_ALBUM && track->album && *track->album) {
        g_menu_append(nav, "Открыть альбом", "track.open-album");
    }

    /* Исполнители: при одном исполнителе — обычный пункт, при нескольких —
     * подменю с выбором конкретного. */
    GPtrArray *artists = collect_artists(track);
    if (artists && artists->len == 1) {
        GMenuItem *item = g_menu_item_new("Перейти к исполнителю", NULL);
        g_menu_item_set_action_and_target(item, "track.go-to-artist", "s",
                                          g_ptr_array_index(artists, 0));
        g_menu_append_item(nav, item);
        g_object_unref(item);
    } else if (artists && artists->len > 1) {
        GMenu *sub = g_menu_new();
        for (guint i = 0; i < artists->len; i++) {
            const char *name = g_ptr_array_index(artists, i);
            GMenuItem *item = g_menu_item_new(name, NULL);
            g_menu_item_set_action_and_target(item, "track.go-to-artist", "s", name);
            g_menu_append_item(sub, item);
            g_object_unref(item);
        }
        g_menu_append_submenu(nav, "Перейти к исполнителю", G_MENU_MODEL(sub));
        g_object_unref(sub);
    }
    if (artists) g_ptr_array_unref(artists);

    if (g_menu_model_get_n_items(G_MENU_MODEL(nav)) > 0) {
        g_menu_append_section(menu, NULL, G_MENU_MODEL(nav));
    }
    g_object_unref(nav);

    /* В очереди «играть далее» и «добавить в очередь» бессмысленны: вместо них
     * трек можно продублировать сразу за собой. */
    GMenu *playback = g_menu_new();
    if (context == TRACK_CONTEXT_QUEUE) {
        g_menu_append(playback, "Дублировать", "track.duplicate");
    } else {
        g_menu_append(playback, "Играть далее", "track.play-next");
        g_menu_append(playback, "Добавить в очередь", "track.append");
    }
    g_menu_append_section(menu, NULL, G_MENU_MODEL(playback));
    g_object_unref(playback);

    /* Любимые треки и добавление в плейлисты. */
    gboolean favorite = (playlist_is_favorite_cb && track->uri)
        ? playlist_is_favorite_cb(track->uri, playlist_cb_data)
        : FALSE;

    GMenu *lists = g_menu_new();
    g_menu_append(lists, "Добавить в любимые", "track.favorite");

    GPtrArray *playlists = playlist_list_cb ? playlist_list_cb(playlist_cb_data) : NULL;
    GMenu *submenu = g_menu_new();
    guint playlist_items = 0;
    if (playlists && track->uri) {
        for (guint i = 0; i < playlists->len; i++) {
            Playlist *pl = g_ptr_array_index(playlists, i);
            /* «Любимые треки» здесь не показываем: за них отвечает отдельный
             * пункт «Добавить в любимые». */
            if (pl->favorite) continue;

            gboolean has = playlist_has_cb
                ? playlist_has_cb(pl->path, track->uri, playlist_cb_data)
                : FALSE;

            /* У каждого плейлиста своё состояние (галочка), поэтому и своё
             * действие: состояние хранится в действии, а не в модели меню. */
            char *action_name = g_strdup_printf("toggle-playlist-%u", i);
            GSimpleAction *act = g_simple_action_new_stateful(
                action_name, NULL, g_variant_new_boolean(has));

            PlaylistItemCtx *ctx = g_new0(PlaylistItemCtx, 1);
            ctx->actions = actions;
            ctx->playlist_path = g_strdup(pl->path);
            g_object_set_data_full(G_OBJECT(act), "playlist-item-ctx", ctx,
                                   playlist_item_ctx_free);
            g_signal_connect(act, "change-state",
                             G_CALLBACK(on_toggle_playlist), NULL);
            g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act));
            g_object_unref(act);

            char *label = truncate_menu_name(pl->name, PLAYLIST_MENU_NAME_MAX);
            char *detailed = g_strdup_printf("track.%s", action_name);
            GMenuItem *item = g_menu_item_new(label, detailed);
            g_free(detailed);
            g_free(label);
            g_free(action_name);
            g_menu_append_item(submenu, item);
            g_object_unref(item);
            playlist_items++;
        }
    }
    if (playlist_items > 0) {
        g_menu_append_submenu(lists, "Добавить в плейлисты", G_MENU_MODEL(submenu));
    }
    g_object_unref(submenu);

    g_menu_append_section(menu, NULL, G_MENU_MODEL(lists));
    g_object_unref(lists);

    /* Сведения о треке — отдельной секцией в самом низу. */
    GMenu *info = g_menu_new();
    g_menu_append(info, "Сведения", "track.show-info");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(info));
    g_object_unref(info);

    GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    g_object_unref(menu);

    g_object_set_data_full(G_OBJECT(popover), "track-menu-actions", actions, g_free);

    /* Состоятельное действие — в меню отображается галочкой. */
    GSimpleAction *favorite_action = g_simple_action_new_stateful(
        "favorite", NULL, g_variant_new_boolean(favorite));
    g_signal_connect(favorite_action, "change-state",
                     G_CALLBACK(on_favorite_change_state), actions);
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(favorite_action));
    g_object_unref(favorite_action);

    gtk_widget_insert_action_group(popover, "track", G_ACTION_GROUP(group));
    g_object_unref(group);

    return popover;
}

GtkWidget *track_menu_button_new(void) {
    GtkWidget *btn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(btn), "view-more-symbolic");
    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_set_tooltip_text(btn, "Меню трека");
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    return btn;
}

static void on_secondary_click(GtkGestureClick *gesture, int n_press,
                               double x, double y, gpointer user_data) {
    (void)n_press;
    (void)x;
    (void)y;
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    gtk_menu_button_popup(GTK_MENU_BUTTON(user_data));
}

static void on_long_press(GtkGestureLongPress *gesture, double x, double y,
                          gpointer user_data) {
    (void)x;
    (void)y;
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    gtk_menu_button_popup(GTK_MENU_BUTTON(user_data));
}

void track_menu_add_triggers(GtkWidget *target, GtkWidget *menu_button) {
    if (!target || !menu_button) return;

    /* Правый клик: перехватываем в фазе capture, чтобы клик не доходил до
     * активации строки. */
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_secondary_click), menu_button);
    gtk_widget_add_controller(target, GTK_EVENT_CONTROLLER(click));

    /* Долгое нажатие основной кнопкой: захватываем последовательность, чтобы
     * после показа меню строка не активировалась. */
    GtkGesture *long_press = gtk_gesture_long_press_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(long_press), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(long_press), GTK_PHASE_CAPTURE);
    g_signal_connect(long_press, "pressed", G_CALLBACK(on_long_press), menu_button);
    gtk_widget_add_controller(target, GTK_EVENT_CONTROLLER(long_press));
}

/* Контекст строки для ленивого построения меню: popover создаётся только при
 * первом показе, а не для каждой строки заранее. */
typedef struct {
    Track *track;
    Player *player;
    TrackContext context;
} TrackMenuRow;

static void create_row_popup(GtkMenuButton *button, gpointer user_data) {
    TrackMenuRow *rm = user_data;
    GtkWidget *popover = track_menu_popover_new(rm->track, rm->context, rm->player);
    gtk_menu_button_set_popover(button, popover);
    track_menu_detach_on_close(popover, GTK_WIDGET(button));
}

/* Общее для обоих вариантов строк: кнопка «…» с ленивым построением меню и
 * триггеры правого клика/долгого нажатия. Держит последовательность создания в
 * одном месте, чтобы варианты строк не расходились. */
static GtkWidget *track_menu_make_button(GtkWidget *row,
                                         GtkMenuButtonCreatePopupFunc create_popup,
                                         gpointer popup_data,
                                         GDestroyNotify popup_data_destroy) {
    GtkWidget *btn = track_menu_button_new();
    gtk_menu_button_set_create_popup_func(GTK_MENU_BUTTON(btn),
                                          create_popup, popup_data,
                                          popup_data_destroy);
    track_menu_add_triggers(row, btn);
    return btn;
}

void track_menu_attach(GtkWidget *row, Track *track, TrackContext context, Player *player) {
    if (!row || !track) return;

    /* Сердечко стоит слева от кнопки меню. */
    adw_action_row_add_suffix(ADW_ACTION_ROW(row),
                              track_favorite_button_new(track->uri));

    TrackMenuRow *rm = g_new0(TrackMenuRow, 1);
    rm->track = track;
    rm->player = player;
    rm->context = context;

    adw_action_row_add_suffix(ADW_ACTION_ROW(row),
                              track_menu_make_button(row, create_row_popup, rm, g_free));
}

/* ===== меню для виртуализированных списков =====
 * Строки GtkListView переиспользуются, поэтому кнопки создаются один раз, а на
 * привязке меняется только текущий трек. Состояние строки хранится рядом с самой
 * строкой и умирает вместе с ней. */
typedef struct {
    Track *track;
    Player *player;
    TrackContext context;
    GtkWidget *fav_btn;
} TrackRowMenu;

static void create_row_menu_popup(GtkMenuButton *button, gpointer user_data) {
    TrackRowMenu *m = user_data;
    if (!m || !m->track) return;
    GtkWidget *popover = track_menu_popover_new(m->track, m->context, m->player);
    gtk_menu_button_set_popover(button, popover);
    track_menu_detach_on_close(popover, GTK_WIDGET(button));
}

GtkWidget *track_menu_attach_row(GtkWidget *row, GtkWidget *suffix_box,
                                 TrackContext context, Player *player) {
    if (!row || !suffix_box) return NULL;

    TrackRowMenu *m = g_new0(TrackRowMenu, 1);
    m->player = player;
    m->context = context;

    /* Сердечко стоит слева от кнопки меню. */
    m->fav_btn = track_favorite_button_new(NULL);
    gtk_box_append(GTK_BOX(suffix_box), m->fav_btn);

    GtkWidget *menu_btn = track_menu_make_button(row, create_row_menu_popup, m, NULL);
    gtk_box_append(GTK_BOX(suffix_box), menu_btn);

    g_object_set_data_full(G_OBJECT(row), "track-row-menu", m, g_free);
    return menu_btn;
}

void track_menu_row_set_track(GtkWidget *row, Track *track) {
    if (!row) return;
    TrackRowMenu *m = g_object_get_data(G_OBJECT(row), "track-row-menu");
    if (!m) return;
    m->track = track;
    track_favorite_button_set_uri(m->fav_btn, track ? track->uri : NULL);
}
