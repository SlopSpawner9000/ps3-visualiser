#!/bin/sh
# Windows build, run in Git Bash (see SETUP.md). Needs gcc, cmake and ninja (e.g. from Scoop:
# `scoop install git mingw cmake ninja`).
#
# Downloads the newest SDL3 MinGW development package and a prebuilt FFmpeg (LGPL, shared) into
# deps/, builds the player into build-win/, and puts ps3vis.exe with every DLL it needs into
# builds/ps3vis-windows/ and builds/ps3vis-windows.zip. That folder runs on any 64-bit
# Windows 10/11 PC; nothing else needs installing there.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "Missing '$1'. Install it with Scoop: scoop install $2" >&2
        exit 1
    }
}
need g++ mingw
need cmake cmake
need ninja ninja
need curl git

# Unpacks a zip into a folder (Git Bash may not have unzip; PowerShell always exists).
unpack() {
    if command -v unzip >/dev/null 2>&1; then
        unzip -q -o "$1" -d "$2"
    else
        powershell.exe -NoProfile -Command "Expand-Archive -Force -LiteralPath '$(cygpath -w "$1")' -DestinationPath '$(cygpath -w "$2")'"
    fi
}

# Prints the download URL of the newest asset of a GitHub release matching a pattern.
asset() { # repo, release ("latest" or a tag), grep -E pattern
    if [ "$2" = latest ]; then api=https://api.github.com/repos/$1/releases/latest
    else api=https://api.github.com/repos/$1/releases/tags/$2; fi
    curl -fsSL "$api" | grep -o '"browser_download_url": *"[^"]*"' | sed 's/.*"\(https[^"]*\)"/\1/' |
        grep -E "$3" | sort -V | tail -1
}

mkdir -p deps
if [ ! -d deps/SDL3 ]; then
    url=$(asset libsdl-org/SDL latest 'SDL3-devel-3\.[0-9.]+-mingw\.zip$')
    [ -n "$url" ] || { echo "Could not find the SDL3 download." >&2; exit 1; }
    echo "Downloading $url"
    curl -fL --progress-bar "$url" -o deps/sdl3.zip
    rm -rf deps/sdl3-tmp && mkdir deps/sdl3-tmp && unpack deps/sdl3.zip deps/sdl3-tmp
    mv deps/sdl3-tmp/SDL3-* deps/SDL3 && rm -rf deps/sdl3-tmp deps/sdl3.zip
fi
if [ ! -d deps/ffmpeg ]; then
    # A release branch (nX.Y), not master, so the API matches what the player was written for.
    url=$(asset BtbN/FFmpeg-Builds latest 'ffmpeg-n[0-9.]+-latest-win64-lgpl-shared-[0-9.]+\.zip$')
    [ -n "$url" ] || { echo "Could not find the FFmpeg download." >&2; exit 1; }
    echo "Downloading $url"
    curl -fL --progress-bar "$url" -o deps/ffmpeg.zip
    rm -rf deps/ffmpeg-tmp && mkdir deps/ffmpeg-tmp && unpack deps/ffmpeg.zip deps/ffmpeg-tmp
    mv deps/ffmpeg-tmp/ffmpeg-* deps/ffmpeg && rm -rf deps/ffmpeg-tmp deps/ffmpeg.zip
fi

sdl=$root/deps/SDL3/x86_64-w64-mingw32
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS3VIS_BUILD_TARGET=windows-build \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_PREFIX_PATH="$sdl" -DFFMPEG_DIR="$root/deps/ffmpeg"
cmake --build build-win

out=$root/builds/ps3vis-windows # (ready-to-run programs go to builds/; dist/ is for source packages)
rm -rf "$out"
mkdir -p "$out"
cp build-win/ps3vis.exe "$out/"
cp "$sdl/bin/SDL3.dll" "$out/"
cp deps/ffmpeg/bin/*.dll "$out/"
# The C++ runtime is linked statically; winpthread may still be a DLL.
gccbin=$(dirname "$(command -v g++)")
[ -f "$gccbin/libwinpthread-1.dll" ] && cp "$gccbin/libwinpthread-1.dll" "$out/"
cp README.md SETUP.md "$out/"
cp -r fonts "$out/" # (the themes' free fonts)

# Check that every DLL the program imports is either in the folder or part of Windows.
missing=""
for dll in $(objdump -p "$out/ps3vis.exe" | sed -n 's/^[[:space:]]*DLL Name: //p'); do
    [ -f "$out/$dll" ] && continue
    [ -f "/c/Windows/System32/$dll" ] && continue
    case "$dll" in api-ms-win-*|API-MS-WIN-*) continue ;; esac
    missing="$missing $dll"
done
if [ -n "$missing" ]; then
    echo "These DLLs are needed but weren't bundled:$missing" >&2
    echo "Please report this (and the output above)." >&2
    exit 1
fi
echo "All DLLs bundled: $(ls "$out"/*.dll | wc -l) files"
cat > "$out/START HERE.txt" <<'TXT'
PS3 Visualizer for Windows

0. If you got this as a zip: extract it first (right-click > Extract All). Starting ps3vis.exe
   from inside the zip fails with "...dll was not found".
1. Install RPCS3 (https://rpcs3.net) and in it choose File > Install Firmware with the official
   PS3UPDAT.PUP from playstation.com. This is a one-time step (see SETUP.md, part 1).
2. Double-click ps3vis.exe. If it asks, choose "Locate RPCS3 folder..." and pick the folder
   that contains rpcs3.exe.
3. Drag music files or folders onto the window. Esc opens the menu.

If Windows SmartScreen warns you, choose "More info" > "Run anyway" (the program isn't signed).
TXT

cd "$root/builds"
rm -f ps3vis-windows.zip
if command -v zip >/dev/null 2>&1; then
    zip -qr ps3vis-windows.zip ps3vis-windows
else
    powershell.exe -NoProfile -Command "Compress-Archive -Force -Path 'ps3vis-windows' -DestinationPath 'ps3vis-windows.zip'"
fi
echo
echo "Done: $root/builds/ps3vis-windows (double-click ps3vis.exe in this folder)"
echo "      $root/builds/ps3vis-windows.zip (to share)"
