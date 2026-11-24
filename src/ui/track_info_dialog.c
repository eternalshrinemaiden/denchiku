#include "track_info_dialog.h"

#include "settings.h"
#include "tag_editor.h"
#include "ui_helpers.h"

#include <adwaita.h>

/* Диалог «Сведения» показывает метаданные трека: сверху «альбомные» теги,
 * ниже (через разделитель) — «трековые», справа — обложка фиксированного
 * размера. Правки здесь нет: теги меняет внешнее приложение (см. tag_editor). */

typedef enum {
    FIELD_TITLE,
    FIELD_ARTIST,
    FIELD_ALBUM,
    FIELD_ALBUM_ARTIST,
    FIELD_GENRE,
    FIELD_YEAR,
    FIELD_TRACK,
    FIELD_DISC,
    FIELD_DISC_SUBTITLE,
    FIELD_COUNT
} FieldId;

/* Размер обложки. Виджет обложки имеет фиксированный размер (size_request +
 * hexpand/vexpand FALSE), поэтому не растягивается вместе с окном. */
#define INFO_COVER_SIZE 168

typedef struct {
    GtkWidget *values[FIELD_COUNT];
    Track *track;       /* заимствован (живёт в медиатеке) */
    GtkWindow *parent;  /* для диалога ошибки запуска редактора */
} TrackInfo;

/* Строка «Сведения»: заголовок + значение справа. */
static GtkWidget *make_info_row(const char *title, GtkWidget **out_value) {
    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle_lines(row, 3);

    GtkWidget *value = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(value), 1.0f);
    gtk_label_set_ellipsize(GTK_LABEL(value), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(value, "dim-label");
    gtk_widget_set_valign(value, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(row, value);

    *out_value = value;
    return GTK_WIDGET(row);
}

static void set_value(TrackInfo *ti, FieldId id, const char *text) {
    gtk_label_set_text(GTK_LABEL(ti->values[id]), (text && *text) ? text : "—");
}

static void set_value_num(TrackInfo *ti, FieldId id, guint value) {
    char *buf = g_strdup_printf("%u", value);
    set_value(ti, id, value ? buf : NULL);
    g_free(buf);
}

/* Группа «альбомных» тегов. */
static GtkWidget *build_album_group(TrackInfo *ti) {
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_add(group, make_info_row("Альбом", &ti->values[FIELD_ALBUM]));
    adw_preferences_group_add(group, make_info_row("Исполнитель альбома", &ti->values[FIELD_ALBUM_ARTIST]));
    adw_preferences_group_add(group, make_info_row("Год", &ti->values[FIELD_YEAR]));
    adw_preferences_group_add(group, make_info_row("Жанр", &ti->values[FIELD_GENRE]));
    return GTK_WIDGET(group);
}

/* Группа «трековых» тегов. */
static GtkWidget *build_track_group(TrackInfo *ti) {
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_add(group, make_info_row("Название", &ti->values[FIELD_TITLE]));
    adw_preferences_group_add(group, make_info_row("Исполнитель", &ti->values[FIELD_ARTIST]));
    adw_preferences_group_add(group, make_info_row("Номер трека", &ti->values[FIELD_TRACK]));
    adw_preferences_group_add(group, make_info_row("Номер диска", &ti->values[FIELD_DISC]));
    adw_preferences_group_add(group, make_info_row("Название диска", &ti->values[FIELD_DISC_SUBTITLE]));
    return GTK_WIDGET(group);
}

/* Разделитель между альбомными и трековыми тегами. */
static GtkWidget *make_divider(void) {
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep, 4);
    gtk_widget_set_margin_bottom(sep, 4);
    gtk_widget_set_margin_start(sep, 4);
    gtk_widget_set_margin_end(sep, 4);
    return sep;
}

/* Имя файла трека (с расширением) для заголовка окна. */
static char *track_file_name(const Track *t) {
    if (!t || !t->uri) return g_strdup("");
    GFile *f = g_file_new_for_uri(t->uri);
    char *path = g_file_get_path(f);
    char *name = NULL;
    if (path) {
        name = g_filename_display_basename(path);
    } else {
        char *base = g_file_get_basename(f);
        name = base ? g_filename_display_name(base) : g_strdup("");
        g_free(base);
    }
    g_free(path);
    g_object_unref(f);
    return name;
}

/* Содержимое: слева столбец тегов (сверху альбомные, ниже трековые, между ними
 * разделитель), справа — обложка. */
static GtkWidget *build_content(TrackInfo *ti, Track *t) {
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_widget_set_margin_top(hbox, 12);
    gtk_widget_set_margin_bottom(hbox, 12);
    gtk_widget_set_margin_start(hbox, 12);
    gtk_widget_set_margin_end(hbox, 12);

    GtkWidget *tags = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(tags, TRUE);
    gtk_widget_set_valign(tags, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(tags), build_album_group(ti));
    gtk_box_append(GTK_BOX(tags), make_divider());
    gtk_box_append(GTK_BOX(tags), build_track_group(ti));

    /* Контейнер обложки: фиксированный размер задаёт сам виджет обложки. */
    GtkWidget *cover_slot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(cover_slot, GTK_ALIGN_END);
    gtk_widget_set_valign(cover_slot, GTK_ALIGN_START);
    gtk_widget_set_hexpand(cover_slot, FALSE);
    gtk_widget_set_vexpand(cover_slot, FALSE);
    gtk_box_append(GTK_BOX(cover_slot),
                   ui_create_cover_widget(t->cover_path, INFO_COVER_SIZE));

    gtk_box_append(GTK_BOX(hbox), tags);
    gtk_box_append(GTK_BOX(hbox), cover_slot);
    return hbox;
}

static void fill_values(TrackInfo *ti, Track *t) {
    set_value(ti, FIELD_TITLE, t->title);
    set_value(ti, FIELD_ARTIST, t->artist);
    set_value(ti, FIELD_ALBUM, t->album);
    set_value(ti, FIELD_ALBUM_ARTIST, t->album_artist);
    set_value(ti, FIELD_GENRE, t->genre);
    set_value(ti, FIELD_DISC_SUBTITLE, t->disc_subtitle);
    set_value_num(ti, FIELD_YEAR, t->year);
    set_value_num(ti, FIELD_TRACK, t->track_number);
    set_value_num(ti, FIELD_DISC, t->disc_number);
}

/* Кнопка-карандаш: открывает текущий трек во внешнем редакторе тегов. */
static void on_edit_tags_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    TrackInfo *ti = data;
    if (!ti->track || !ti->track->uri) return;

    GPtrArray *uris = g_ptr_array_new();
    g_ptr_array_add(uris, ti->track->uri);

    GError *err = NULL;
    gboolean ok = tag_editor_launch(settings_tag_editor(), uris, &err);
    g_ptr_array_unref(uris);

    if (!ok) {
        AdwDialog *alert = adw_alert_dialog_new("Не удалось открыть редактор тегов",
                                                err ? err->message : NULL);
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "ok", "Ок");
        adw_dialog_present(alert, GTK_WIDGET(ti->parent));
    }
    if (err) g_error_free(err);
}

void track_info_dialog_present(GtkWindow *parent, Track *track) {
    if (!track) return;

    TrackInfo *ti = g_new0(TrackInfo, 1);
    ti->track = track;
    ti->parent = parent;

    /* Сначала строим содержимое (оно создаёт строки и заполняет ti->values),
     * затем — заполняем значения. */
    GtkWidget *content = build_content(ti, track);
    fill_values(ti, track);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);

    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 720);
    adw_clamp_set_child(ADW_CLAMP(clamp), content);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);

    /* В заголовке — имя файла, в подзаголовке — название трека. */
    char *file_name = track_file_name(track);
    GtkWidget *title_widget = adw_window_title_new(
        file_name, (track->title && *track->title) ? track->title : "");
    g_free(file_name);

    GtkWidget *hb = gtk_header_bar_new();
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hb), title_widget);

    /* Кнопка правки тегов во внешнем редакторе. */
    GtkWidget *edit_btn = gtk_button_new_from_icon_name("document-edit-symbolic");
    gtk_widget_add_css_class(edit_btn, "flat");
    gtk_widget_set_tooltip_text(edit_btn, "Редактировать теги");
    g_signal_connect(edit_btn, "clicked", G_CALLBACK(on_edit_tags_clicked), ti);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), edit_btn);

    AdwToolbarView *tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(tv, hb);
    adw_toolbar_view_set_content(tv, scroll);

    AdwDialog *dialog = adw_dialog_new();
    adw_dialog_set_content_width(dialog, 720);
    adw_dialog_set_child(dialog, GTK_WIDGET(tv));
    g_object_set_data_full(G_OBJECT(dialog), "track-info-state", ti, g_free);

    adw_dialog_present(dialog, GTK_WIDGET(parent));
}
