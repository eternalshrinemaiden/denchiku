#include "settings_dialog.h"
#include "settings.h"

typedef struct {
    gboolean *field;
    SettingsCategory category;
    SettingsChangedCallback on_changed;
    gpointer user_data;
} ToggleCtx;

typedef struct _SeparatorList {
    gboolean artists;
    AdwExpanderRow *expander;
    GPtrArray *rows; /* GtkWidget* строк-разделителей (заимствованы у expander) */
    SettingsChangedCallback on_changed;
    gpointer user_data;
} SeparatorList;

typedef struct {
    SeparatorList *list;
    char *token;
} RemoveCtx;

static void free_closure_ctx(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

static void free_remove_ctx(gpointer data, GClosure *closure) {
    (void)closure;
    RemoveCtx *ctx = data;
    g_free(ctx->token);
    g_free(ctx);
}

static void separator_list_free(gpointer data) {
    SeparatorList *list = data;
    g_ptr_array_free(list->rows, TRUE);
    g_free(list);
}

static SettingsCategory category_of(const SeparatorList *list) {
    return list->artists ? SETTINGS_CATEGORY_ARTISTS : SETTINGS_CATEGORY_GENRES;
}

static void apply_change(SettingsCategory category,
                         SettingsChangedCallback on_changed, gpointer user_data) {
    settings_rebuild();
    settings_save();
    if (on_changed) on_changed(category, user_data);
}

static void separator_list_refresh(SeparatorList *list);

static void on_remove_separator(GtkButton *btn, gpointer data) {
    /* Держим кнопку живой: refresh ниже может удалить её вместе со строкой. */
    g_object_ref(btn);

    RemoveCtx *ctx = data;
    SeparatorList *list = ctx->list;
    tag_split_remove_separator(list->artists, ctx->token);
    apply_change(category_of(list), list->on_changed, list->user_data);
    separator_list_refresh(list);

    g_object_unref(btn);
}

static void on_add_separator(AdwEntryRow *row, gpointer data) {
    SeparatorList *list = data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(row));
    char *token = g_strstrip(g_strdup(text ? text : ""));
    if (*token) {
        tag_split_add_separator(list->artists, token);
        apply_change(category_of(list), list->on_changed, list->user_data);
        separator_list_refresh(list);
    }
    g_free(token);
    gtk_editable_set_text(GTK_EDITABLE(row), "");
}

static void on_switch_notify(GObject *obj, GParamSpec *pspec, gpointer data) {
    (void)pspec;
    ToggleCtx *ctx = data;
    *ctx->field = adw_switch_row_get_active(ADW_SWITCH_ROW(obj));
    apply_change(ctx->category, ctx->on_changed, ctx->user_data);
}

static AdwSwitchRow *make_switch(const char *title, gboolean *field,
                                 SettingsCategory category,
                                 SettingsChangedCallback on_changed,
                                 gpointer user_data) {
    AdwSwitchRow *row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_switch_row_set_active(row, *field);

    ToggleCtx *ctx = g_new0(ToggleCtx, 1);
    ctx->field = field;
    ctx->category = category;
    ctx->on_changed = on_changed;
    ctx->user_data = user_data;
    g_signal_connect_data(row, "notify::active", G_CALLBACK(on_switch_notify),
                          ctx, free_closure_ctx, 0);
    return row;
}

/* Перестраивает строки-разделители внутри expander из текущих настроек. */
static void separator_list_refresh(SeparatorList *list) {
    for (guint i = 0; i < list->rows->len; i++)
        adw_expander_row_remove(list->expander, g_ptr_array_index(list->rows, i));
    g_ptr_array_set_size(list->rows, 0);

    GPtrArray *separators = tag_split_separators(list->artists);
    GString *summary = g_string_new(NULL);
    for (guint i = 0; i < separators->len; i++) {
        const char *token = g_ptr_array_index(separators, i);
        if (i > 0) g_string_append_c(summary, ' ');
        g_string_append(summary, token);

        AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), token);

        GtkWidget *remove = gtk_button_new_from_icon_name("window-close-symbolic");
        gtk_widget_add_css_class(remove, "flat");
        gtk_widget_set_tooltip_text(remove, "Удалить разделитель");
        RemoveCtx *ctx = g_new0(RemoveCtx, 1);
        ctx->list = list;
        ctx->token = g_strdup(token);
        g_signal_connect_data(remove, "clicked", G_CALLBACK(on_remove_separator),
                              ctx, free_remove_ctx, 0);
        adw_action_row_add_suffix(row, remove);

        adw_expander_row_add_row(list->expander, GTK_WIDGET(row));
        g_ptr_array_add(list->rows, GTK_WIDGET(row));
    }

    adw_expander_row_set_subtitle(list->expander,
        separators->len > 0 ? summary->str : "Разделителей нет");
    g_string_free(summary, TRUE);
}

/* Создаёт скрываемый список разделителей и поле для добавления. */
static void build_separator_section(AdwPreferencesPage *page, const char *owner_key,
                                    AdwPreferencesGroup *group, AdwSwitchRow *master,
                                    gboolean artists,
                                    SettingsChangedCallback on_changed, gpointer user_data) {
    SeparatorList *list = g_new0(SeparatorList, 1);
    list->artists = artists;
    list->rows = g_ptr_array_new();
    list->on_changed = on_changed;
    list->user_data = user_data;

    AdwExpanderRow *expander = ADW_EXPANDER_ROW(adw_expander_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(expander), "Разделители");
    list->expander = expander;
    adw_preferences_group_add(group, GTK_WIDGET(expander));
    g_object_bind_property(master, "active", expander, "sensitive", G_BINDING_SYNC_CREATE);

    AdwEntryRow *entry = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(entry), "Добавить разделитель");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(entry),
                                "Символ или слово, затем Enter");
    adw_entry_row_set_show_apply_button(entry, TRUE);
    g_signal_connect_data(entry, "apply", G_CALLBACK(on_add_separator), list, NULL, 0);
    g_signal_connect_data(entry, "entry-activated", G_CALLBACK(on_add_separator),
                          list, NULL, 0);
    adw_preferences_group_add(group, GTK_WIDGET(entry));
    g_object_bind_property(master, "active", entry, "sensitive", G_BINDING_SYNC_CREATE);

    /* Владельцем списка делаем страницу: expander/entry хранят лишь указатель. */
    g_object_set_data_full(G_OBJECT(page), owner_key, list, separator_list_free);

    separator_list_refresh(list);
}

static void settings_dialog_build(GtkWindow *parent,
                                  SettingsChangedCallback on_changed,
                                  gpointer user_data) {
    TagSplitSettings *s = tag_split_settings();

    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());

    /* ------------------------ Исполнители ------------------------ */
    AdwPreferencesGroup *ag = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(ag, "Исполнители");
    adw_preferences_group_set_description(ag,
        "Разбивать тег исполнителя или исполнителя альбома на нескольких "
        "исполнителей. Например, «A feat. B» попадёт и к «A», и к «B».");

    AdwSwitchRow *a_master = make_switch("Разбивать по разделителям",
                                         &s->artists.enabled,
                                         SETTINGS_CATEGORY_ARTISTS, on_changed, user_data);
    adw_preferences_group_add(ag, GTK_WIDGET(a_master));
    build_separator_section(page, "sep-list-artists", ag, a_master, TRUE,
                            on_changed, user_data);
    adw_preferences_page_add(page, ag);

    /* -------------------------- Жанры ---------------------------- */
    AdwPreferencesGroup *gg = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(gg, "Жанры");
    adw_preferences_group_set_description(gg,
        "Разбивать тег жанра на несколько жанров. Например, «Rock, Pop» "
        "попадёт и к «Rock», и к «Pop».");

    AdwSwitchRow *g_master = make_switch("Разбивать по разделителям",
                                         &s->genres.enabled,
                                         SETTINGS_CATEGORY_GENRES, on_changed, user_data);
    adw_preferences_group_add(gg, GTK_WIDGET(g_master));
    build_separator_section(page, "sep-list-genres", gg, g_master, FALSE,
                            on_changed, user_data);
    adw_preferences_page_add(page, gg);

    AdwHeaderBar *hb = ADW_HEADER_BAR(adw_header_bar_new());
    adw_header_bar_set_title_widget(hb, GTK_WIDGET(adw_window_title_new("Настройки", NULL)));

    AdwToolbarView *tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(tv, GTK_WIDGET(hb));
    adw_toolbar_view_set_content(tv, GTK_WIDGET(page));

    AdwDialog *dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, "Настройки");
    adw_dialog_set_content_width(dialog, 460);
    adw_dialog_set_child(dialog, GTK_WIDGET(tv));
    adw_dialog_present(dialog, GTK_WIDGET(parent));
}

void settings_dialog_present(GtkWindow *parent, SettingsChangedCallback on_changed,
                             gpointer user_data) {
    settings_dialog_build(parent, on_changed, user_data);
}
