**macbat 0.4.0** — fan control, plus a fix for adapter toggling at the charge limit.

### Install / upgrade
```sh
curl -fsSL https://raw.githubusercontent.com/neuralfoundry-coder/macbat/main/install.sh | sudo sh
```
Upgrading keeps `/etc/macbat.conf` (limit, auto-discharge, …). Or grab a package below: `macbat_0.4.0_amd64.deb` (Ubuntu/Debian on Intel Macs) · `macbat-0.4.0-macos-universal.pkg` (macOS, Apple Silicon + Intel; unsigned — right-click → Open, or `sudo installer -pkg … -target /`).

### What's new
- **Fan control.** `macbat fan` shows every fan's speed, min / max, target and who controls it (SMC, mbpfan, macbat), plus the CPU temperature. `sudo macbat fan set 3000` (RPM) or `sudo macbat fan set 50%` (share of the fan's min..max) pins a speed; `sudo macbat fan auto` returns to automatic control. Also on the settings screen (new FANS section) and in `macbat status` / `--json`.
  - Speeds are always clamped to the fan's own min..max: fans are never slowed below the SMC minimum and can't be switched off.
  - The service re-applies the speed after boot / resume, hands the fans back to automatic control before sleep, on `restore` and when it stops, and has a **thermal guard**: at 90 °C CPU it switches to automatic until the CPU is back at 75 °C (Linux, Intel macOS).
  - **mbpfan:** left alone until you ask for a manual speed; then macbat stops and disables it (and says so), and `macbat fan auto` / `macbat uninstall` enable and start it again.
  - Linux uses the `applesmc` fan files (tested on a MacBook Pro 2015). **macOS fan writes are experimental** — implemented (Apple Silicon `F<n>Md` / `Ftst`, Intel `FS!`) but not tested on real hardware; reading fans on macOS is tested.
- **Fix: auto-discharge no longer toggles the adapter every 30 s** when the battery sits at the limit / limit + 1 % (seen with the SMC hardware limit on Linux). Auto-discharge now starts at limit + `hysteresis` (default 3 %) and still drains down to the limit.
- Settings screen: the key help line fits again when not running as root.
- New screenshots in the README (status, settings, fan page) — captured from a real MacBook Pro 2015 with `packaging/capture.py` + `packaging/ansi2svg.py`, which can now record the full-screen settings screen.

### Assets
| file | for |
|---|---|
| `macbat-linux-x86_64` | static Linux binary (Intel Macs) |
| `macbat_0.4.0_amd64.deb` | Debian / Ubuntu package |
| `macbat-macos-universal` | macOS binary (arm64 + x86_64, ad-hoc signed) |
| `macbat-0.4.0-macos-universal.pkg` | macOS installer package |
| `install.sh`, `SHA256SUMS` | one-line installer and checksums |
