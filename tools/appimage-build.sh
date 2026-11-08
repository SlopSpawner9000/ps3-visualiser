#!/bin/sh
# AppImage build (see SETUP.md): one file you build here and copy to a Steam Deck, SteamOS or any
# other x86-64 Linux, where it runs without installing anything (chmod +x, then start it).
#
#   sh tools/appimage-build.sh          build builds/ps3vis-<date>-<n>-x86_64.AppImage, plus the same
#                                       as a portable folder (…-portable.tar.gz) that needs no FUSE
#   sh tools/appimage-build.sh --yes    install the build environment without asking
#
# An AppImage runs only on systems whose C library (glibc) is at least as new as the one it was built
# against, so it isn't built on this (possibly very new) system directly: the build runs inside the
# Freedesktop 23.08 SDK (glibc 2.38; gcc 13), used through flatpak as a ready-made build system, no
# root needed. That covers SteamOS 3.5+, Ubuntu 24.04+, Debian 13+, Fedora 39+ and newer. SDL3 and
# FFmpeg (audio decoding only) are built from source and bundled; the C++ runtime is linked in.
# Needs: flatpak and curl. appimagetool is downloaded on first use. Firmware is never included.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
sdk=org.freedesktop.Sdk//23.08
work=$root/build-appimage
ffmpeg=ffmpeg-9.0.2
ffmpegSha=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
sdl=SDL3-3.4.18
sdlSha=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3

yes=0
for arg in "$@"; do
    case $arg in
    --yes | -y) yes=1 ;;
    -h | --help) sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown option: $arg (see --help)" >&2; exit 2 ;;
    esac
done
for tool in flatpak curl sha256sum; do
    command -v $tool >/dev/null 2>&1 || { echo "Missing '$tool'." >&2; exit 1; }
done

# The build environment (about 1 GB, once).
if ! flatpak info --user $sdk >/dev/null 2>&1 && ! flatpak info --system $sdk >/dev/null 2>&1; then
    if [ "$yes" = 0 ]; then
        [ -t 0 ] || { echo "The build needs $sdk (about 1 GB). Run again with --yes to install it." >&2; exit 1; }
        printf "The build needs the Freedesktop 23.08 SDK from Flathub (about 1 GB, for this user). Install it? [Y/n] "
        read -r answer
        case $answer in [nN]*) exit 1 ;; esac
    fi
    flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
    flatpak install --user -y --noninteractive flathub $sdk
fi

# Sources (checked against known checksums).
mkdir -p "$work/downloads"
fetch() { # file url sha256
    if [ ! -f "$work/downloads/$1" ] || ! echo "$3  $work/downloads/$1" | sha256sum -c --status; then
        echo "Downloading $2"
        curl -fL --progress-bar -o "$work/downloads/$1" "$2"
        echo "$3  $work/downloads/$1" | sha256sum -c --status || { echo "Checksum mismatch for $1" >&2; exit 1; }
    fi
}
fetch $ffmpeg.tar.xz https://ffmpeg.org/releases/$ffmpeg.tar.xz $ffmpegSha
fetch $sdl.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/$sdl.tar.gz $sdlSha

# Build inside the SDK. Libraries go to $work/prefix and are only rebuilt when missing.
cat >"$work/build-inside.sh" <<EOF
set -eu
work="$work"
root="$root"
jobs=\$(nproc)
prefix=\$work/prefix
export PKG_CONFIG_PATH=\$prefix/lib/pkgconfig
cd "\$work"
if [ ! -f \$prefix/lib/libavformat.so ]; then
    rm -rf $ffmpeg && tar -xf downloads/$ffmpeg.tar.xz
    cd $ffmpeg
    # Audio decoding only, and no optional system libraries (the target may not have them).
    ./configure --prefix=\$prefix --enable-shared --disable-static --disable-programs --disable-doc \\
        --disable-avdevice --disable-avfilter --disable-swscale --disable-network --disable-encoders \\
        --disable-muxers --disable-devices --disable-hwaccels --disable-x86asm --disable-debug \\
        --disable-autodetect --enable-zlib
    make -j\$jobs && make install
    cd "\$work"
fi
if [ ! -f \$prefix/lib/libSDL3.so ]; then
    rm -rf $sdl && tar -xf downloads/$sdl.tar.gz
    cmake -S $sdl -B $sdl/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=\$prefix \\
        -DCMAKE_INSTALL_LIBDIR=lib -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
    cmake --build $sdl/build && cmake --install $sdl/build
fi
# The player, with the C++ runtime linked in (targets may have an older libstdc++).
cmake -S "\$root" -B "\$work/ps3vis" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=\$prefix \\
    -DPS3VIS_BUILD_TARGET=appimage \\
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
cmake --build "\$work/ps3vis"
EOF
flatpak run --filesystem="$root" --command=sh $sdk "$work/build-inside.sh"

# The AppDir: the player, the bundled libraries, a launcher, the desktop entry and the icon.
app=$work/AppDir
rm -rf "$app"
mkdir -p "$app/usr/bin" "$app/usr/lib"
cp "$work/ps3vis/ps3vis" "$app/usr/bin/"
mkdir -p "$app/usr/share/ps3vis" && cp -r fonts "$app/usr/share/ps3vis/" # (the themes' free fonts)
for lib in libSDL3.so.0 libavformat.so.* libavcodec.so.* libavutil.so.* libswresample.so.*; do
    for f in "$work"/prefix/lib/$lib; do
        [ -e "$f" ] && cp -P "$f" "$app/usr/lib/"
    done
done
# (copy the real files the version symlinks point to as well)
for f in "$work"/prefix/lib/libSDL3.so.0.* "$work"/prefix/lib/libav*.so.*.* "$work"/prefix/lib/libswresample.so.*.*; do
    [ -e "$f" ] && cp "$f" "$app/usr/lib/"
done
cat >"$app/AppRun" <<'EOF'
#!/bin/sh
# Use the bundled SDL3 and FFmpeg; everything else (graphics drivers, audio) comes from the system.
here=$(dirname "$(readlink -f "$0")")
export LD_LIBRARY_PATH="$here/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# apprun.log next to the settings: the launch environment and, when there's no terminal to show them
# (e.g. Steam's Game Mode), the program's own error output, including loader errors before it starts.
logdir="${XDG_DATA_HOME:-$HOME/.local/share}/ps3vis/PS3 Visualizer"
mkdir -p "$logdir" 2>/dev/null
{
    echo "AppRun $(date '+%F %T'): $APPIMAGE"
    echo "args: $*"
    echo "desktop ${XDG_CURRENT_DESKTOP:-?}, session ${XDG_SESSION_TYPE:-?}, WAYLAND_DISPLAY ${WAYLAND_DISPLAY:-}, DISPLAY ${DISPLAY:-}, gamescope ${GAMESCOPE_WAYLAND_DISPLAY:-}, SteamGamepadUI ${SteamGamepadUI:-}"
    echo "LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
    echo "LD_PRELOAD=${LD_PRELOAD:-}"
    echo "SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-} SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-}"
} >"$logdir/apprun.log" 2>/dev/null
if [ -t 2 ]; then
    exec "$here/usr/bin/ps3vis" "$@"
fi
exec "$here/usr/bin/ps3vis" "$@" >>"$logdir/apprun.log" 2>&1
EOF
chmod +x "$app/AppRun"
sed 's/^Icon=.*/Icon=ps3vis/' packaging/flatpak/io.github.ps3vis.Visualizer.desktop >"$app/ps3vis.desktop"
cp packaging/flatpak/io.github.ps3vis.Visualizer.svg "$app/ps3vis.svg"
ln -sf ps3vis.svg "$app/.DirIcon"

# appimagetool (downloaded once; run extracted so FUSE isn't needed on this machine).
tool=$work/appimagetool-x86_64.AppImage
if [ ! -x "$tool" ]; then
    echo "Downloading appimagetool"
    curl -fL --progress-bar -o "$tool" https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x "$tool"
fi
mkdir -p builds # (ready-to-run programs go to builds/; dist/ is for source packages)
day=$(date +%Y%m%d)
n=1
while [ -e "builds/ps3vis-$day-$n-x86_64.AppImage" ]; do n=$((n + 1)); done
out=builds/ps3vis-$day-$n-x86_64.AppImage
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$tool" --no-appstream "$app" "$out"

# The same files as a plain portable folder: no self-mounting (FUSE), which some launchers (e.g. a
# Steam shortcut running in Steam's container) can't provide. Unpack it and start ps3vis/ps3vis.sh.
portable=builds/ps3vis-$day-$n-x86_64-portable.tar.gz
rm -rf "$work/portable"
mkdir -p "$work/portable"
cp -a "$app" "$work/portable/ps3vis"
rm -f "$work/portable/ps3vis/.DirIcon"
cp "$app/AppRun" "$work/portable/ps3vis/ps3vis.sh"
tar -C "$work/portable" -czf "$portable" ps3vis

# The oldest glibc the result needs (it runs on systems with this version or newer).
need=$(for f in "$app/usr/bin/ps3vis" "$app"/usr/lib/*.so.*; do objdump -T "$f" 2>/dev/null; done |
    grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
echo
echo "AppImage: $root/$out ($(du -h "$out" | cut -f1)), needs ${need:-glibc ?} or newer"
echo "Copy it to the target (e.g. a Steam Deck), then: chmod +x $(basename "$out") and run it."
echo "Portable folder (no FUSE needed): $root/$portable ($(du -h "$portable" | cut -f1))"
echo "  unpack it (tar -xzf $(basename "$portable")) and start ps3vis/ps3vis.sh"
