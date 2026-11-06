**macbat 0.6.0** — MacBat, a menu bar app for macOS, and a one-time top-up to 100 %.

### Install / upgrade
```sh
curl -fsSL https://raw.githubusercontent.com/neuralfoundry-coder/macbat/main/install.sh | sudo sh
```
On macOS this now also installs **/Applications/MacBat.app** and starts it (`MACBAT_NO_APP=1` skips it). Upgrading keeps `/etc/macbat.conf`. Packages: `macbat_0.6.0_amd64.deb` (Ubuntu/Debian on Intel Macs) · `macbat-0.6.0-macos-universal.pkg` (the tool + MacBat.app; the package itself is unsigned — right-click → Open) · `MacBat-app.zip` (the app alone).

### What's new
- **MacBat.app** (macOS 13+, Apple Silicon + Intel): a template battery icon in the menu bar (charge level; bolt when charging, pause when held at the limit, arrow when auto-discharging; optional percentage) and a panel with the charge %, state, time left, a gauge with the limit, a charge-limit slider, auto-discharge, energy mode / Low Power Mode, **Top up to 100 % once**, battery details (power source and adapter watts, health, cycles, temperature, voltage / current), fans (auto / fixed speed), and a button that opens the full settings screen in Terminal. English and Korean. Starts at login (toggle in its ⋯ menu). The accent colour follows the macbat theme.
  - Reads as the normal user (`macbat status --json`). Changes go to the root service over **`/var/run/macbat.sock`** — root-owned, mode 0660, group admin, peer credentials checked on every connection (root, or an admin logged in at the screen), a fixed list of validated requests, each one logged. Without the service it falls back to the standard administrator password prompt.
  - With AlDente / Battery Toolkit running it shows that and stays read-only, unless you choose *Take over…*.
  - Signed with a Developer ID (hardened runtime) and **notarized**; the ticket is stapled to the app. The `macbat` binaries for macOS are Developer ID signed too.
  - Not tested on real hardware: writes from the app on macOS (the only Apple Silicon Mac available runs AlDente and stays read-only). The control socket protocol was tested with the Linux build.
- **`sudo macbat topup`**: charge to 100 % once, then back to the limit (`topup off` cancels; the service ends it at 100 % or after 12 hours). Also in the app.
- **Control socket** for scripts and the app, on Linux too (`/run/macbat.sock`, group sudo / admin / wheel): `printf 'limit 85\n' | nc -U /run/macbat.sock`.
- macOS settings screen: *Energy mode* (automatic / low power / high power) on macOS versions that use `powermode`.
- `macbat status --json`: `theme` and `conf.topup`.

### Assets
| file | for |
|---|---|
| `macbat-linux-x86_64` | static Linux binary (Intel Macs) |
| `macbat_0.6.0_amd64.deb` | Debian / Ubuntu package |
| `macbat-macos-universal` | macOS command-line tool (arm64 + x86_64, Developer ID signed) |
| `MacBat-app.zip` | MacBat.app, the menu bar app (notarized) |
| `macbat-0.6.0-macos-universal.pkg` | macOS installer package: tool + app |
| `install.sh`, `SHA256SUMS` | one-line installer and checksums |
