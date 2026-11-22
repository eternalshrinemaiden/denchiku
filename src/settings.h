#pragma once
#include <glib.h>

/* Настройки разбиения тегов (tag-splitting). Для исполнителей и жанров хранится
 * собственный список разделителей, который можно редактировать в окне настроек.
 * Хранятся в ~/.config/keruu/settings.ini. */

typedef struct {
    gboolean  enabled;     /* включено ли разбиение для этой категории */
    GPtrArray *separators; /* char* — пользуемые списком разделители */
} TagSplitCategory;

typedef struct {
    TagSplitCategory artists;
    TagSplitCategory genres;
} TagSplitSettings;

/* Глобальный синглтон настроек. При первом обращении значения по умолчанию уже
 * заданы; файл на диске подхватывается через settings_load(). */
TagSplitSettings *tag_split_settings(void);

/* Загружает настройки с диска (best-effort) и пересобирает кэш выражений. */
void settings_load(void);

/* Сохраняет настройки на диск (best-effort). */
void settings_save(void);

/* Пересобирает кэш скомпилированных выражений из текущих значений. Вызывать
 * после изменения tag_split_settings(). */
void settings_rebuild(void);

/* Скомпилированное выражение-разделитель для категории (artists: TRUE —
 * исполнители, FALSE — жанры) либо NULL, если разбиение выключено. */
GRegex *tag_split_regex(gboolean artists);

/* Список разделителей категории (artists: TRUE — исполнители, FALSE — жанры).
 * Возвращается внутренний массив, элементы — char*. */
GPtrArray *tag_split_separators(gboolean artists);

/* Добавляет разделитель, если его ещё нет (пустая строка игнорируется). */
void tag_split_add_separator(gboolean artists, const char *separator);

/* Удаляет разделитель, если он есть. */
void tag_split_remove_separator(gboolean artists, const char *separator);
