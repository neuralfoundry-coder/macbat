#!/bin/sh
# Render macos/MacBat/AppIcon.svg into macos/MacBat/AppIcon.icns (needs rsvg-convert; iconutil ships with macOS).
set -eu
cd "$(dirname "$0")/MacBat"
set_dir=$(mktemp -d)/AppIcon.iconset
mkdir -p "$set_dir"
for s in 16 32 128 256 512; do
  rsvg-convert -w $s -h $s AppIcon.svg -o "$set_dir/icon_${s}x${s}.png"
  rsvg-convert -w $((s * 2)) -h $((s * 2)) AppIcon.svg -o "$set_dir/icon_${s}x${s}@2x.png"
done
iconutil -c icns "$set_dir" -o AppIcon.icns
rm -rf "$(dirname "$set_dir")"
ls -l AppIcon.icns
