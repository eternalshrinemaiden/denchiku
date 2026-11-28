#pragma once
#include <glib.h>

/* Пометки «любимое» на альбомах и исполнителях.
 *
 * В Spotify лайк альбома/исполнителя — это отметка самой сущности в библиотеке,
 * а не добавление её треков в плейлист «Любимые треки». Поэтому такие пометки
 * живут отдельно от playlist.c: здесь хранятся только ключи сущностей.
 *
 * Пометки — синглтон с ленивой загрузкой и записью при каждом изменении
 * (файл крошечный), по образцу settings.c. Изменения рассылаются слушателям,
 * чтобы разделы «Альбомы»/«Исполнители» перестраивали полки «Любимые». */

/* Альбомы идентифицируются тем же ключом, что и в разделе альбомов: пара
 * (album, album_artist). NULL-аргументы трактуются как "". */
gboolean favorites_album_is_liked(const char *album, const char *album_artist);
void     favorites_album_set_liked(const char *album, const char *album_artist,
                                   gboolean liked);

/* Исполнитель — по отображаемому имени (результат track_split_artists). */
gboolean favorites_artist_is_liked(const char *artist);
void     favorites_artist_set_liked(const char *artist, gboolean liked);

/* Слушатели изменений: вызываются при любой успешной смене пометки. */
typedef void (*FavoritesChangedFn)(gpointer user_data);
void     favorites_add_listener(FavoritesChangedFn cb, gpointer user_data);
void     favorites_remove_listener(FavoritesChangedFn cb, gpointer user_data);
