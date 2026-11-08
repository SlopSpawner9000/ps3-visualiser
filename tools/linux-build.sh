#!/bin/sh
# Linux build (see SETUP.md). Checks for the compiler, CMake, SDL3 and FFmpeg, offers to install
# what's missing with your distribution's package manager, and builds the player into build/.
#
#   sh tools/linux-build.sh             build (asks before installing packages)
#   sh tools/linux-build.sh --yes       install missing packages without asking
#   sh tools/linux-build.sh --install   also put it in ~/.local/bin and the app menu
#   sh tools/linux-build.sh --clean     start from an empty build folder
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

yes=0 install=0 clean=0
for arg in "$@"; do
    case $arg in
    --yes | -y) yes=1 ;;
    --install) install=1 ;;
    --clean) clean=1 ;;
    -h | --help) sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown option: $arg (see --help)" >&2; exit 2 ;;
    esac
done

# --- What's missing?
missing=""
have() { command -v "$1" >/dev/null 2>&1; }
have c++ || have g++ || have clang++ || missing="$missing compiler"
have cmake || missing="$missing cmake"
have ninja || have make || missing="$missing ninja"
if have pkg-config || have pkgconf; then
    pc=$(command -v pkg-config || command -v pkgconf)
    "$pc" --exists sdl3 || missing="$missing sdl3"
    "$pc" --exists libavformat libavcodec libswresample libavutil || missing="$missing ffmpeg"
else
    missing="$missing pkg-config sdl3 ffmpeg" # can't check the libraries without it
fi

# --- Offer to install it.
if [ -n "$missing" ]; then
    echo "Missing:$missing"
    id="" like=""
    if [ -r /etc/os-release ]; then
        id=$(. /etc/os-release && echo "${ID:-}")
        like=$(. /etc/os-release && echo "${ID_LIKE:-}")
    fi
    case " $id $like " in
    *" arch "*)
        cmd="sudo pacman -S --needed base-devel cmake ninja sdl3 ffmpeg pkgconf" ;;
    *" debian "* | *" ubuntu "*)
        cmd="sudo apt install build-essential cmake ninja-build pkg-config libsdl3-dev libavformat-dev libavcodec-dev libswresample-dev libavutil-dev" ;;
    *" fedora "* | *" rhel "*)
        cmd="sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config SDL3-devel ffmpeg-free-devel" ;;
    *" opensuse"* | *" suse "*)
        cmd="sudo zypper install gcc-c++ cmake ninja pkgconf-pkg-config SDL3-devel ffmpeg-devel" ;;
    *) cmd="" ;;
    esac
    if [ -z "$cmd" ]; then
        echo "Install a C++ compiler, CMake, Ninja (or make), pkg-config, and the development" >&2
        echo "packages of SDL3 and FFmpeg (libavformat, libavcodec, libswresample, libavutil)," >&2
        echo "then run this script again." >&2
        exit 1
    fi
    echo "These packages provide it:"
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
    case "$missing" in
    *sdl3*)
        if ! "${pc:-pkg-config}" --exists sdl3 2>/dev/null; then
            echo "SDL3 still isn't found. Older distributions (Ubuntu before 25.04, Debian before 13)" >&2
            echo "don't package it; build SDL3 from https://github.com/libsdl-org/SDL first." >&2
            exit 1
        fi ;;
    esac
fi

# --- Build.
[ "$clean" = 1 ] && rm -rf build
generator="Unix Makefiles"
have ninja && generator=Ninja
if [ -f build/CMakeCache.txt ] && ! grep -q "CMAKE_GENERATOR:INTERNAL=$generator" build/CMakeCache.txt; then
    rm -rf build # configured with another generator before
fi
cmake -S . -B build -G "$generator" -DCMAKE_BUILD_TYPE=Release -DPS3VIS_BUILD_TARGET=linux-build
cmake --build build --parallel
echo
echo "Built: $root/build/ps3vis"

# --- Install into the home folder (no root needed).
if [ "$install" = 1 ]; then
    bindir=${XDG_BIN_HOME:-$HOME/.local/bin}
    appdir=${XDG_DATA_HOME:-$HOME/.local/share}/applications
    mkdir -p "$bindir" "$appdir"
    install -m 755 build/ps3vis "$bindir/ps3vis"
    cat >"$appdir/ps3vis.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=PS3 Visualizer
Comment=Music player with the PS3 visualizer
Exec=$bindir/ps3vis %F
Terminal=false
Categories=AudioVideo;Audio;Player;
MimeType=audio/mpeg;audio/flac;audio/ogg;audio/x-wav;audio/mp4;audio/aac;audio/opus;inode/directory;
EOF
    have update-desktop-database && update-desktop-database "$appdir" >/dev/null 2>&1 || true
    echo "Installed: $bindir/ps3vis (and \"PS3 Visualizer\" in the app menu)"
    case ":$PATH:" in *":$bindir:"*) ;; *) echo "Note: $bindir isn't on your PATH; start it by its full path or from the app menu." ;; esac
fi

# --- Firmware check (the player also asks for it on start-up when it's missing).
found=""
for d in "$HOME/.config/rpcs3/dev_flash" "$HOME/.var/app/net.rpcs3.RPCS3/config/rpcs3/dev_flash"; do
    [ -d "$d" ] && found=$d && break
done
if [ -n "$found" ]; then
    echo "PS3 system software found: $found"
else
    echo "PS3 system software not found yet: install it with RPCS3 first (SETUP.md, step 1)."
fi
echo
echo "Play music:  ./build/ps3vis ~/Music"
