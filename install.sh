#!/bin/sh
# macbat one-line installer - needs only sh + curl (or wget). No Python, no compiler, no packages.
#
#   curl -fsSL https://raw.githubusercontent.com/neuralfoundry-coder/macbat/main/install.sh | sudo sh
#
# env:  MACBAT_VERSION=0.2.0   install a specific release (default: latest)
#       MACBAT_DRY_RUN=1       download + verify only, change nothing (no root needed)
# extra args go to 'macbat install', e.g.:  ... | sudo sh -s -- --force
set -eu
REPO="neuralfoundry-coder/macbat"
VER="${MACBAT_VERSION:-latest}"
say()  { printf 'macbat-installer: %s\n' "$*"; }
fail() { say "$*" >&2; exit 1; }

os=$(uname -s); arch=$(uname -m)
case "$os" in
  Linux)  [ "$arch" = x86_64 ] || fail "the Linux build is for Intel Macs (x86_64); this machine is $arch"
          asset="macbat-linux-x86_64" ;;
  Darwin) asset="macbat-macos-universal" ;;
  *)      fail "unsupported OS: $os" ;;
esac
if [ "$VER" = latest ]; then base="https://github.com/$REPO/releases/latest/download"
else base="https://github.com/$REPO/releases/download/v$VER"; fi

fetch() {
  if command -v curl >/dev/null 2>&1; then curl -fsSL "$1" -o "$2"
  elif command -v wget >/dev/null 2>&1; then wget -q "$1" -O "$2"
  else fail "need curl or wget"; fi
}
sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

tmp=$(mktemp -d 2>/dev/null || mktemp -d -t macbat)
trap 'rm -rf "$tmp"' EXIT INT TERM
say "downloading $asset ($VER)"
fetch "$base/$asset" "$tmp/macbat"
fetch "$base/SHA256SUMS" "$tmp/SHA256SUMS"
want=$(awk -v a="$asset" '$2==a || $2==("*" a) {print $1}' "$tmp/SHA256SUMS")
got=$(sha256 "$tmp/macbat")
{ [ -n "$want" ] && [ "$want" = "$got" ]; } || fail "checksum mismatch for $asset"
say "checksum OK"
chmod 755 "$tmp/macbat"
v=$("$tmp/macbat" --version 2>/dev/null) || fail "the downloaded binary does not run on this machine"
if [ "${MACBAT_DRY_RUN:-0}" = 1 ]; then say "dry run: $v verified, nothing installed"; exit 0; fi

[ "$(id -u)" = 0 ] || fail "please run as root:  curl -fsSL https://raw.githubusercontent.com/$REPO/main/install.sh | sudo sh"
mkdir -p /usr/local/bin
cp "$tmp/macbat" /usr/local/bin/macbat.new
chmod 755 /usr/local/bin/macbat.new
mv -f /usr/local/bin/macbat.new /usr/local/bin/macbat
say "installed /usr/local/bin/macbat ($v)"
/usr/local/bin/macbat install "$@"
say "done - run 'macbat' to see the battery status"
