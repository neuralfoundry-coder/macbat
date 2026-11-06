#!/bin/sh
# Build every release asset into dist/ and write SHA256SUMS.   usage: packaging/release.sh [--publish]
set -eu
VERSION=$(sed -n 's/^#define MACBAT_VERSION "\(.*\)"/\1/p' macbat.c)
rm -rf dist && mkdir -p dist
packaging/build-macos.sh
packaging/build-linux.sh
cp install.sh dist/install.sh
(cd dist && shasum -a 256 macbat-macos-universal MacBat-app.zip macbat-linux-x86_64 "macbat-$VERSION-macos-universal.pkg" \
                          "macbat_${VERSION}_amd64.deb" install.sh > SHA256SUMS && cat SHA256SUMS)
if [ "${1:-}" = --publish ]; then
  (cd dist && gh release create "v$VERSION" macbat-macos-universal MacBat-app.zip macbat-linux-x86_64 \
     "macbat-$VERSION-macos-universal.pkg" "macbat_${VERSION}_amd64.deb" install.sh SHA256SUMS \
     --title "macbat $VERSION" --notes-file ../packaging/RELEASE_NOTES.md)
fi
