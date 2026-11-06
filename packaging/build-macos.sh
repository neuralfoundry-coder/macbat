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
pkgbuild --quiet --root "$w/root" --scripts "$w/scripts" --identifier com.neuralfoundry.macbat \
         --version "$VERSION" --install-location / "dist/macbat-$VERSION-macos-universal.pkg"
rm -rf "$w"
ls -l dist/macbat-macos-universal "dist/macbat-$VERSION-macos-universal.pkg"
