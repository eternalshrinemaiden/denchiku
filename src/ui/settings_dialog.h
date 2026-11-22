#pragma once
#include <adwaita.h>

/* Категория тегов, к которой относится изменение настроек. */
typedef enum {
    SETTINGS_CATEGORY_ARTISTS,
    SETTINGS_CATEGORY_GENRES
} SettingsCategory;

/* Обратный вызов при изменении настроек tag-splitting. Вызывается после
 * сохранения с указанием изменившейся категории, чтобы вызывающая сторона
 * перестроила только соответствующий список. */
typedef void (*SettingsChangedCallback)(SettingsCategory category, gpointer user_data);

/* Открывает окно настроек, привязанное к parent. */
void settings_dialog_present(GtkWindow *parent,
                             SettingsChangedCallback on_changed,
                             gpointer user_data);
