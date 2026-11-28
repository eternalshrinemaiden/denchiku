project('denchiku', 'c',
  version: '0.2.0',
  default_options: ['warning_level=2', 'c_std=c11']
)

deps = [
  dependency('libadwaita-1'),
  dependency('gstreamer-1.0'),
  dependency('gstreamer-pbutils-1.0'),
  dependency('gdk-pixbuf-2.0'),
]

inc = include_directories('src', 'src/ui')

gnome = import('gnome')

resources = gnome.compile_resources(
  'denchiku-resources',
  'src/denchiku.gresource.xml',
  source_dir: 'src',
  c_name: 'denchiku',
)

sources = files(
  'src/main.c',
  'src/track.c',
  'src/cache.c',
  'src/scanner.c',
  'src/watcher.c',
  'src/playlist.c',
  'src/favorites.c',
  'src/player.c',
  'src/state.c',
  'src/settings.c',
  'src/stats.c',
  'src/tag_editor.c',
  'src/ui/ui_helpers.c',
  'src/ui/track_menu.c',
  'src/ui/track_info_dialog.c',
  'src/ui/track_list.c',
  'src/ui/view_controls.c',
  'src/ui/media_browser.c',
  'src/ui/album_detail_page.c',
  'src/ui/artist_detail_page.c',
  'src/ui/albums_view.c',
  'src/ui/artists_view.c',
  'src/ui/genres_view.c',
  'src/ui/tracks_view.c',
  'src/ui/playlists_view.c',
  'src/ui/library_view.c',
  'src/ui/queue_view.c',
  'src/ui/player_bar.c',
  'src/ui/settings_dialog.c',
  'src/ui/window.c',
)

executable('denchiku', sources, resources,
  include_directories: inc,
  dependencies: deps,
  install: true,
)

# Файлы интеграции с рабочим столом нужны для Flatpak и системной установки.
install_data('data/org.svyata.denchiku.desktop',
  install_dir: get_option('datadir') / 'applications',
)
install_data('data/org.svyata.denchiku.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo',
)
install_data('data/icons/org.svyata.denchiku.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)
