**macbat 0.3.0** — native, dependency-free builds for Linux (Intel MacBooks) and macOS.

### Install
```sh
curl -fsSL https://raw.githubusercontent.com/neuralfoundry-coder/macbat/main/install.sh | sudo sh
```
or grab a package below: `macbat_0.3.0_amd64.deb` (Ubuntu/Debian on Intel Macs) · `macbat-0.3.0-macos-universal.pkg` (macOS, Apple Silicon + Intel; unsigned — right-click → Open, or `sudo installer -pkg … -target /`).

### What's new
- Rewritten in C: a single static binary per platform, no Python or other runtime needed.
- `macbat` opens a retro **settings screen** covering battery limit / auto-discharge, screen-off-when-idle, display & keyboard brightness, lid / power button / idle sleep, SATA link power (ALPM) and disk spin-down, Wi-Fi power save, USB autosuspend, CPU governor & turbo, PCIe ASPM (Linux) — and display / system / disk sleep, Power Nap, low power mode (macOS, via `pmset`). Changes apply immediately and survive reboots (TLP-aware on Linux).
- Retro status page (`macbat status`), green or amber (`MACBAT_THEME=amber`), `--plain` / `--json` for scripts.
- Battery health is shown as "n/a (uncalibrated)" when the fuel gauge reports more than the design capacity, instead of a misleading >100 %.
- Refuses to touch charging while AlDente / Battery Toolkit is running (override with `--force`).
- `.deb` and `.pkg` packages, one-line installer with SHA-256 verification.

### Assets
| file | for |
|---|---|
| `macbat-linux-x86_64` | static Linux binary (Intel Macs) |
| `macbat_0.3.0_amd64.deb` | Debian / Ubuntu package |
| `macbat-macos-universal` | macOS binary (arm64 + x86_64, ad-hoc signed) |
| `macbat-0.3.0-macos-universal.pkg` | macOS installer package |
| `install.sh`, `SHA256SUMS` | one-line installer and checksums |
