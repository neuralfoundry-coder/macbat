#!/bin/sh
# Build the static Linux x86_64 binary and the .deb inside a Debian container (works on arm64 hosts:
# uses the x86_64 cross compiler, no emulation).   usage: packaging/build-linux.sh  (run from repo root)
set -eu
VERSION=$(sed -n 's/^#define MACBAT_VERSION "\(.*\)"/\1/p' macbat.c)
docker run --rm -v "$PWD":/src -w /src debian:bookworm sh -euc "
  apt-get update -qq >/dev/null
  apt-get install -y -qq --no-install-recommends gcc-x86-64-linux-gnu libc6-dev-amd64-cross dpkg-dev >/dev/null
  mkdir -p dist
  x86_64-linux-gnu-gcc -O2 -Wall -Wextra -Wno-format-truncation -static -s -o dist/macbat-linux-x86_64 macbat.c
  # .deb: binary in /usr/bin, service installed/removed by maintainer scripts
  d=\$(mktemp -d); mkdir -p \$d/DEBIAN \$d/usr/bin \$d/usr/share/doc/macbat
  install -m 755 dist/macbat-linux-x86_64 \$d/usr/bin/macbat
  install -m 644 README.md LICENSE \$d/usr/share/doc/macbat/
  cat > \$d/DEBIAN/control <<CTL
Package: macbat
Version: $VERSION
Section: utils
Priority: optional
Architecture: amd64
Maintainer: neuralfoundry-coder <233372748+neuralfoundry-coder@users.noreply.github.com>
Homepage: https://github.com/neuralfoundry-coder/macbat
Description: battery charge limiter, fan control and status for Intel MacBooks on Linux
 Hardware charge limit (SMC BCLM), auto-discharge (ACEN) and fan control
 (applesmc) for Apple laptops running Linux, plus battery / fan status pages
 and a settings screen. Installs the macbat systemd service.
CTL
  printf '#!/bin/sh\nset -e\nif [ \"\$1\" = configure ]; then\n  if [ -d /sys/devices/platform/applesmc.768 ]; then /usr/bin/macbat install || echo \"macbat: service not started (see above)\"; else echo \"macbat: applesmc not found - not an Intel Mac? service not installed\"; fi\nfi\nexit 0\n' > \$d/DEBIAN/postinst
  printf '#!/bin/sh\nset -e\nif [ \"\$1\" = remove ] || [ \"\$1\" = purge ]; then /usr/bin/macbat uninstall || true; fi\nexit 0\n' > \$d/DEBIAN/prerm
  printf '#!/bin/sh\nset -e\nif [ \"\$1\" = purge ]; then rm -f /etc/macbat.conf /run/macbat.state; fi\nexit 0\n' > \$d/DEBIAN/postrm
  chmod 755 \$d/DEBIAN/postinst \$d/DEBIAN/prerm \$d/DEBIAN/postrm
  dpkg-deb --root-owner-group --build \$d dist/macbat_${VERSION}_amd64.deb >/dev/null
  chown -R $(id -u):$(id -g) dist
"
ls -l dist/macbat-linux-x86_64 dist/macbat_${VERSION}_amd64.deb
