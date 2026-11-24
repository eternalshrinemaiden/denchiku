#include "cache.h"
#include "player.h"
#include "scanner.h"
#include "settings.h"
#include "stats.h"
#include "ui/window.h"
#include "watcher.h"
#include <adwaita.h>
#include <gst/gst.h>

typedef struct {
    Player *player;
    GtkWidget *window;
    Watcher *watcher;
} AppContext;

/* Батч обновлений от watcher'а: снимок медиатеки приходит целиком, окно само
 * решает, нужно ли перестраивать виды. */
static void on_watcher_changed(GPtrArray *tracks, gpointer user_data) {
    main_window_set_tracks(GTK_WIDGET(user_data), tracks);
}

static void on_initial_scan_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    (void)source;
    AppContext *ctx = user_data;
    GPtrArray *fresh_tracks = scanner_scan_finish(res, NULL);

    if (fresh_tracks) {
        main_window_set_tracks(ctx->window, fresh_tracks);
        g_ptr_array_unref(fresh_tracks);
    }
    main_window_set_scanning(ctx->window, FALSE);

    /* Живое слежение включаем после первичного скана: кэш уже заполнен, а
     * файловые события начнут приходить в фоне. */
    if (!ctx->watcher) {
        ctx->watcher = watcher_new();
        watcher_set_changed_cb(ctx->watcher, on_watcher_changed, ctx->window);
        const char *music_dir = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
        if (!music_dir) music_dir = g_get_home_dir();
        watcher_start(ctx->watcher, music_dir);
    }
}

static void on_quit_action(GSimpleAction *action, GVariant *param, gpointer user_data) {
    (void)action;
    (void)param;
    /* Закрываем окно, чтобы сработал обработчик close-request и состояние
     * сессии сохранилось; если окон нет — выходим напрямую. */
    GtkApplication *app = GTK_APPLICATION(user_data);
    GList *windows = gtk_application_get_windows(app);
    if (windows) {
        gtk_window_close(GTK_WINDOW(windows->data));
    } else {
        g_application_quit(G_APPLICATION(app));
    }
}

typedef struct {
    GtkWidget *window;
    GPtrArray *tracks;
} RestoreContext;

static gboolean restore_session_idle(gpointer data) {
    RestoreContext *rc = data;
    main_window_restore_session(rc->window, rc->tracks);
    g_ptr_array_unref(rc->tracks);
    g_free(rc);
    return G_SOURCE_REMOVE;
}

static void on_activate(AdwApplication *app, gpointer user_data) {
    AppContext *ctx = user_data;

    /* Иконки-сердечки (контур/закрашенное) лежат в ресурсах приложения. */
    gtk_icon_theme_add_resource_path(
        gtk_icon_theme_get_for_display(gdk_display_get_default()),
        "/org/svyata/denchiku/icons");

    ctx->window = main_window_new(app, ctx->player);
    gtk_window_present(GTK_WINDOW(ctx->window));

    GPtrArray *cached = cache_load_tracks();
    if (cached) {
        main_window_set_tracks(ctx->window, cached);
        /* Восстанавливаем сессию после первой раскладки окна: наполнение
         * виртуализированной очереди до первого выделения размеров вызывало
         * переполнение минимумов при первичной раскладке. */
        RestoreContext *rc = g_new0(RestoreContext, 1);
        rc->window = ctx->window;
        rc->tracks = g_ptr_array_ref(cached);
        g_idle_add(restore_session_idle, rc);
        g_ptr_array_unref(cached);
    }

    main_window_set_scanning(ctx->window, TRUE);
    scanner_scan_async(NULL, on_initial_scan_done, ctx);
}

int main(int argc, char **argv) {
    gst_init(&argc, &argv);

    AppContext ctx = {0};
    ctx.player = player_new();

    /* Настройки tag-splitting нужны до построения группировки медиатеки. */
    settings_load();
    /* Статистика прослушиваний — до запуска воспроизведения. */
    stats_load();

    AdwApplication *app = adw_application_new("org.svyata.denchiku", G_APPLICATION_DEFAULT_FLAGS);

    /* Действие выхода по Ctrl+Q */
    const GActionEntry app_entries[] = {
        { "quit", on_quit_action, NULL, NULL, NULL, {0} }
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), app_entries, G_N_ELEMENTS(app_entries), app);

    /* Назначение комбинаций клавиш */
    const char *refresh_accels[]   = { "F5", "<Primary>r", NULL };
    const char *settings_accels[]  = { "<Primary>comma", NULL };
    const char *shortcuts_accels[] = { "<Primary>question", NULL };
    const char *quit_accels[]      = { "<Primary>q", NULL };

    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.refresh", refresh_accels);
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.settings", settings_accels);
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.shortcuts", shortcuts_accels);
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", quit_accels);

    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &ctx);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    if (ctx.watcher) watcher_free(ctx.watcher);
    cache_flush();
    g_object_unref(app);
    player_free(ctx.player);

    return status;
}