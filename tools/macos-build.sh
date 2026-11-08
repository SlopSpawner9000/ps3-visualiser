#!/bin/sh
# macOS build (see SETUP.md). Needs Homebrew (https://brew.sh). Checks for the build tools, SDL3,
# FFmpeg and dylibbundler, offers to install what's missing, builds ps3vis.app, bundles every
# Homebrew dylib it needs into it (so it doesn't depend on Homebrew at the target), ad-hoc signs
# it, and zips it into builds/ps3vis-macos.zip.
#
# The app isn't notarized (that needs a paid Apple Developer ID), so Gatekeeper will call it
# "unidentified developer" on first launch: right-click (or Control-click) it and choose Open.
#
#   sh tools/macos-build.sh             build (asks before installing packages)
#   sh tools/macos-build.sh --yes       install missing packages without asking
#   sh tools/macos-build.sh --clean     start from an empty build folder
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

yes=0 clean=0
for arg in "$@"; do
    case $arg in
    --yes | -y) yes=1 ;;
    --clean) clean=1 ;;
    -h | --help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown option: $arg (see --help)" >&2; exit 2 ;;
    esac
done

command -v brew >/dev/null 2>&1 || {
    echo "Homebrew is missing. Install it from https://brew.sh, then run this script again." >&2
    exit 1
}

# --- What's missing?
have() { command -v "$1" >/dev/null 2>&1; }
missing=""
have cmake || missing="$missing cmake"
have ninja || missing="$missing ninja"
have pkg-config || missing="$missing pkgconf"
have dylibbundler || missing="$missing dylibbundler"
if have pkg-config; then
    pkg-config --exists sdl3 || missing="$missing sdl3"
    pkg-config --exists libavformat libavcodec libswresample libavutil || missing="$missing ffmpeg"
else
    missing="$missing sdl3 ffmpeg" # can't check the libraries without it
fi

if [ -n "$missing" ]; then
    echo "Missing:$missing"
    cmd="brew install$missing"
    echo "Homebrew provides it:"
    echo "  $cmd"
    if [ "$yes" = 0 ]; then
        if [ ! -t 0 ]; then
            echo "Run that command (or this script with --yes), then run this script again." >&2
            exit 1
        fi
        printf "Install them now? [Y/n] "
        read -r answer
        case $answer in [nN]*) exit 1 ;; esac
    fi
    $cmd
fi

# --- Build.
work=build-macos
[ "$clean" = 1 ] && rm -rf "$work"
cmake -S . -B "$work" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS3VIS_BUILD_TARGET=macos-build
cmake --build "$work"

# --- Bundle: ps3vis.app with every Homebrew dylib it needs, so it runs without Homebrew installed.
out=$root/builds/ps3vis-macos # (ready-to-run programs go to builds/; dist/ is for source packages)
rm -rf "$out"
mkdir -p "$out"
cp -R "$work/ps3vis.app" "$out/"
app="$out/ps3vis.app"
mkdir -p "$app/Contents/Frameworks"
dylibbundler -od -b -x "$app/Contents/MacOS/ps3vis" -d "$app/Contents/Frameworks/" \
    -p "@executable_path/../Frameworks/"

# The themes' free fonts: next to the executable, and in Resources (SDL_GetBasePath's folder
# differs between a plain binary and an app bundle, so both are covered).
cp -r fonts "$app/Contents/MacOS/"
mkdir -p "$app/Contents/Resources"
cp -r fonts "$app/Contents/Resources/"

# Re-sign after dylibbundler rewrote the binary's library paths (ad-hoc: no paid Developer ID, so
# this isn't notarized - see the note at the top of this script).
codesign --force --deep -s - "$app"

cp README.md SETUP.md "$out/"
cat > "$out/START HERE.txt" <<'TXT'
PS3 Visualizer for macOS

1. Install RPCS3 (https://rpcs3.net) and in it choose File > Install Firmware with the official
   PS3UPDAT.PUP from playstation.com. This is a one-time step (see SETUP.md, part 1).
2. Right-click (or Control-click) ps3vis.app and choose Open. macOS will warn that it's from an
   unidentified developer (it isn't notarized) - choose Open anyway. This is only needed once.
3. If it asks, choose "Locate RPCS3 folder..." and pick the folder that contains RPCS3.app.
4. Drag music files or folders onto the window. Esc opens the menu.
TXT

cd "$root/builds"
rm -f ps3vis-macos.zip
ditto -c -k --sequesterRsrc --keepParent ps3vis-macos ps3vis-macos.zip
echo
echo "Built: $out/ps3vis.app"
echo "Zipped: $root/builds/ps3vis-macos.zip (to share)"
