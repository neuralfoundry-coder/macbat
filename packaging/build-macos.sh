#!/bin/sh
# Build the universal (arm64 + x86_64) macOS binary and an installer .pkg.  Run on macOS from the repo root.
set -eu
VERSION=$(sed -n 's/^#define MACBAT_VERSION "\(.*\)"/\1/p' macbat.c)
mkdir -p dist
clang -O2 -Wall -Wextra -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 \
      -o dist/macbat-macos-universal macbat.c -framework IOKit -framework CoreFoundation
codesign -s - -f dist/macbat-macos-universal          # ad-hoc signature (required on Apple Silicon)
w=$(mktemp -d)
mkdir -p "$w/root/usr/local/bin" "$w/scripts"
cp dist/macbat-macos-universal "$w/root/usr/local/bin/macbat"
cat > "$w/scripts/postinstall" <<'PI'
#!/bin/sh
/usr/local/bin/macbat install || echo "macbat: service not started - resolve the message above, then run: sudo macbat install"
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
ls -l dist/macbat-macos-universal "dist/macbat-$VERSION-macos-universal.pkg"
