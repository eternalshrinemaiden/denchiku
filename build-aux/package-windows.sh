#!/usr/bin/env bash
#
# Упаковка Denchiku для Windows (запускается внутри окружения MSYS2 UCRT64).
#
# Собирает самодостаточный каталог dist/denchiku: сам exe, все его зависимости
# из UCRT64, плагины GStreamer, тему иконок Adwaita и схемы GSettings. Каталог
# можно заархивировать и запускать на Windows без установленного MSYS2.
#
# Пример:
#   meson setup build-win --buildtype=release
#   meson compile -C build-win
#   ./build-aux/package-windows.sh build-win
set -euo pipefail

build_dir="${1:-build-win}"
prefix="${MINGW_PREFIX:-/ucrt64}"
dist="dist/denchiku"
exe="$build_dir/denchiku.exe"

if [ ! -f "$exe" ]; then
    echo "Не найден собранный $exe" >&2
    exit 1
fi

rm -rf dist
mkdir -p "$dist/lib"

cp -f "$exe" "$dist/"

declare -A copied=()

# Копирует DLL в dist/ ровно один раз (ключ — имя файла).
copy_dll() {
    local src="$1" base
    base="$(basename "$src")"
    if [ -n "${copied[$base]:-}" ]; then
        return 0
    fi
    copied[$base]=1
    cp -f "$src" "$dist/"
}

# Рекурсивно копирует зависимости переданных файлов, лежащие внутри UCRT64.
# Системные DLL Windows (kernel32 и т.п.) пропускаются — они уже есть в системе.
collect_deps() {
    local target dep base
    for target in "$@"; do
        while IFS= read -r dep; do
            base="$(basename "$dep")"
            if [ -n "${copied[$base]:-}" ]; then
                continue
            fi
            copy_dll "$dep"
            collect_deps "$dep"
        done < <(ldd "$target" 2>/dev/null | awk '{print $3}' | grep -E "^${prefix}/" || true)
    done
}

echo "==> Зависимости denchiku.exe"
collect_deps "$exe"

echo "==> Плагины GStreamer"
gst_plugins_src="$(pkg-config --variable=pluginsdir gstreamer-1.0)"
if [ -d "$gst_plugins_src" ]; then
    mkdir -p "$dist/lib/gstreamer-1.0"
    cp -f "$gst_plugins_src"/*.dll "$dist/lib/gstreamer-1.0/"
    shopt -s nullglob
    plugin_dlls=("$dist"/lib/gstreamer-1.0/*.dll)
    shopt -u nullglob
    if [ ${#plugin_dlls[@]} -gt 0 ]; then
        collect_deps "${plugin_dlls[@]}"
    fi
else
    echo "Каталог плагинов GStreamer не найден: $gst_plugins_src" >&2
    exit 1
fi

echo "==> Сканер плагинов GStreamer"
for scanner in gst-plugin-scanner.exe gst-plugin-scanner; do
    if [ -f "$prefix/bin/$scanner" ]; then
        cp -f "$prefix/bin/$scanner" "$dist/"
        collect_deps "$dist/$scanner"
        break
    fi
done

echo "==> Тема иконок Adwaita"
if [ -d "$prefix/share/icons/Adwaita" ]; then
    mkdir -p "$dist/share/icons"
    cp -r "$prefix/share/icons/Adwaita" "$dist/share/icons/"
fi
if [ -d "$prefix/share/icons/hicolor" ]; then
    cp -r "$prefix/share/icons/hicolor" "$dist/share/icons/"
fi

echo "==> Схемы GSettings"
if [ -f "$prefix/share/glib-2.0/schemas/gschemas.compiled" ]; then
    mkdir -p "$dist/share/glib-2.0/schemas"
    cp -f "$prefix/share/glib-2.0/schemas/gschemas.compiled" \
        "$dist/share/glib-2.0/schemas/"
fi

echo "==> Готово: $dist"
du -sh "$dist" || true
