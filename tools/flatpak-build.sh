#!/bin/sh
# Flatpak build (see SETUP.md): for the Steam Deck, SteamOS and other systems where installing
# build tools or libraries isn't possible. Builds a single-file bundle in builds/ that installs with
#   flatpak install --user ps3vis-<date>-<n>.flatpak
#
#   sh tools/flatpak-build.sh             build the bundle (asks before installing build tools)
#   sh tools/flatpak-build.sh --yes       install missing build tools without asking
#   sh tools/flatpak-build.sh --install   also install the result for this user and add it to the app menu
#
# Needs flatpak (preinstalled on SteamOS and most desktops). flatpak-builder is used when installed,
# otherwise its Flatpak (org.flatpak.Builder, from Flathub, per user: no root needed). The Freedesktop
# runtime and SDK are fetched from Flathub on the first build (about 1 GB, kept for later builds).
# Firmware is never included: the player reads RPCS3's dev_flash at runtime.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
app=io.github.ps3vis.Visualizer
manifest=packaging/flatpak/$app.yml

yes=0 install=0
for arg in "$@"; do
    case $arg in
    --yes | -y) yes=1 ;;
    --install) install=1 ;;
    -h | --help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown option: $arg (see --help)" >&2; exit 2 ;;
    esac
done

command -v flatpak >/dev/null 2>&1 || {
    echo "flatpak is missing. Install it with your distribution's package manager (it's preinstalled on SteamOS)." >&2
    exit 1
}
ask() { # question; returns 0 for yes
    [ "$yes" = 1 ] && return 0
    [ -t 0 ] || { echo "$1 Run again with --yes to allow it." >&2; return 1; }
    printf "%s [Y/n] " "$1"
    read -r answer
    case $answer in [nN]*) return 1 ;; esac
    return 0
}

# Flathub for this user (the runtime, the SDK and possibly the builder come from there).
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo

if command -v flatpak-builder >/dev/null 2>&1; then
    builder=flatpak-builder
else
    if ! flatpak info --user org.flatpak.Builder >/dev/null 2>&1 && ! flatpak info --system org.flatpak.Builder >/dev/null 2>&1; then
        ask "flatpak-builder isn't installed. Install org.flatpak.Builder from Flathub for this user?" || exit 1
        flatpak install --user -y --noninteractive flathub org.flatpak.Builder
    fi
    # (Inside its sandbox "--user" would mean the builder's private data folder: point it at the real one.)
    builder="flatpak run --env=FLATPAK_USER_DIR=${XDG_DATA_HOME:-$HOME/.local/share}/flatpak --command=flatpak-builder org.flatpak.Builder"
fi

# The runtime and SDK, installed from the host (the builder's own sandbox can't reach this user's
# Flathub remote, so --install-deps-from doesn't work with org.flatpak.Builder).
flatpak install --user -y --noninteractive --or-update flathub org.freedesktop.Platform//26.08 org.freedesktop.Sdk//26.08

# A clean copy of the sources (the same set as tools/package.sh): never devnotes/, USER-DATA/ or
# anything firmware-derived.
rm -rf build-flatpak/source
mkdir -p build-flatpak/source
tar --create --exclude='__pycache__' SETUP.md README.md CMakeLists.txt src third_party fonts docs packaging |
    tar --extract --directory build-flatpak/source

# Build into a local repository, then export a single-file bundle.
$builder --user --force-clean --ccache \
    --state-dir=build-flatpak/state --repo=build-flatpak/repo build-flatpak/app "$manifest"

mkdir -p builds # (ready-to-run programs go to builds/; dist/ is for source packages)
day=$(date +%Y%m%d)
n=1
while [ -e "builds/ps3vis-$day-$n.flatpak" ]; do n=$((n + 1)); done
bundle=builds/ps3vis-$day-$n.flatpak
flatpak build-bundle --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo build-flatpak/repo "$bundle" "$app"
echo
echo "Bundle: $root/$bundle ($(du -h "$bundle" | cut -f1))"
echo "Install it on any Linux PC or Steam Deck (Desktop Mode, Konsole) with:"
echo "  flatpak install --user $(basename "$bundle")"
echo "Run it from the app menu, or: flatpak run $app ~/Music"

if [ "$install" = 1 ]; then
    flatpak install --user -y --noninteractive --reinstall "$bundle"
    echo "Installed for this user: \"PS3 Visualizer\" is in the app menu."
fi
