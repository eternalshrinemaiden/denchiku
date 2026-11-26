#include "media_browser.h"
#include "ui_helpers.h"

struct _MediaBrowser {
    GListStore *store;
    ViewControls *controls;
    MediaBrowserConfig cfg;
    MediaItemActivatedFn on_activate;
    gpointer user_data;

    ViewZoom last_zoom;

    GtkSortListModel *sort_model;

    GtkStack *stack;
    GtkGridView *grid_view;
    GtkColumnView *col_view;
    GtkColumnViewColumn *col_cover;
    GtkColumnViewColumn *col_title;
    GtkColumnViewColumn *col_sub;
    GtkColumnViewColumn *col_year;

    gboolean syncing_sort;
};

/* Сравнение по заданному полю с тай-брейком по названию (как раньше). */
static int compare_by_field(MediaBrowser *mb, ViewSortField field,
                            GObject *i1, GObject *i2) {
    int res = 0;

    if (field == VIEW_SORT_ARTIST && mb->cfg.get_subtitle) {
        const char *s1 = mb->cfg.get_subtitle(i1);
        const char *s2 = mb->cfg.get_subtitle(i2);
        res = g_utf8_collate(s1 ? s1 : "", s2 ? s2 : "");
    } else if (field == VIEW_SORT_YEAR && mb->cfg.get_year) {
        guint y1 = mb->cfg.get_year(i1);
        guint y2 = mb->cfg.get_year(i2);
        if (y1 != y2) {
            if (y1 == 0) res = 1;
            else if (y2 == 0) res = -1;
            else res = (y1 < y2) ? -1 : 1;
        }
    }

    if (res == 0 && mb->cfg.get_title) {
        const char *t1 = mb->cfg.get_title(i1);
        const char *t2 = mb->cfg.get_title(i2);
        res = g_utf8_collate(t1 ? t1 : "", t2 ? t2 : "");
    }

    return res;
}

static int cmp_title(gconstpointer a, gconstpointer b, gpointer user_data) {
    return compare_by_field(user_data, VIEW_SORT_TITLE, (GObject *)a, (GObject *)b);
}
static int cmp_subtitle(gconstpointer a, gconstpointer b, gpointer user_data) {
    return compare_by_field(user_data, VIEW_SORT_ARTIST, (GObject *)a, (GObject *)b);
}
static int cmp_year(gconstpointer a, gconstpointer b, gpointer user_data) {
    return compare_by_field(user_data, VIEW_SORT_YEAR, (GObject *)a, (GObject *)b);
}

/* Клик по шапке столбца меняет сортировку таблицы — переносим это в панель
 * управления, чтобы меню и состояние вида оставались согласованными. */
static void on_column_sorter_changed(GtkSorter *sorter, GtkSorterChange change,
                                     gpointer user_data) {
    (void)change;
    MediaBrowser *mb = user_data;
    if (mb->syncing_sort) return;

    GtkColumnViewSorter *cvs = GTK_COLUMN_VIEW_SORTER(sorter);
    GtkColumnViewColumn *col = gtk_column_view_sorter_get_primary_sort_column(cvs);
    GtkSortType order = gtk_column_view_sorter_get_primary_sort_order(cvs);

    ViewSortField field = VIEW_SORT_TITLE;
    if (col == mb->col_sub) field = VIEW_SORT_ARTIST;
    else if (col == mb->col_year) field = VIEW_SORT_YEAR;

    view_controls_set_sort(mb->controls, field, order == GTK_SORT_DESCENDING);
}

/* Применяем состояние панели управления к сортировке таблицы. */
static void apply_sort(MediaBrowser *mb) {
    ViewSortField field = view_controls_get_sort_field(mb->controls);
    GtkColumnViewColumn *col = mb->col_title;
    if (field == VIEW_SORT_ARTIST && mb->col_sub) col = mb->col_sub;
    else if (field == VIEW_SORT_YEAR && mb->col_year) col = mb->col_year;

    GtkSortType order = view_controls_get_sort_descending(mb->controls)
                            ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING;

    /* Ничего не делаем, если сортировка уже такая (в т.ч. после клика по
     * столбцу) — избегаем лишних пересортировок и реентрантного вызова. */
    GtkColumnViewSorter *cvs = GTK_COLUMN_VIEW_SORTER(
        gtk_column_view_get_sorter(mb->col_view));
    if (gtk_column_view_sorter_get_primary_sort_column(cvs) == col &&
        gtk_column_view_sorter_get_primary_sort_order(cvs) == order) {
        return;
    }

    mb->syncing_sort = TRUE;
    gtk_column_view_sort_by_column(mb->col_view, col, order);
    mb->syncing_sort = FALSE;
}

/* ================= GridView фабрики ================= */

static void on_grid_setup(GtkListItemFactory *f, GtkListItem *li, gpointer d) {
    (void)f; (void)d;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);

    GtkWidget *cover_slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(cover_slot, GTK_ALIGN_CENTER);

    GtkWidget *title_lbl = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(title_lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(title_lbl, "heading");

    GtkWidget *sub_lbl = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(sub_lbl), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(sub_lbl, "caption");
    gtk_widget_add_css_class(sub_lbl, "dim-label");

    gtk_box_append(GTK_BOX(box), cover_slot);
    gtk_box_append(GTK_BOX(box), title_lbl);
    gtk_box_append(GTK_BOX(box), sub_lbl);

    gtk_list_item_set_child(li, box);
}

static void on_grid_bind(GtkListItemFactory *f, GtkListItem *li, gpointer d) {
    (void)f;
    MediaBrowser *mb = d;
    GObject *item = gtk_list_item_get_item(li);
    GtkWidget *box = gtk_list_item_get_child(li);

    int cover_size = view_controls_get_grid_cover_size(mb->controls);
    int max_chars = cover_size / 8;
    gtk_widget_set_size_request(box, cover_size + 12, -1);

    GtkWidget *cover_slot = gtk_widget_get_first_child(box);
    GtkWidget *title_lbl = gtk_widget_get_next_sibling(cover_slot);
    GtkWidget *sub_lbl = gtk_widget_get_next_sibling(title_lbl);

    gtk_label_set_max_width_chars(GTK_LABEL(title_lbl), max_chars);
    gtk_label_set_max_width_chars(GTK_LABEL(sub_lbl), max_chars);

    const char *cov = mb->cfg.get_cover_path ? mb->cfg.get_cover_path(item) : NULL;
    GtkWidget *old = gtk_widget_get_first_child(cover_slot);
    if (old) { ui_detach_cover(old); gtk_box_remove(GTK_BOX(cover_slot), old); }
    gtk_box_append(GTK_BOX(cover_slot),
                   mb->cfg.round_covers
                       ? ui_create_round_cover_widget(cov, cover_size)
                       : ui_create_cover_widget(cov, cover_size));

    const char *title = mb->cfg.get_title ? mb->cfg.get_title(item) : "";
    gtk_label_set_text(GTK_LABEL(title_lbl), title);

    if (mb->cfg.get_subtitle) {
        gtk_label_set_text(GTK_LABEL(sub_lbl), mb->cfg.get_subtitle(item));
        gtk_widget_set_visible(sub_lbl, TRUE);
    } else {
        gtk_widget_set_visible(sub_lbl, FALSE);
    }
}

static void on_grid_activate(GtkGridView *grid, guint pos, gpointer d) {
    MediaBrowser *mb = d;
    GListModel *model = G_LIST_MODEL(gtk_grid_view_get_model(grid));
    GObject *item = g_list_model_get_item(model, pos);
    if (item) {
        if (mb->on_activate) mb->on_activate(item, mb->user_data);
        g_object_unref(item);
    }
}

/* ================= ColumnView фабрики ================= */

static void on_col_cover_setup(GtkListItemFactory *f, GtkListItem *li, gpointer d) {
    (void)f; (void)d;
    GtkWidget *slot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top(slot, 4);
    gtk_widget_set_margin_bottom(slot, 4);
    gtk_list_item_set_child(li, slot);
}

static void on_col_cover_bind(GtkListItemFactory *f, GtkListItem *li, gpointer d) {
    (void)f;
    MediaBrowser *mb = d;
    GObject *item = gtk_list_item_get_item(li);
    GtkWidget *slot = gtk_list_item_get_child(li);

    int size = view_controls_get_list_cover_size(mb->controls);
    const char *cov = mb->cfg.get_cover_path ? mb->cfg.get_cover_path(item) : NULL;
    GtkWidget *old = gtk_widget_get_first_child(slot);
    if (old) { ui_detach_cover(old); gtk_box_remove(GTK_BOX(slot), old); }
    gtk_box_append(GTK_BOX(slot),
                   mb->cfg.round_covers
                       ? ui_create_round_cover_widget(cov, size)
                       : ui_create_cover_widget(cov, size));
}

static void on_col_label_setup(GtkListItemFactory *f, GtkListItem *li, gpointer d) {
    (void)f; (void)d;
    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_list_item_set_child(li, lbl);
}

static void on_col_title_bind(GtkListItemFactory *f, GtkListItem *li, gpointer d) {
    (void)f;
    MediaBrowser *mb = d;
    GObject *item = gtk_list_item_get_item(li);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)), mb->cfg.get_title(item));
}

static void on_col_sub_bind(GtkListItemFactory *f, GtkListItem *li, gpointer d) {
    (void)f;
    MediaBrowser *mb = d;
    GObject *item = gtk_list_item_get_item(li);
    GtkWidget *lbl = gtk_list_item_get_child(li);
    gtk_label_set_text(GTK_LABEL(lbl), mb->cfg.get_subtitle(item));
    gtk_widget_add_css_class(lbl, "dim-label");
}

static void on_col_year_bind(GtkListItemFactory *f, GtkListItem *li, gpointer d) {
    (void)f;
    MediaBrowser *mb = d;
    GObject *item = gtk_list_item_get_item(li);
    GtkWidget *lbl = gtk_list_item_get_child(li);
    guint yr = mb->cfg.get_year(item);
    if (yr > 0) {
        char *ys = g_strdup_printf("%u", yr);
        gtk_label_set_text(GTK_LABEL(lbl), ys);
        g_free(ys);
    } else {
        gtk_label_set_text(GTK_LABEL(lbl), "—");
    }
    gtk_widget_add_css_class(lbl, "dim-label");
}

static void on_col_activate(GtkColumnView *cv, guint pos, gpointer d) {
    MediaBrowser *mb = d;
    GListModel *model = G_LIST_MODEL(gtk_column_view_get_model(cv));
    GObject *item = g_list_model_get_item(model, pos);
    if (item) {
        if (mb->on_activate) mb->on_activate(item, mb->user_data);
        g_object_unref(item);
    }
}

MediaBrowser *media_browser_new(GListStore *store,
                                ViewControls *controls,
                                const MediaBrowserConfig *config,
                                MediaItemActivatedFn on_activate,
                                gpointer user_data) {
    MediaBrowser *mb = g_new0(MediaBrowser, 1);
    mb->store = g_object_ref(store);
    mb->controls = controls;
    mb->cfg = *config;
    mb->on_activate = on_activate;
    mb->user_data = user_data;

    /* Таблица. Клик по шапке столбца переключает сортировку: для этого каждому
     * столбцу задаём сортер, а сортер самой таблицы ставим на модель. */
    mb->col_view = GTK_COLUMN_VIEW(gtk_column_view_new(NULL));
    gtk_column_view_set_single_click_activate(mb->col_view, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(mb->col_view), "data-table");
    g_signal_connect(mb->col_view, "activate", G_CALLBACK(on_col_activate), mb);

    GtkListItemFactory *f_cov = gtk_signal_list_item_factory_new();
    g_signal_connect(f_cov, "setup", G_CALLBACK(on_col_cover_setup), NULL);
    g_signal_connect(f_cov, "bind", G_CALLBACK(on_col_cover_bind), mb);
    mb->col_cover = gtk_column_view_column_new("", f_cov);
    gtk_column_view_column_set_fixed_width(mb->col_cover, 60);
    gtk_column_view_append_column(mb->col_view, mb->col_cover);

    GtkListItemFactory *f_title = gtk_signal_list_item_factory_new();
    g_signal_connect(f_title, "setup", G_CALLBACK(on_col_label_setup), NULL);
    g_signal_connect(f_title, "bind", G_CALLBACK(on_col_title_bind), mb);
    mb->col_title = gtk_column_view_column_new(
        mb->cfg.title_col_name ? mb->cfg.title_col_name : "Название", f_title);
    gtk_column_view_column_set_expand(mb->col_title, TRUE);
    GtkSorter *s_title = GTK_SORTER(gtk_custom_sorter_new(cmp_title, mb, NULL));
    gtk_column_view_column_set_sorter(mb->col_title, s_title);
    g_object_unref(s_title);
    gtk_column_view_append_column(mb->col_view, mb->col_title);

    if (mb->cfg.get_subtitle) {
        GtkListItemFactory *f_sub = gtk_signal_list_item_factory_new();
        g_signal_connect(f_sub, "setup", G_CALLBACK(on_col_label_setup), NULL);
        g_signal_connect(f_sub, "bind", G_CALLBACK(on_col_sub_bind), mb);
        mb->col_sub = gtk_column_view_column_new(
            mb->cfg.subtitle_col_name ? mb->cfg.subtitle_col_name : "Исполнитель", f_sub);
        gtk_column_view_column_set_expand(mb->col_sub, TRUE);
        GtkSorter *s_sub = GTK_SORTER(gtk_custom_sorter_new(cmp_subtitle, mb, NULL));
        gtk_column_view_column_set_sorter(mb->col_sub, s_sub);
        g_object_unref(s_sub);
        gtk_column_view_append_column(mb->col_view, mb->col_sub);
    }
    if (mb->cfg.get_year && mb->cfg.year_col_name) {
        GtkListItemFactory *f_yr = gtk_signal_list_item_factory_new();
        g_signal_connect(f_yr, "setup", G_CALLBACK(on_col_label_setup), NULL);
        g_signal_connect(f_yr, "bind", G_CALLBACK(on_col_year_bind), mb);
        mb->col_year = gtk_column_view_column_new(mb->cfg.year_col_name, f_yr);
        gtk_column_view_column_set_fixed_width(mb->col_year, 80);
        GtkSorter *s_yr = GTK_SORTER(gtk_custom_sorter_new(cmp_year, mb, NULL));
        gtk_column_view_column_set_sorter(mb->col_year, s_yr);
        g_object_unref(s_yr);
        gtk_column_view_append_column(mb->col_view, mb->col_year);
    }

    /* Сортер таблицы становится сортером общей модели — сетка и список
     * сортируются одинаково и реагируют на клики по столбцам. */
    GtkSorter *col_sorter = gtk_column_view_get_sorter(mb->col_view);
    g_signal_connect(col_sorter, "changed", G_CALLBACK(on_column_sorter_changed), mb);
    mb->sort_model = gtk_sort_list_model_new(G_LIST_MODEL(g_object_ref(mb->store)),
                                             g_object_ref(col_sorter));

    GtkNoSelection *col_sel = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(mb->sort_model)));
    gtk_column_view_set_model(mb->col_view, GTK_SELECTION_MODEL(col_sel));

    /* Сетка */
    GtkNoSelection *grid_sel = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(mb->sort_model)));
    GtkListItemFactory *gf = gtk_signal_list_item_factory_new();
    g_signal_connect(gf, "setup", G_CALLBACK(on_grid_setup), NULL);
    g_signal_connect(gf, "bind", G_CALLBACK(on_grid_bind), mb);

    mb->grid_view = GTK_GRID_VIEW(gtk_grid_view_new(GTK_SELECTION_MODEL(grid_sel), gf));
    gtk_grid_view_set_single_click_activate(mb->grid_view, TRUE);
    gtk_grid_view_set_min_columns(mb->grid_view, 2);
    gtk_grid_view_set_max_columns(mb->grid_view, 8);
    g_signal_connect(mb->grid_view, "activate", G_CALLBACK(on_grid_activate), mb);
    /* gtk_grid_view_new() забирает владение и моделью, и фабрикой (transfer full),
     * поэтому отдельно освобождать grid_sel не нужно. */

    GtkWidget *grid_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(grid_scroll), GTK_WIDGET(mb->grid_view));

    apply_sort(mb);

    GtkWidget *col_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(col_scroll), GTK_WIDGET(mb->col_view));
    /* Стек режимов */
    mb->stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(mb->stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_add_named(mb->stack, grid_scroll, "grid");
    gtk_stack_add_named(mb->stack, col_scroll, "list");
    return mb;
}
GtkWidget *media_browser_get_widget(MediaBrowser *mb) {
    return GTK_WIDGET(mb->stack);
}
void media_browser_refresh(MediaBrowser *mb) {
    if (view_controls_get_mode(mb->controls) == VIEW_MODE_GRID) {
        gtk_stack_set_visible_child_name(mb->stack, "grid");
    } else {
        gtk_stack_set_visible_child_name(mb->stack, "list");
    }

    apply_sort(mb);

    ViewZoom current_zoom = view_controls_get_zoom(mb->controls);
    if (current_zoom != mb->last_zoom) {
        mb->last_zoom = current_zoom;

        int list_cov = view_controls_get_list_cover_size(mb->controls);
        gtk_column_view_column_set_fixed_width(mb->col_cover, list_cov + 16);

        GtkListItemFactory *gf = gtk_signal_list_item_factory_new();
        g_signal_connect(gf, "setup", G_CALLBACK(on_grid_setup), NULL);
        g_signal_connect(gf, "bind", G_CALLBACK(on_grid_bind), mb);
        gtk_grid_view_set_factory(mb->grid_view, gf);
        g_object_unref(gf);

        GtkListItemFactory *cf = gtk_signal_list_item_factory_new();
        g_signal_connect(cf, "setup", G_CALLBACK(on_col_cover_setup), NULL);
        g_signal_connect(cf, "bind", G_CALLBACK(on_col_cover_bind), mb);
        gtk_column_view_column_set_factory(mb->col_cover, cf);
        g_object_unref(cf);
    }
}


void media_browser_free(MediaBrowser *mb) {
    if (!mb) return;
    if (mb->col_cover) g_object_unref(mb->col_cover);
    if (mb->col_title) g_object_unref(mb->col_title);
    if (mb->col_sub) g_object_unref(mb->col_sub);
    if (mb->col_year) g_object_unref(mb->col_year);
    if (mb->sort_model) g_object_unref(mb->sort_model);
    if (mb->store) g_object_unref(mb->store);
    g_free(mb);
}
