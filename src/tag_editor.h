#pragma once
#include <gio/gio.h>

/* Внешний редактор тегов.
 *
 * Denchiku не редактирует теги сам: для правки метаданных он открывает
 * установленное пользователем приложение (например, Ear Tag, EasyTAG, Kid3).
 * Здесь — обнаружение подходящих приложений и запуск выбранного. */

/* Приложения, умеющие открывать аудиофайлы (кандидаты для выбора в настройках).
 * Возвращает GPtrArray<GAppInfo*>; владеет элементами (освобождать
 * g_ptr_array_unref). Отсортирован по отображаемому имени. */
GPtrArray *tag_editor_candidate_apps(void);

/* Запускает редактор editor (идентификатор .desktop, путь к .desktop-файлу или
 * путь к исполняемому файлу) со списком file:// URI. Возвращает FALSE и
 * заполняет error при неудаче. */
gboolean tag_editor_launch(const char *editor, GPtrArray *uris /* char* */,
                           GError **error);
