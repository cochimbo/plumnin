#!/usr/bin/env bash
# Packs a Release build of plumnin into plumnin-<version>-x86_64.AppImage, one file that
# runs on most Linux desktops and on the Steam Deck (as a non-Steam game, without Proton).
#
#   tools/linux/make-appimage.sh <build dir> <version> [output dir]
#
# Inside the AppImage plumnin sits in usr/bin/ with its license. SDL2 and the other
# libraries it links to go to usr/lib/ (linuxdeploy picks them); the graphics drivers
# (libGL, libEGL, Mesa) and glibc stay the system's.
set -euo pipefail

build=${1:?usage: make-appimage.sh <build dir> <version> [output dir]}
version=${2:?usage: make-appimage.sh <build dir> <version> [output dir]}
out=${3:-.}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
build=$(cd "$build" && pwd)
mkdir -p "$out"
out=$(cd "$out" && pwd)

# Pinned versions, as the Windows installer's workflow pins its own.
LINUXDEPLOY_TAG=${LINUXDEPLOY_TAG:-1-alpha-20250213-2}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
app=$work/AppDir
bin=$app/usr/bin
mkdir -p "$bin"

install -m 755 "$build/plumnin" "$bin/plumnin"

cp "$root/LICENSE" "$bin/LICENSE.txt"

# The menu entry and the plum icon.
cp "$here/plumnin.desktop" "$work/plumnin.desktop"
cp "$root/src/frontend/assets/icon.png" "$work/plumnin.png"

curl -sSfL -o "$work/linuxdeploy" \
  "https://github.com/linuxdeploy/linuxdeploy/releases/download/$LINUXDEPLOY_TAG/linuxdeploy-x86_64.AppImage"
chmod +x "$work/linuxdeploy"

# Without FUSE (containers, some CI runners) the AppImage tools unpack themselves instead.
export APPIMAGE_EXTRACT_AND_RUN=1
export LINUXDEPLOY_OUTPUT_VERSION=$version
(cd "$work" && ./linuxdeploy --appdir "$app" \
  --executable "$bin/plumnin" \
  --desktop-file "$work/plumnin.desktop" \
  --icon-file "$work/plumnin.png" \
  --output appimage)

mv "$work"/plumnin-*.AppImage "$out/plumnin-$version-x86_64.AppImage"
echo "$out/plumnin-$version-x86_64.AppImage"
