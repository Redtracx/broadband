#!/usr/bin/env bash
# Bundles a cross-compiled broadband.exe with exactly the DLLs it needs into a
# ZIP that runs on any Windows 10/11 x64 machine without installing Qt.
#
# Usage:
#   scripts/package-windows.sh <build-dir> <qt-mingw-dir> [runtime-dir...]
#
#   build-dir     CMake build directory containing broadband.exe
#   qt-mingw-dir  Qt for Windows/MinGW, e.g. ~/Qt/6.8.3/mingw_64
#   runtime-dir   Extra directories to search for DLLs (MinGW runtime:
#                 libstdc++-6.dll, libgcc_s_seh-1.dll, libwinpthread-1.dll).
#                 Searched before Qt's own copy of the runtime.
#
# Required DLLs are found by following the import tables (objdump -p), so only
# what the program actually loads ends up in the package.
set -euo pipefail

if [[ $# -lt 2 ]]; then
    sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
fi

build_dir=$(realpath "$1")
qt_dir=$(realpath "$2")
shift 2
runtime_dirs=("$@")

objdump=${OBJDUMP:-x86_64-w64-mingw32-objdump}
command -v "$objdump" >/dev/null || { echo "error: $objdump not found (set OBJDUMP)" >&2; exit 1; }

exe="$build_dir/broadband.exe"
[[ -f $exe ]] || { echo "error: $exe not found" >&2; exit 1; }

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
version=$(sed -n 's/^ *VERSION \([0-9.]*\)$/\1/p' "$repo_dir/CMakeLists.txt" | head -1)
name="broadband-${version}-windows-x64"
out="$build_dir/$name"

rm -rf "$out" "$out.zip"
mkdir -p "$out"
cp "$exe" "$out/"

# Qt plugins that are loaded at runtime and therefore never show up as imports
plugins=(
    platforms/qwindows.dll          # native window system integration
    styles/qmodernwindowsstyle.dll  # native look on Windows 10/11
    tls/qschannelbackend.dll        # HTTPS via Windows' own TLS stack
)
for plugin in "${plugins[@]}"; do
    [[ -f $qt_dir/plugins/$plugin ]] || { echo "error: plugin $plugin not found" >&2; exit 1; }
    mkdir -p "$out/$(dirname "$plugin")"
    cp "$qt_dir/plugins/$plugin" "$out/$plugin"
done

search_dirs=("${runtime_dirs[@]}" "$qt_dir/bin" "$qt_dir")

find_dll() {
    local dll=$1 dir
    for dir in "${search_dirs[@]}"; do
        local match
        match=$(find "$dir" -maxdepth 1 -iname "$dll" -print -quit 2>/dev/null)
        [[ -n $match ]] && { echo "$match"; return 0; }
    done
    return 1
}

# Breadth-first walk over the import tables. DLLs that aren't found in the
# search directories are Windows system DLLs (kernel32.dll, ...).
queue=("$out/broadband.exe")
for plugin in "${plugins[@]}"; do queue+=("$out/$plugin"); done
declare -A seen=()
system_dlls=()
runtime_copied=()
while [[ ${#queue[@]} -gt 0 ]]; do
    file=${queue[0]}
    queue=("${queue[@]:1}")
    while read -r dll; do
        key=${dll,,}
        [[ -n ${seen[$key]:-} ]] && continue
        seen[$key]=1
        if src=$(find_dll "$dll"); then
            cp "$src" "$out/"
            # Toolchain runtime DLLs often ship with debug info; Qt's own DLLs
            # are left untouched.
            for dir in "${runtime_dirs[@]}"; do
                [[ $src == "$(realpath "$dir")"/* ]] && runtime_copied+=("$out/$(basename "$src")")
            done
            queue+=("$out/$(basename "$src")")
        else
            system_dlls+=("$key")
        fi
    done < <("$objdump" -p "$file" | sed -n 's/^\s*DLL Name: //p')
done

qt_version=$(sed -n 's/^QT_VERSION = //p' "$qt_dir/mkspecs/qconfig.pri" 2>/dev/null || true)

cp "$repo_dir/LICENSE" "$out/LICENSE.txt"
cat > "$out/README.txt" <<EOF
broadband $version - Bandbreiten- und Speedtest

Start: broadband.exe doppelklicken.
LAN-Server ohne GUI: broadband.exe --server [--port 8080]

Dieses Paket enthaelt Qt ${qt_version} (https://www.qt.io),
lizenziert unter der GNU LGPL v3 (https://www.gnu.org/licenses/lgpl-3.0.html),
sowie die GCC/MinGW-w64-Laufzeitbibliotheken (GPL mit Runtime Library Exception).
Quellcode: siehe Projekt-Repository.
EOF

strip=${STRIP:-x86_64-w64-mingw32-strip}
if command -v "$strip" >/dev/null; then
    "$strip" "$out/broadband.exe"
    for dll in "${runtime_copied[@]}"; do "$strip" --strip-unneeded "$dll"; done
fi

(cd "$build_dir" && rm -f "$name.zip" && zip -qr "$name.zip" "$name")

echo "Packaged files:"
(cd "$out" && find . -type f | sort | sed 's|^\./|  |')
echo "Windows system DLLs (not bundled): ${system_dlls[*]}"
echo "=> $build_dir/$name.zip ($(du -h "$build_dir/$name.zip" | cut -f1))"
