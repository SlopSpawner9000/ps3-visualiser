#!/bin/sh
# Creates dist/ps3vis-<date>-<n>.zip and .tar.gz (n counts up, nothing is overwritten): everything someone needs to build and run the
# player (see SETUP.md). Leaves out firmware, personal data, notes and build output.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
day=$(date +%Y%m%d)
mkdir -p "$root/dist"
# Every package gets the next free number for the day (ps3vis-<date>-1, -2, ...); older ones stay.
n=1
while [ -e "$root/dist/ps3vis-$day-$n.zip" ] || [ -e "$root/dist/ps3vis-$day-$n.tar.gz" ]; do
    n=$((n + 1))
done
name=ps3vis-$day-$n
out=$root/dist/$name
stage=$root/dist/.stage

cd "$root"
rm -rf "$stage"
mkdir -p "$stage/$name"
tar --create --exclude='__pycache__' \
    SETUP.md README.md CMakeLists.txt .gitignore \
    src third_party fonts docs tools/re tools/package.sh tools/windows-build.sh tools/linux-build.sh tools/macos-build.sh tools/flatpak-build.sh tools/appimage-build.sh packaging |
    tar --extract --directory "$stage/$name"

(cd "$stage" && zip -qr "$out.zip" "$name" && tar --create --gzip --file "$out.tar.gz" "$name")
rm -rf "$stage"

echo "$out.zip"
echo "$out.tar.gz"
unzip -Z1 "$out.zip" | sed "s,^$name/,  ," | grep -v '/$'
