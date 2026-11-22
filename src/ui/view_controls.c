#include "view_controls.h"

struct _ViewControls {
    AdwSplitButton *split_btn;
    GSimpleActionGroup *action_group;
    GSimpleAction *zoom_act;
    GSimpleAction *sort_act;
    GSimpleAction *dir_act;

    ViewMode mode;
    ViewZoom zoom;
    ViewSortField sort_field;
    gboolean sort_descending;

    ViewControlsChangedCb on_changed;
    gpointer user_data;
};

static void notify_changed(ViewControls *vc) {
    if (vc->on_changed) {
        vc->on_changed(vc, vc->user_data);
    }
}

static void update_split_button_icon(ViewControls *vc) {
    if (vc->mode == VIEW_MODE_GRID) {
        adw_split_button_set_icon_name(vc->split_btn, "view-list-symbolic");
        gtk_widget_set_tooltip_text(GTK_WIDGET(vc->split_btn), "Показать списком");
    } else {
        adw_split_button_set_icon_name(vc->split_btn, "view-grid-symbolic");
        gtk_widget_set_tooltip_text(GTK_WIDGET(vc->split_btn), "Показать сеткой");
    }
}

/* Клик по левой части AdwSplitButton — быстрое переключение сетка/список */
static void on_split_button_clicked(AdwSplitButton *btn, gpointer user_data) {
    (void)btn;
    ViewControls *vc = user_data;
    vc->mode = (vc->mode == VIEW_MODE_GRID) ? VIEW_MODE_LIST : VIEW_MODE_GRID;
    update_split_button_icon(vc);
    notify_changed(vc);
}

/* Выбор размера элементов (Маленький / Средний / Крупный) */
static void on_zoom_action(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    ViewControls *vc = user_data;
    const char *val = g_variant_get_string(parameter, NULL);
    g_simple_action_set_state(action, parameter);

    if (g_strcmp0(val, "small") == 0) {
        vc->zoom = VIEW_ZOOM_SMALL;
    } else if (g_strcmp0(val, "large") == 0) {
        vc->zoom = VIEW_ZOOM_LARGE;
    } else {
        vc->zoom = VIEW_ZOOM_MEDIUM;
    }

    notify_changed(vc);
}

/* Выбор поля сортировки */
static void on_sort_field_action(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    ViewControls *vc = user_data;
    const char *val = g_variant_get_string(parameter, NULL);
    g_simple_action_set_state(action, parameter);

    if (g_strcmp0(val, "artist") == 0) {
        vc->sort_field = VIEW_SORT_ARTIST;
    } else if (g_strcmp0(val, "year") == 0) {
        vc->sort_field = VIEW_SORT_YEAR;
    } else {
        vc->sort_field = VIEW_SORT_TITLE;
    }

    notify_changed(vc);
}

/* Переключение направления сортировки (По возрастанию / По убыванию) */
static void on_sort_dir_action(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    ViewControls *vc = user_data;
    const char *val = g_variant_get_string(parameter, NULL);
    g_simple_action_set_state(action, parameter);

    vc->sort_descending = (g_strcmp0(val, "desc") == 0);
    notify_changed(vc);
}

ViewControls *view_controls_new(gboolean show_year_sort,
                                ViewControlsChangedCb on_changed,
                                gpointer user_data) {
    ViewControls *vc = g_new0(ViewControls, 1);
    vc->mode = VIEW_MODE_GRID;
    vc->zoom = VIEW_ZOOM_MEDIUM;
    vc->sort_field = VIEW_SORT_TITLE;
    vc->sort_descending = FALSE;
    vc->on_changed = on_changed;
    vc->user_data = user_data;

    /* Настраиваем GActionGroup в стиле Nautilus */
    vc->action_group = g_simple_action_group_new();

    GSimpleAction *zoom_act = g_simple_action_new_stateful(
        "zoom", G_VARIANT_TYPE_STRING, g_variant_new_string("medium"));
    g_signal_connect(zoom_act, "activate", G_CALLBACK(on_zoom_action), vc);
    g_action_map_add_action(G_ACTION_MAP(vc->action_group), G_ACTION(zoom_act));
    vc->zoom_act = zoom_act;
    g_object_unref(zoom_act);

    GSimpleAction *sort_act = g_simple_action_new_stateful(
        "sort-by", G_VARIANT_TYPE_STRING, g_variant_new_string("title"));
    g_signal_connect(sort_act, "activate", G_CALLBACK(on_sort_field_action), vc);
    g_action_map_add_action(G_ACTION_MAP(vc->action_group), G_ACTION(sort_act));
    vc->sort_act = sort_act;
    g_object_unref(sort_act);

    GSimpleAction *dir_act = g_simple_action_new_stateful(
        "sort-dir", G_VARIANT_TYPE_STRING, g_variant_new_string("asc"));
    g_signal_connect(dir_act, "activate", G_CALLBACK(on_sort_dir_action), vc);
    g_action_map_add_action(G_ACTION_MAP(vc->action_group), G_ACTION(dir_act));
    vc->dir_act = dir_act;
    g_object_unref(dir_act);

    /* Строим модель меню GMenu (как в Nautilus) */
    GMenu *menu = g_menu_new();

    /* Секция 1: Размер элементов */
    GMenu *zoom_section = g_menu_new();
    g_menu_append(zoom_section, "Маленькие значки", "view.zoom::small");
    g_menu_append(zoom_section, "Средние значки",   "view.zoom::medium");
    g_menu_append(zoom_section, "Крупные значки",   "view.zoom::large");
    g_menu_append_section(menu, "Размер элементов", G_MENU_MODEL(zoom_section));
    g_object_unref(zoom_section);

    /* Секция 2: Поле сортировки */
    GMenu *sort_section = g_menu_new();
    g_menu_append(sort_section, "По названию",    "view.sort-by::title");
    g_menu_append(sort_section, "По исполнителю", "view.sort-by::artist");
    if (show_year_sort) {
        g_menu_append(sort_section, "По году",    "view.sort-by::year");
    }
    g_menu_append_section(menu, "Сортировка", G_MENU_MODEL(sort_section));
    g_object_unref(sort_section);

    /* Секция 3: Порядок сортировки */
    GMenu *dir_section = g_menu_new();
    g_menu_append(dir_section, "По возрастанию (А–Я)", "view.sort-dir::asc");
    g_menu_append(dir_section, "По убыванию (Я–А)",    "view.sort-dir::desc");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(dir_section));
    g_object_unref(dir_section);

    /* Создаём AdwSplitButton */
    vc->split_btn = ADW_SPLIT_BUTTON(adw_split_button_new());
    gtk_widget_insert_action_group(GTK_WIDGET(vc->split_btn), "view",
                                   G_ACTION_GROUP(vc->action_group));
    adw_split_button_set_menu_model(vc->split_btn, G_MENU_MODEL(menu));
    adw_split_button_set_dropdown_tooltip(vc->split_btn, "Параметры вида");
    g_object_unref(menu);

    update_split_button_icon(vc);
    g_signal_connect(vc->split_btn, "clicked", G_CALLBACK(on_split_button_clicked), vc);

    return vc;
}

GtkWidget *view_controls_get_widget(ViewControls *vc) {
    return GTK_WIDGET(vc->split_btn);
}

ViewMode view_controls_get_mode(ViewControls *vc) {
    return vc->mode;
}

ViewZoom view_controls_get_zoom(ViewControls *vc) {
    return vc->zoom;
}

ViewSortField view_controls_get_sort_field(ViewControls *vc) {
    return vc->sort_field;
}

gboolean view_controls_get_sort_descending(ViewControls *vc) {
    return vc->sort_descending;
}

void view_controls_set_sort(ViewControls *vc, ViewSortField field, gboolean descending) {
    if (vc->sort_field == field && vc->sort_descending == descending) return;

    vc->sort_field = field;
    vc->sort_descending = descending;

    const char *field_val = "title";
    if (field == VIEW_SORT_ARTIST) field_val = "artist";
    else if (field == VIEW_SORT_YEAR) field_val = "year";

    /* Держим состояние пунктов меню в соответствии с выбранной сортировкой. */
    if (vc->sort_act) g_simple_action_set_state(vc->sort_act, g_variant_new_string(field_val));
    if (vc->dir_act) g_simple_action_set_state(vc->dir_act,
                                               g_variant_new_string(descending ? "desc" : "asc"));

    notify_changed(vc);
}

void view_controls_set_mode(ViewControls *vc, ViewMode mode) {
    if (vc->mode == mode) return;
    vc->mode = mode;
    update_split_button_icon(vc);
    notify_changed(vc);
}

void view_controls_set_zoom(ViewControls *vc, ViewZoom zoom) {
    if (vc->zoom == zoom) return;
    vc->zoom = zoom;

    const char *val = "medium";
    if (zoom == VIEW_ZOOM_SMALL) val = "small";
    else if (zoom == VIEW_ZOOM_LARGE) val = "large";
    if (vc->zoom_act) g_simple_action_set_state(vc->zoom_act, g_variant_new_string(val));

    notify_changed(vc);
}

int view_controls_get_grid_cover_size(ViewControls *vc) {
    switch (vc->zoom) {
        case VIEW_ZOOM_SMALL: return 104;
        case VIEW_ZOOM_LARGE: return 196;
        case VIEW_ZOOM_MEDIUM:
        default:              return 148;
    }
}

int view_controls_get_list_cover_size(ViewControls *vc) {
    switch (vc->zoom) {
        case VIEW_ZOOM_SMALL: return 32;
        case VIEW_ZOOM_LARGE: return 64;
        case VIEW_ZOOM_MEDIUM:
        default:              return 44;
    }
}

void view_controls_free(ViewControls *vc) {
    if (!vc) return;
    if (vc->action_group) g_object_unref(vc->action_group);
    g_free(vc);
}
