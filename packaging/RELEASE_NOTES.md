**macbat 0.5.0** — themes, and a clean modern look by default.

### Install / upgrade
```sh
curl -fsSL https://raw.githubusercontent.com/neuralfoundry-coder/macbat/main/install.sh | sudo sh
```
Upgrading keeps `/etc/macbat.conf` (limit, auto-discharge, fan, …). Or grab a package below: `macbat_0.5.0_amd64.deb` (Ubuntu/Debian on Intel Macs) · `macbat-0.5.0-macos-universal.pkg` (macOS, Apple Silicon + Intel; unsigned — right-click → Open, or `sudo installer -pkg … -target /`).

### What's new
- **Themes** for `macbat status`, `macbat fan` and the settings screen:
  - `modern` — **the new default**: rounded frame, slim gauge, normal-weight text, one blue accent that reads on dark and light terminals.
  - `mono` (no colour), `ocean` (blue / aqua, gradient gauge), `candy` (pastel, dotted gauge), `nord` (Nord palette).
  - `retro` and `amber` — the green / amber CRT look of 0.3 and 0.4, unchanged.
- **Choosing a theme**: `--theme NAME` > `MACBAT_THEME` > `theme=` in `/etc/macbat.conf` > `modern`. Save one with `sudo macbat theme NAME` (or `default`), preview all with `macbat theme --list`, or pick it on the settings screen (*Appearance → Theme*: ← → previews live, even without sudo). Unknown names stop with the list of themes. Configs without `theme=` get the new default.
- **Colours follow the terminal**: truecolor (`COLORTERM=truecolor`), 256 colours, 16 colours on the Linux console (`TERM=linux`, fbterm / VT), none with `NO_COLOR`; `MACBAT_COLORS=16|256|truecolor|none` overrides. Every theme only uses glyphs the console's Terminus font has.
- `--plain` and `--json` output is unchanged.
- Screenshot tooling: `packaging/ansi2svg.py` understands 256 / truecolor and renders plain terminal windows (dark / light / VGA console) besides the CRT; `packaging/capture.py` no longer hangs when it has to stop a full-screen program.
- Fix: value text on the settings screen could be garbled on macOS (byte-wise `tolower` on UTF-8).

### Assets
| file | for |
|---|---|
| `macbat-linux-x86_64` | static Linux binary (Intel Macs) |
| `macbat_0.5.0_amd64.deb` | Debian / Ubuntu package |
| `macbat-macos-universal` | macOS binary (arm64 + x86_64, ad-hoc signed) |
| `macbat-0.5.0-macos-universal.pkg` | macOS installer package |
| `install.sh`, `SHA256SUMS` | one-line installer and checksums |
