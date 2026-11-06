#!/bin/sh
# Build the universal (arm64 + x86_64) macOS CLI, MacBat.app and an installer .pkg.  Run on macOS from the repo root.
# Signing: the first "Developer ID Application" identity (hardened runtime) or ad-hoc - override with MACBAT_SIGN_ID.
# Notarization: MACBAT_NOTARIZE=1 with APP_STORE_CONNECT_KEY_PATH / _API_KEY_ID / _ISSUER_ID (App Store Connect API key).
set -eu
VERSION=$(sed -n 's/^#define MACBAT_VERSION "\(.*\)"/\1/p' macbat.c)
ID="${MACBAT_SIGN_ID:-$(security find-identity -v -p codesigning 2>/dev/null | sed -n 's/.*"\(Developer ID Application: [^"]*\)".*/\1/p' | head -1)}"
[ -n "$ID" ] || ID=-
export MACBAT_SIGN_ID="$ID"
mkdir -p dist
clang -O2 -Wall -Wextra -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 \
      -o dist/macbat-macos-universal macbat.c -framework IOKit -framework CoreFoundation
if [ "$ID" = - ]; then codesign -s - -f dist/macbat-macos-universal          # ad-hoc (required on Apple Silicon)
else codesign --force --options runtime --timestamp --sign "$ID" dist/macbat-macos-universal; fi

packaging/build-app.sh
if [ "${MACBAT_NOTARIZE:-0}" = 1 ] && [ "$ID" != - ]; then
  # the zip holds the app and its copy of the CLI: both get notarized; the ticket is stapled to the app
  n=$(mktemp -d)
  ditto -c -k --keepParent dist/MacBat.app "$n/MacBat.zip"
  xcrun notarytool submit "$n/MacBat.zip" --key "$APP_STORE_CONNECT_KEY_PATH" --key-id "$APP_STORE_CONNECT_API_KEY_ID" \
        --issuer "$APP_STORE_CONNECT_ISSUER_ID" --wait --timeout 30m
  xcrun stapler staple dist/MacBat.app
  rm -rf "$n"
  spctl -a -t exec -vv dist/MacBat.app
fi
rm -f dist/MacBat-app.zip
ditto -c -k --keepParent dist/MacBat.app dist/MacBat-app.zip

w=$(mktemp -d)
mkdir -p "$w/root/usr/local/bin" "$w/root/Applications" "$w/scripts"
cp dist/macbat-macos-universal "$w/root/usr/local/bin/macbat"
ditto dist/MacBat.app "$w/root/Applications/MacBat.app"
cat > "$w/scripts/postinstall" <<'PI'
#!/bin/sh
/usr/local/bin/macbat install || echo "macbat: service not started - resolve the message above, then run: sudo macbat install"
# start the menu bar app for the user at the screen (it adds itself to the login items once)
u=$(stat -f %Su /dev/console 2>/dev/null || true)
if [ -n "$u" ] && [ "$u" != root ] && [ -d /Applications/MacBat.app ]; then
  launchctl asuser "$(id -u "$u")" sudo -u "$u" open -g /Applications/MacBat.app 2>/dev/null || true
fi
exit 0
PI
chmod 755 "$w/scripts/postinstall"
# macOS tags every new file with com.apple.provenance (cannot be removed) and pkgbuild turns
# xattrs into AppleDouble "._" entries. Build with pkgbuild, then replace Payload/Bom with
# clean ones made without extended attributes.
pkgbuild --quiet --root "$w/root" --scripts "$w/scripts" --identifier com.neuralfoundry.macbat \
         --version "$VERSION" --install-location / "$w/raw.pkg"
pkgutil --expand "$w/raw.pkg" "$w/x"
(cd "$w/root" && find . | COPYFILE_DISABLE=1 cpio -o --quiet --format odc -R 0:80 | gzip -9 -c) > "$w/x/Payload"
mkbom -s "$w/root" "$w/x/Bom"                  # -s: every entry owned by root:admin
nfiles=$(cd "$w/root" && find . | wc -l | tr -d ' ')
sed -i '' "s/numberOfFiles=\"[0-9]*\"/numberOfFiles=\"$nfiles\"/" "$w/x/PackageInfo"
rm -f "dist/macbat-$VERSION-macos-universal.pkg"
pkgutil --flatten "$w/x" "dist/macbat-$VERSION-macos-universal.pkg"
rm -rf "$w"
ls -l dist/macbat-macos-universal dist/MacBat-app.zip "dist/macbat-$VERSION-macos-universal.pkg"
