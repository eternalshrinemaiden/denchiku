#pragma once
#include "track.h"
#include <adwaita.h>

/* Окно «Сведения»: только отображение метаданных трека (без правки — теги
 * меняются внешним приложением, см. tag_editor). */
void track_info_dialog_present(GtkWindow *parent, Track *track);
