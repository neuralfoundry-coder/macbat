#!/bin/sh
# Build MacBat.app (menu bar app, macOS 13+, arm64 + x86_64) into dist/.  Run on macOS from the repo root after
# the CLI (dist/macbat-macos-universal) is built - the app carries a copy of it in Contents/Helpers.
# env: MACBAT_SIGN_ID  signing identity ("-" = ad-hoc; default: the first "Developer ID Application" identity)
set -eu
VERSION=$(sed -n 's/^#define MACBAT_VERSION "\(.*\)"/\1/p' macbat.c)
SRC=macos/MacBat
APP=dist/MacBat.app
w=$(mktemp -d)
SDK=$(xcrun --show-sdk-path)
for a in arm64 x86_64; do
  swiftc -O -swift-version 5 -parse-as-library -target "$a-apple-macos13.0" -sdk "$SDK" "$SRC"/Sources/*.swift -o "$w/MacBat-$a"
done
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Helpers"
lipo -create -output "$APP/Contents/MacOS/MacBat" "$w/MacBat-arm64" "$w/MacBat-x86_64"
cp dist/macbat-macos-universal "$APP/Contents/Helpers/macbat"
sed "s/@VERSION@/$VERSION/g" "$SRC/Info.plist" > "$APP/Contents/Info.plist"
cp -R "$SRC"/Resources/*.lproj "$APP/Contents/Resources/"
cp "$SRC/AppIcon.icns" "$APP/Contents/Resources/"
printf 'APPL????' > "$APP/Contents/PkgInfo"
xattr -cr "$APP"
rm -rf "$w"

ID="${MACBAT_SIGN_ID:-$(security find-identity -v -p codesigning 2>/dev/null | sed -n 's/.*"\(Developer ID Application: [^"]*\)".*/\1/p' | head -1)}"
if [ -n "$ID" ] && [ "$ID" != "-" ]; then
  # inside out: the helper, then the app (hardened runtime + secure timestamp, as notarization requires)
  codesign --force --options runtime --timestamp --sign "$ID" "$APP/Contents/Helpers/macbat"
  codesign --force --options runtime --timestamp --sign "$ID" "$APP"
else
  codesign --force --sign - "$APP/Contents/Helpers/macbat"
  codesign --force --sign - "$APP"
fi
codesign --verify --strict "$APP"
echo "built $APP ($VERSION, signed: ${ID:--})"
