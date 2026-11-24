#include "settings_dialog.h"
#include "settings.h"
#include "tag_editor.h"

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

        /* Заголовок разбирается как разметка, поэтому экранируем спецсимволы:
         * «&» в разделителе иначе вызывает ошибку парсинга markup. */
        char *title = g_markup_escape_text(token, -1);
        AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        g_free(title);

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

    /* Сводка содержит разделители (в т.ч. «&»), поэтому подзаголовок задаётся
     * как обычный текст: разметка по умолчанию сломалась бы на «&». */
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(list->expander), FALSE);
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
    /* У AdwEntryRow нет subtitle/placeholder-API, а заголовок сам служит
     * подсказкой; отдельный subtitle был недопустимым приведением к ActionRow. */
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

/* ===== внешний редактор тегов ===== */

typedef struct {
    GtkWindow *parent;
    AdwComboRow *combo;
    GPtrArray *apps;      /* GAppInfo* — идут первыми в списке */
    gboolean updating;    /* подавляет реакцию на программную установку выбора */
    guint none_index;     /* индекс пункта «Не выбрано» (всегда последний) */
    char *custom_value;   /* значение, показанное отдельным (не из apps) пунктом */
    guint idle_id;        /* отложенная перестройка модели */
} TagEditorSection;

static void tag_editor_section_free(gpointer data) {
    TagEditorSection *sec = data;
    if (sec->idle_id) g_source_remove(sec->idle_id);
    g_free(sec->custom_value);
    g_ptr_array_unref(sec->apps);
    g_free(sec);
}

/* Строит модель списка и выставляет текущий выбор: сначала приложения, затем
 * необязательный «пользовательский» пункт (выбранный вручную путь/команда),
 * последним — «Не выбрано». */
static void tag_editor_rebuild(TagEditorSection *sec) {
    const char *editor = settings_tag_editor();
    guint selected = GTK_INVALID_LIST_POSITION;

    g_clear_pointer(&sec->custom_value, g_free);

    GtkStringList *model = gtk_string_list_new(NULL);
    for (guint i = 0; i < sec->apps->len; i++) {
        GAppInfo *app = g_ptr_array_index(sec->apps, i);
        const char *name = g_app_info_get_display_name(app);
        gtk_string_list_append(model, name ? name : g_app_info_get_id(app));
        if (editor && *editor && g_strcmp0(g_app_info_get_id(app), editor) == 0)
            selected = i;
    }

    /* Значение, не совпавшее с известными приложениями (выбрано вручную),
     * показываем отдельным пунктом перед «Не выбрано». */
    if (editor && *editor && selected == GTK_INVALID_LIST_POSITION) {
        const char *slash = strrchr(editor, '/');
        gtk_string_list_append(model, slash ? slash + 1 : editor);
        sec->custom_value = g_strdup(editor);
        selected = sec->apps->len;
    }

    gtk_string_list_append(model, "Не выбрано");
    sec->none_index = sec->apps->len + (sec->custom_value ? 1 : 0);
    if (!editor || !*editor)
        selected = sec->none_index;

    sec->updating = TRUE;
    adw_combo_row_set_model(sec->combo, G_LIST_MODEL(model));
    adw_combo_row_set_selected(sec->combo, selected);
    sec->updating = FALSE;
    g_object_unref(model);
}

static gboolean tag_editor_rebuild_idle(gpointer data) {
    TagEditorSection *sec = data;
    sec->idle_id = 0;
    tag_editor_rebuild(sec);
    return G_SOURCE_REMOVE;
}

static void on_tag_editor_selected(GObject *obj, GParamSpec *pspec, gpointer data) {
    (void)pspec;
    TagEditorSection *sec = data;
    if (sec->updating) return;

    guint idx = adw_combo_row_get_selected(ADW_COMBO_ROW(obj));
    if (idx == sec->none_index) {
        settings_set_tag_editor("");
    } else if (idx < sec->apps->len) {
        GAppInfo *app = g_ptr_array_index(sec->apps, idx);
        settings_set_tag_editor(g_app_info_get_id(app));
    } else if (idx < sec->none_index && sec->custom_value) {
        settings_set_tag_editor(sec->custom_value);
    } else {
        return;
    }

    /* Список мог измениться (появление/исчезновение «пользовательского» пункта),
     * поэтому перестраиваем модель — отложенно, чтобы не менять её прямо из
     * обработчика сигнала. */
    if (!sec->idle_id) sec->idle_id = g_idle_add(tag_editor_rebuild_idle, sec);
}

static void on_pick_app_finish(GObject *source, GAsyncResult *res, gpointer data) {
    TagEditorSection *sec = data;
    GError *err = NULL;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), res, &err);
    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            settings_set_tag_editor(path);
            tag_editor_rebuild(sec);
        }
        g_free(path);
        g_object_unref(file);
    }
    if (err) g_error_free(err);
}

static void on_pick_app_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    TagEditorSection *sec = data;
    GtkFileDialog *fd = gtk_file_dialog_new();
    gtk_file_dialog_set_title(fd, "Выбор приложения для тегов");
    gtk_file_dialog_open(fd, sec->parent, NULL, on_pick_app_finish, sec);
    g_object_unref(fd);
}

/* Секция «Теги»: выбор приложения, которым открываются файлы для правки тегов.
 * Denchiku сам теги не меняет. */
static void build_tag_editor_section(AdwPreferencesPage *page, GtkWindow *parent) {
    TagEditorSection *sec = g_new0(TagEditorSection, 1);
    sec->parent = parent;
    sec->apps = tag_editor_candidate_apps();

    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, "Теги");
    adw_preferences_group_set_description(group,
        "Приложение, которым открываются треки для правки тегов (например, "
        "Ear Tag, EasyTAG, Kid3).");

    sec->combo = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(sec->combo), "Редактор тегов");
    g_signal_connect(sec->combo, "notify::selected",
                     G_CALLBACK(on_tag_editor_selected), sec);
    adw_preferences_group_add(group, GTK_WIDGET(sec->combo));

    AdwButtonRow *pick = ADW_BUTTON_ROW(adw_button_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(pick), "Выбрать другое приложение…");
    g_signal_connect(pick, "activated", G_CALLBACK(on_pick_app_clicked), sec);
    adw_preferences_group_add(group, GTK_WIDGET(pick));

    adw_preferences_page_add(page, group);

    tag_editor_rebuild(sec);

    g_object_set_data_full(G_OBJECT(page), "tag-editor-section", sec,
                           tag_editor_section_free);
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

    /* ------------------- Внешний редактор тегов ------------------ */
    build_tag_editor_section(page, parent);

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
