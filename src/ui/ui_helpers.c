#include "ui_helpers.h"
#include "track.h"
#include <gdk-pixbuf/gdk-pixbuf.h>

/* ================= Контейнер, не задающий минимальную ширину =================
 * Обнуляет минимальную ширину ребёнка по горизонтали (натуральную сохраняет),
 * чтобы AdwBottomSheet не запрашивал больше ширины, чем есть в окне. Внутренние
 * AdwOverlaySplitView при этом сами ужимают боковые панели по доступному месту. */

/* Общий приём для всех обёрток, которые не должны задавать минимальную ширину. */
void ui_measure_min_width(GtkWidget *child, GtkOrientation orientation, int for_size,
                          int *minimum, int *natural,
                          int *minimum_baseline, int *natural_baseline) {
    if (child) {
        gtk_widget_measure(child, orientation, for_size,
                           minimum, natural, minimum_baseline, natural_baseline);
        if (orientation == GTK_ORIENTATION_HORIZONTAL && minimum) *minimum = 0;
    } else {
        if (minimum) *minimum = 0;
        if (natural) *natural = 0;
    }
}

#define DENCHIKU_TYPE_MIN_WIDTH_BIN (denchiku_min_width_bin_get_type())
G_DECLARE_FINAL_TYPE(DenchikuMinWidthBin, denchiku_min_width_bin, DENCHIKU, MIN_WIDTH_BIN, GtkWidget)

struct _DenchikuMinWidthBin {
    GtkWidget parent_instance;
    GtkWidget *child;
};

G_DEFINE_FINAL_TYPE(DenchikuMinWidthBin, denchiku_min_width_bin, GTK_TYPE_WIDGET)

static void denchiku_min_width_bin_measure(GtkWidget *widget, GtkOrientation orientation,
                                        int for_size, int *minimum, int *natural,
                                        int *minimum_baseline, int *natural_baseline) {
    DenchikuMinWidthBin *self = DENCHIKU_MIN_WIDTH_BIN(widget);
    ui_measure_min_width(self->child, orientation, for_size,
                         minimum, natural, minimum_baseline, natural_baseline);
}

static void denchiku_min_width_bin_allocate(GtkWidget *widget, int width, int height, int baseline) {
    DenchikuMinWidthBin *self = DENCHIKU_MIN_WIDTH_BIN(widget);
    if (self->child) {
        gtk_widget_allocate(self->child, width, height, baseline, NULL);
    }
}

static void denchiku_min_width_bin_dispose(GObject *object) {
    DenchikuMinWidthBin *self = DENCHIKU_MIN_WIDTH_BIN(object);
    g_clear_pointer(&self->child, gtk_widget_unparent);
    G_OBJECT_CLASS(denchiku_min_width_bin_parent_class)->dispose(object);
}

static void denchiku_min_width_bin_class_init(DenchikuMinWidthBinClass *klass) {
    G_OBJECT_CLASS(klass)->dispose = denchiku_min_width_bin_dispose;
}

static void denchiku_min_width_bin_init(DenchikuMinWidthBin *self) {
    self->child = NULL;
    GtkLayoutManager *layout = gtk_custom_layout_new(NULL,
        denchiku_min_width_bin_measure, denchiku_min_width_bin_allocate);
    gtk_widget_set_layout_manager(GTK_WIDGET(self), layout);
}

GtkWidget *ui_create_min_width_bin(GtkWidget *child) {
    DenchikuMinWidthBin *bin = g_object_new(DENCHIKU_TYPE_MIN_WIDTH_BIN, NULL);
    if (child) {
        bin->child = child;
        gtk_widget_set_parent(child, GTK_WIDGET(bin));
    }
    return GTK_WIDGET(bin);
}

/* ================= Контейнер фиксированного квадратного размера =================
 * Обложка (GtkFrame + GtkPicture) наследует натуральный размер декодированной
 * текстуры, поэтому в контейнере без жёстких размеров «квадрат» превращается в
 * прямоугольник, а круглый аватар — в овал. Эта обёртка сообщает ровно size×size
 * и выделяет ребёнку те же размеры. */
#define DENCHIKU_TYPE_FIXED_SIZE_BIN (denchiku_fixed_size_bin_get_type())
G_DECLARE_FINAL_TYPE(DenchikuFixedSizeBin, denchiku_fixed_size_bin, DENCHIKU, FIXED_SIZE_BIN, GtkWidget)

struct _DenchikuFixedSizeBin {
    GtkWidget parent_instance;
    GtkWidget *child;
    int size;
};

G_DEFINE_FINAL_TYPE(DenchikuFixedSizeBin, denchiku_fixed_size_bin, GTK_TYPE_WIDGET)

static void denchiku_fixed_size_bin_measure(GtkWidget *widget, GtkOrientation orientation,
                                            int for_size, int *minimum, int *natural,
                                            int *minimum_baseline, int *natural_baseline) {
    (void)orientation; (void)for_size;
    DenchikuFixedSizeBin *self = DENCHIKU_FIXED_SIZE_BIN(widget);
    if (minimum) *minimum = self->size;
    if (natural) *natural = self->size;
    if (minimum_baseline) *minimum_baseline = -1;
    if (natural_baseline) *natural_baseline = -1;
}

static void denchiku_fixed_size_bin_allocate(GtkWidget *widget, int width, int height, int baseline) {
    (void)width; (void)height;
    DenchikuFixedSizeBin *self = DENCHIKU_FIXED_SIZE_BIN(widget);
    if (self->child) {
        gtk_widget_allocate(self->child, self->size, self->size, baseline, NULL);
    }
}

static void denchiku_fixed_size_bin_dispose(GObject *object) {
    DenchikuFixedSizeBin *self = DENCHIKU_FIXED_SIZE_BIN(object);
    g_clear_pointer(&self->child, gtk_widget_unparent);
    G_OBJECT_CLASS(denchiku_fixed_size_bin_parent_class)->dispose(object);
}

static void denchiku_fixed_size_bin_class_init(DenchikuFixedSizeBinClass *klass) {
    G_OBJECT_CLASS(klass)->dispose = denchiku_fixed_size_bin_dispose;
}

static void denchiku_fixed_size_bin_init(DenchikuFixedSizeBin *self) {
    self->child = NULL;
    self->size = 0;
    GtkLayoutManager *layout = gtk_custom_layout_new(NULL,
        denchiku_fixed_size_bin_measure, denchiku_fixed_size_bin_allocate);
    gtk_widget_set_layout_manager(GTK_WIDGET(self), layout);
}

GtkWidget *ui_create_fixed_size_bin(GtkWidget *child, int size) {
    DenchikuFixedSizeBin *bin = g_object_new(DENCHIKU_TYPE_FIXED_SIZE_BIN, NULL);
    bin->size = size;
    gtk_widget_set_halign(GTK_WIDGET(bin), GTK_ALIGN_CENTER);
    gtk_widget_set_valign(GTK_WIDGET(bin), GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(GTK_WIDGET(bin), FALSE);
    gtk_widget_set_vexpand(GTK_WIDGET(bin), FALSE);
    if (child) {
        bin->child = child;
        gtk_widget_set_parent(child, GTK_WIDGET(bin));
    }
    return GTK_WIDGET(bin);
}

GtkWidget *ui_create_sidebar_row_content(const char *title, const char *icon_name) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
    GtkWidget *label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    /* Длинные названия обрезаем многоточием, а не растягиваем боковую панель. */
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    return box;
}

GtkWidget *ui_create_sidebar_row(const char *page_id, const char *title, const char *icon_name) {
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row),
                               ui_create_sidebar_row_content(title, icon_name));
    g_object_set_data_full(G_OBJECT(row), "page-id", g_strdup(page_id), g_free);

    return row;
}

GtkWidget *ui_create_sidebar_section_header(const char *title, GtkWidget *suffix) {
    GtkWidget *row = gtk_list_box_row_new();
    /* Подзаголовок — только визуальный разделитель, его нельзя выбрать или
     * активировать. */
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 3);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 6);

    GtkWidget *label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(label, "dim-label");
    gtk_widget_add_css_class(label, "caption-heading");
    gtk_widget_set_hexpand(label, TRUE);

    gtk_box_append(GTK_BOX(box), label);
    if (suffix) gtk_box_append(GTK_BOX(box), suffix);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    return row;
}

void ui_set_row_playing(AdwActionRow *row, gboolean is_playing) {
    GtkWidget *existing_icon = g_object_get_data(G_OBJECT(row), "playing-icon");

    if (is_playing) {
        gtk_widget_add_css_class(GTK_WIDGET(row), "accent");
        if (!existing_icon) {
            GtkWidget *icon = gtk_image_new_from_icon_name("media-playback-start-symbolic");
            adw_action_row_add_prefix(row, icon);
            g_object_set_data(G_OBJECT(row), "playing-icon", icon);
        }
    } else {
        gtk_widget_remove_css_class(GTK_WIDGET(row), "accent");
        if (existing_icon) {
            adw_action_row_remove(row, existing_icon);
            g_object_set_data(G_OBJECT(row), "playing-icon", NULL);
        }
    }
}

GtkWidget *ui_create_content_header(AdwOverlaySplitView *split_view) {
    AdwHeaderBar *hb = ADW_HEADER_BAR(adw_header_bar_new());
    adw_header_bar_set_show_start_title_buttons(hb, FALSE);

    GtkWidget *toggle_btn = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(toggle_btn), "sidebar-show-symbolic");
    gtk_widget_set_tooltip_text(toggle_btn, "Боковая панель");
    g_object_bind_property(split_view, "show-sidebar",
                           toggle_btn, "active",
                           G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
    g_object_bind_property(split_view, "collapsed",
                           toggle_btn, "visible",
                           G_BINDING_SYNC_CREATE);
    adw_header_bar_pack_start(hb, toggle_btn);

    return GTK_WIDGET(hb);
}

GtkWidget *ui_create_play_shuffle_group(GtkWidget **out_play_btn,
                                        GtkWidget **out_shuffle_btn) {
    /* Класс linked склеивает кнопки в единый блок, как у split-button: общая
     * рамка, внешние скругления только у крайних частей. */
    GtkWidget *group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(group, "linked");
    gtk_widget_set_valign(group, GTK_ALIGN_CENTER);

    GtkWidget *play = gtk_button_new_from_icon_name("media-playback-start-symbolic");
    gtk_widget_set_tooltip_text(play, "Воспроизвести");
    gtk_widget_add_css_class(play, "suggested-action");
    gtk_widget_set_valign(play, GTK_ALIGN_CENTER);

    GtkWidget *shuffle = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(shuffle), "media-playlist-shuffle-symbolic");
    gtk_widget_set_tooltip_text(shuffle, "Перемешать");
    gtk_widget_set_valign(shuffle, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(group), play);
    gtk_box_append(GTK_BOX(group), shuffle);

    if (out_play_btn) *out_play_btn = play;
    if (out_shuffle_btn) *out_shuffle_btn = shuffle;
    return group;
}

/* ================= Асинхронный кэш обложек =================
 * Декодирование обложек альбомов (нередко 800x800 и несколько мегабайт) на
 * главном потоке занимает десятки миллисекунд на файл. При построении списков
 * GTK привязывает сотни элементов сразу, поэтому синхронное декодирование
 * надолго замораживает старт. Декодируем в фоновом пуле, кэшируем готовые
 * текстуры по (путь, размер) и обновляем виджеты по готовности. Все обращения к
 * кэшу и спискам ожидающих выполняются только в главном потоке. */

typedef struct {
    char *key;
    char *path;
    int size;
} CoverDecode;

typedef struct {
    char *key;
    GdkPixbuf *pixbuf;
} CoverResult;

static GHashTable *cover_cache;    /* "путь\x1fразмер" -> GdkTexture* (принадлежит кэшу) */
static GHashTable *cover_waiters;  /* "путь\x1fразмер" -> GPtrArray<GtkPicture*> */
static GThreadPool *cover_pool;
static GQueue *cover_lru;          /* ключи в порядке использования: голова — самый старый */
static GHashTable *cover_lru_nodes;/* ключ -> GList* (узел в cover_lru) для O(1) доступа */
static gsize cover_cache_bytes;    /* суммарный размер декодированных текстур */
static guint cover_cache_count;    /* число записей в кэше */

/* Ограничиваем кэш по ПАМЯТИ, а не по числу записей: крупные обложки
 * (крупный зум альбомов) занимают мегабайты каждая, и 400 записей раздували
 * потребление до сотен мегабайт. При переполнении вытесняется самая давно
 * использованная запись (LRU). Лимит по числу оставляем как страховку от
 * распухания на множестве мелких текстур. */
#define COVER_CACHE_MAX_BYTES ((gsize)64 * 1024 * 1024)
#define COVER_CACHE_MAX_ENTRIES 2000

static void cover_decode_worker(gpointer data, gpointer user_data);

static gsize texture_size_bytes(GdkTexture *texture) {
    /* RGBA-оценка: формат текстуры может быть без альфы, но переоценка здесь
     * безопаснее недооценки (кэш только вытеснит чуть больше). */
    return (gsize)gdk_texture_get_width(texture) *
           (gsize)gdk_texture_get_height(texture) * 4;
}

static void cover_cache_touch(const char *key) {
    /* Узел ищем через хэш: линейный поиск по очереди на каждый показ обложки
     * (при прокрутке — сотни раз в секунду) заметно подтормаживал бы. */
    GList *node = g_hash_table_lookup(cover_lru_nodes, key);
    if (node) {
        g_queue_unlink(cover_lru, node);
        g_queue_push_tail_link(cover_lru, node);
    }
}

static void cover_cache_evict_one(void) {
    char *oldest = g_queue_pop_head(cover_lru);
    if (!oldest) return;
    g_hash_table_remove(cover_lru_nodes, oldest);
    GdkTexture *texture = g_hash_table_lookup(cover_cache, oldest);
    if (texture) {
        cover_cache_bytes -= texture_size_bytes(texture);
        if (cover_cache_count > 0) cover_cache_count--;
    }
    g_hash_table_remove(cover_cache, oldest);
    g_free(oldest);
}

static void cover_cache_store(const char *key, GdkTexture *texture) {
    /* Защита от повторной вставки того же ключа (обычно исключена вызовами):
     * снимаем старый узел LRU, чтобы не дублировать его в очереди и хэше. */
    if (g_hash_table_contains(cover_lru_nodes, key)) {
        GList *node = g_hash_table_lookup(cover_lru_nodes, key);
        if (node) g_queue_delete_link(cover_lru, node);
        g_hash_table_remove(cover_lru_nodes, key);
        GdkTexture *old = g_hash_table_lookup(cover_cache, key);
        if (old) {
            cover_cache_bytes -= texture_size_bytes(old);
            if (cover_cache_count > 0) cover_cache_count--;
        }
    }

    gsize bytes = texture_size_bytes(texture);
    while (!g_queue_is_empty(cover_lru) &&
           (cover_cache_count >= COVER_CACHE_MAX_ENTRIES ||
            cover_cache_bytes + bytes > COVER_CACHE_MAX_BYTES)) {
        cover_cache_evict_one();
    }
    g_hash_table_replace(cover_cache, g_strdup(key), g_object_ref(texture));
    g_queue_push_tail(cover_lru, g_strdup(key));
    g_hash_table_insert(cover_lru_nodes, g_strdup(key), cover_lru->tail);
    cover_cache_bytes += bytes;
    cover_cache_count++;
}

static void cover_loader_init(void) {
    if (cover_pool) return;
    cover_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    cover_waiters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          (GDestroyNotify)g_ptr_array_unref);
    cover_lru = g_queue_new();
    cover_lru_nodes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    cover_cache_bytes = 0;
    cover_cache_count = 0;
    cover_pool = g_thread_pool_new(cover_decode_worker, NULL, 2, FALSE, NULL);
}

static GdkTexture *pixbuf_to_texture(GdkPixbuf *pixbuf) {
    int width = gdk_pixbuf_get_width(pixbuf);
    int height = gdk_pixbuf_get_height(pixbuf);
    int stride = gdk_pixbuf_get_rowstride(pixbuf);
    gboolean has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
    gsize len = (gsize)stride * height;

    GBytes *bytes = g_bytes_new_with_free_func(
        gdk_pixbuf_get_pixels(pixbuf), len, g_object_unref, g_object_ref(pixbuf));

    GdkTexture *texture = gdk_memory_texture_new(
        width, height,
        has_alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
        bytes, stride);
    g_bytes_unref(bytes);
    return texture;
}

static gboolean cover_decode_done(gpointer data) {
    CoverResult *res = data;

    GdkTexture *texture = NULL;
    if (res->pixbuf) {
        texture = pixbuf_to_texture(res->pixbuf);
        g_object_unref(res->pixbuf);
    }

    GPtrArray *waiters = g_hash_table_lookup(cover_waiters, res->key);
    if (waiters) {
        if (texture) {
            for (guint i = 0; i < waiters->len; i++) {
                gtk_picture_set_paintable(GTK_PICTURE(g_ptr_array_index(waiters, i)),
                                          GDK_PAINTABLE(texture));
            }
            if (!g_hash_table_contains(cover_cache, res->key)) {
                cover_cache_store(res->key, texture);
            }
        }
        /* Снимаем запись ожидания: удаление освободит массив и разыменует виджеты. */
        g_hash_table_remove(cover_waiters, res->key);
    }

    if (texture) g_object_unref(texture);
    g_free(res->key);
    g_free(res);
    return G_SOURCE_REMOVE;
}

static void cover_decode_worker(gpointer data, gpointer user_data) {
    (void)user_data;
    CoverDecode *job = data;

    /* Декодируем с запасом ×2 под HiDPI, сохраняя пропорции. */
    int target = job->size * 2;
    GError *err = NULL;
    GdkPixbuf *pix = gdk_pixbuf_new_from_file_at_scale(job->path, target, target, TRUE, &err);
    if (err) g_error_free(err);

    CoverResult *res = g_new0(CoverResult, 1);
    res->key = job->key;
    res->pixbuf = pix;

    g_free(job->path);
    g_free(job);

    g_main_context_invoke(NULL, cover_decode_done, res);
}

static gboolean cover_exists(const char *path) {
    return path && *path && g_file_test(path, G_FILE_TEST_EXISTS);
}

/* Подключает асинхронную загрузку обложки (кэш + пул) к готовому GtkPicture. */
static void attach_cover(GtkWidget *pic, const char *cover_path, int size) {
    char *key = g_strdup_printf("%s\x1f%d", cover_path, size);
    GdkTexture *cached = g_hash_table_lookup(cover_cache, key);
    if (cached) {
        cover_cache_touch(key);
        gtk_picture_set_paintable(GTK_PICTURE(pic), GDK_PAINTABLE(cached));
        g_free(key);
        return;
    }

    GPtrArray *waiters = g_hash_table_lookup(cover_waiters, key);
    if (waiters) {
        g_free(key);
    } else {
        waiters = g_ptr_array_new_with_free_func(g_object_unref);
        g_hash_table_insert(cover_waiters, key, waiters);

        CoverDecode *job = g_new0(CoverDecode, 1);
        job->key = g_strdup(key);
        job->path = g_strdup(cover_path);
        job->size = size;
        g_thread_pool_push(cover_pool, job, NULL);
    }
    g_ptr_array_add(waiters, g_object_ref(pic));
}

/* Круглая обложка: тот же контейнер, но со скруглением до круга; overflow:hidden
 * у кадра обрезает картинку/заглушку по кругу. */
static void ensure_round_cover_css(void) {
    static gboolean loaded = FALSE;
    if (loaded) return;
    loaded = TRUE;

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        ".denchiku-round-cover { border-radius: 9999px; }");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* Adwaita задаёт frame { border-radius: 12px }, поэтому у маленьких обложек
 * (очередь, компактный список, плеер-бар) скругление выглядит чрезмерным.
 * Для некруглых обложек подбираем радиус пропорционально размеру (12.5%, но не
 * меньше 4 и не больше 12 px) и регистрируем под него отдельный CSS-класс. */
static void ensure_cover_radius_css(int radius) {
    static GHashTable *registered = NULL;
    if (!registered) registered = g_hash_table_new(g_direct_hash, g_direct_equal);
    if (g_hash_table_contains(registered, GINT_TO_POINTER(radius))) return;
    g_hash_table_add(registered, GINT_TO_POINTER(radius));

    char *rule = g_strdup_printf(".denchiku-cover-r%d { border-radius: %dpx; }",
                                 radius, radius);
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, rule);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    g_free(rule);
}

static GtkWidget *create_cover_widget(const char *cover_path, int size, gboolean round) {
    cover_loader_init();

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_widget_set_size_request(frame, size, size);
    gtk_widget_set_halign(frame, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(frame, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(frame, FALSE);
    gtk_widget_set_vexpand(frame, FALSE);
    gtk_widget_set_overflow(frame, GTK_OVERFLOW_HIDDEN);
    if (round) {
        ui_make_cover_round(frame);
    } else {
        int radius = CLAMP(size / 8, 4, 12);
        ensure_cover_radius_css(radius);
        char cls[32];
        g_snprintf(cls, sizeof cls, "denchiku-cover-r%d", radius);
        gtk_widget_add_css_class(frame, cls);
    }

    if (cover_exists(cover_path)) {
        GtkWidget *pic = gtk_picture_new();
        gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_COVER);
        /* Без can_shrink картинка требует натуральный размер декодированной
         * текстуры и раздувает строку; с ним — ужимается ровно до size. */
        gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
        gtk_widget_set_size_request(pic, size, size);
        gtk_widget_set_hexpand(pic, FALSE);
        gtk_widget_set_vexpand(pic, FALSE);
        gtk_widget_set_valign(pic, GTK_ALIGN_CENTER);
        gtk_frame_set_child(GTK_FRAME(frame), pic);
        attach_cover(pic, cover_path, size);
        return frame;
    }

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(box, size, size);
    gtk_widget_set_hexpand(box, FALSE);
    gtk_widget_set_vexpand(box, FALSE);
    gtk_widget_add_css_class(box, "view");

    GtkWidget *icon = gtk_image_new_from_icon_name("media-optical-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), size / 2);
    gtk_widget_set_vexpand(icon, TRUE);
    gtk_widget_set_hexpand(icon, TRUE);
    gtk_widget_add_css_class(icon, "dim-label");

    gtk_box_append(GTK_BOX(box), icon);
    gtk_frame_set_child(GTK_FRAME(frame), box);

    return frame;
}

GtkWidget *ui_create_cover_widget(const char *cover_path, int size) {
    return create_cover_widget(cover_path, size, FALSE);
}

void ui_make_cover_round(GtkWidget *widget) {
    if (!widget) return;
    ensure_round_cover_css();
    gtk_widget_add_css_class(widget, "denchiku-round-cover");
}

GtkWidget *ui_create_round_cover_widget(const char *cover_path, int size) {
    return create_cover_widget(cover_path, size, TRUE);
}

GtkWidget *ui_create_cover_picture(const char *cover_path, int size) {
    cover_loader_init();

    if (cover_exists(cover_path)) {
        GtkWidget *pic = gtk_picture_new();
        gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_CONTAIN);
        gtk_widget_set_hexpand(pic, TRUE);
        gtk_widget_set_vexpand(pic, TRUE);
        attach_cover(pic, cover_path, size);
        return pic;
    }

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(box, TRUE);
    gtk_widget_set_vexpand(box, TRUE);

    GtkWidget *icon = gtk_image_new_from_icon_name("media-optical-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 128);
    gtk_widget_set_hexpand(icon, TRUE);
    gtk_widget_set_vexpand(icon, TRUE);
    gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(icon, "dim-label");

    gtk_box_append(GTK_BOX(box), icon);
    return box;
}

GdkTexture *ui_create_cover_texture(const char *cover_path, int size) {
    if (!cover_path || !*cover_path || !g_file_test(cover_path, G_FILE_TEST_EXISTS)) {
        return NULL;
    }

    GError *err = NULL;
    GdkPixbuf *pix = gdk_pixbuf_new_from_file_at_scale(cover_path, size, size, TRUE, &err);
    if (err) g_error_free(err);
    if (!pix) return NULL;

    GdkTexture *texture = pixbuf_to_texture(pix);
    g_object_unref(pix);
    return texture;
}

gboolean ui_cover_dominant_color(const char *cover_path, GdkRGBA *out) {
    if (!cover_path || !*cover_path || !out) return FALSE;
    if (!g_file_test(cover_path, G_FILE_TEST_EXISTS)) return FALSE;

    /* Уменьшаем обложку и собираем гистограмму квантованных цветов. */
    GError *err = NULL;
    GdkPixbuf *pix = gdk_pixbuf_new_from_file_at_scale(cover_path, 40, 40, TRUE, &err);
    if (err) g_error_free(err);
    if (!pix) return FALSE;

    const int width = gdk_pixbuf_get_width(pix);
    const int height = gdk_pixbuf_get_height(pix);
    const int nch = gdk_pixbuf_get_n_channels(pix);
    const int rowstride = gdk_pixbuf_get_rowstride(pix);
    const guchar *pixels = gdk_pixbuf_get_pixels(pix);
    const gboolean has_alpha = gdk_pixbuf_get_has_alpha(pix);

    enum { QB = 8 }; /* 8 уровней на канал → 512 корзин */
    double *weights = g_new0(double, QB * QB * QB);
    double *sum_r = g_new0(double, QB * QB * QB);
    double *sum_g = g_new0(double, QB * QB * QB);
    double *sum_b = g_new0(double, QB * QB * QB);
    double tot_r = 0, tot_g = 0, tot_b = 0, tot_w = 0;

    for (int y = 0; y < height; y++) {
        const guchar *row = pixels + (gsize)y * rowstride;
        for (int x = 0; x < width; x++) {
            const guchar *px = row + (gsize)x * nch;
            if (has_alpha && px[3] < 128) continue;

            const int r = px[0], g = px[1], b = px[2];
            const double mx = MAX(r, MAX(g, b));
            const double mn = MIN(r, MIN(g, b));
            const double v = mx / 255.0;
            const double s = mx > 0 ? (mx - mn) / mx : 0.0;

            /* Отбрасываем почти чёрные пиксели и почти белые серые. */
            if (v < 0.12) continue;
            if (v > 0.96 && s < 0.10) continue;

            /* Насыщенные пиксели важнее для «цвета» обложки. */
            const double weight = 1.0 + 3.0 * s;
            const int idx = ((r >> 5) * QB + (g >> 5)) * QB + (b >> 5);

            weights[idx] += weight;
            sum_r[idx] += r * weight;
            sum_g[idx] += g * weight;
            sum_b[idx] += b * weight;
            tot_r += r; tot_g += g; tot_b += b; tot_w += 1;
        }
    }
    g_object_unref(pix);

    int best = -1;
    double best_weight = 0;
    for (int i = 0; i < QB * QB * QB; i++) {
        if (weights[i] > best_weight) {
            best_weight = weights[i];
            best = i;
        }
    }

    if (best >= 0 && best_weight > 0) {
        out->red   = (sum_r[best] / best_weight) / 255.0;
        out->green = (sum_g[best] / best_weight) / 255.0;
        out->blue  = (sum_b[best] / best_weight) / 255.0;
    } else if (tot_w > 0) {
        out->red   = tot_r / tot_w / 255.0;
        out->green = tot_g / tot_w / 255.0;
        out->blue  = tot_b / tot_w / 255.0;
    } else {
        g_free(weights); g_free(sum_r); g_free(sum_g); g_free(sum_b);
        return FALSE;
    }

    g_free(weights); g_free(sum_r); g_free(sum_g); g_free(sum_b);

    /* Слегка поднимаем насыщенность, чтобы фон был выразительнее. */
    float h = 0, s = 0, v = 0;
    gtk_rgb_to_hsv(out->red, out->green, out->blue, &h, &s, &v);
    if (s > 0.05f && s < 0.45f) s = 0.45f;
    v = CLAMP(v, 0.30f, 0.75f);
    gtk_hsv_to_rgb(h, s, v, &out->red, &out->green, &out->blue);

    out->alpha = 1.0;
    return TRUE;
}

void ui_pin_tracks_reference(GtkWidget *widget, GPtrArray *tracks) {
    if (!widget || !tracks) return;
    g_object_set_data_full(G_OBJECT(widget), "context-owner",
                           g_ptr_array_ref(tracks), (GDestroyNotify)g_ptr_array_unref);
}

gint ui_compare_tracks_by_number(gconstpointer a, gconstpointer b) {
    const Track *t1 = *(const Track * const *)a;
    const Track *t2 = *(const Track * const *)b;

    /* Сначала диск, затем номер трека: так треки многодискового альбома идут
     * группами по дискам в правильном порядке. */
    if (t1->disc_number != t2->disc_number) {
        if (t1->disc_number == 0) return 1;
        if (t2->disc_number == 0) return -1;
        return (t1->disc_number < t2->disc_number) ? -1 : 1;
    }
    if (t1->track_number != t2->track_number) {
        if (t1->track_number == 0) return 1;
        if (t2->track_number == 0) return -1;
        return (t1->track_number < t2->track_number) ? -1 : 1;
    }
    return g_utf8_collate(t1->title ? t1->title : "", t2->title ? t2->title : "");
}

void ui_highlight_playing_in_list(GtkListBox *list, const char *playing_uri) {
    if (!list) return;
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(list));
    while (child) {
        if (GTK_IS_LIST_BOX_ROW(child)) {
            /* Разделители дисков не являются строками-треками: у них нет
             * данных "track", и их нельзя приводить к AdwActionRow. */
            Track *t = g_object_get_data(G_OBJECT(child), "track");
            if (t) {
                gboolean match = (playing_uri && g_strcmp0(t->uri, playing_uri) == 0);
                ui_set_row_playing(ADW_ACTION_ROW(child), match);
            }
        }
        child = gtk_widget_get_next_sibling(child);
    }
}

void ui_highlight_playing_on_page(GtkWidget *page, const char *playing_uri) {
    if (!page) return;
    /* Страница (в т.ч. вложенная) может держать несколько списков-строк; их
     * регистрируют под ключом "playing-listboxes" (GPtrArray<GtkListBox*>). */
    GPtrArray *lists = g_object_get_data(G_OBJECT(page), "playing-listboxes");
    if (!lists) return;
    for (guint i = 0; i < lists->len; i++) {
        ui_highlight_playing_in_list(g_ptr_array_index(lists, i), playing_uri);
    }
}
