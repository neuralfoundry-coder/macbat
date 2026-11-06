# macbat

**Battery charge limiter, battery status and power settings for Apple laptops** — Intel MacBooks running Linux, and macOS (Apple Silicon / Intel).
One small native binary, no dependencies, retro green-screen UI.

![macbat status on a MacBook Pro 2015 running Ubuntu](docs/linux-status.svg)

- **Charge limit** — keep the battery at e.g. 80 % to slow down wear.
  Linux / Intel Macs use the SMC's own hardware limit (`BCLM`); Apple Silicon pauses charging through the SMC (`CHTE`).
- **Auto-discharge** — above the limit, macbat bypasses the AC adapter so the battery drains down to the limit *while plugged in*, then hands power back to the adapter.
- **Battery status** — charge, mode, adapter, health, cycles, voltage / current / power, temperature, time left.
- **Settings screen** — run `macbat` and change everything with the arrow keys: charge limit, screen-off-when-idle, brightness, keyboard backlight, lid / power button / idle sleep, disk power saving, Wi-Fi / USB / CPU / PCIe power saving. Changes apply immediately and survive reboots.

![macbat settings screen](docs/linux-tui.svg)

## Install

One line, needs only `sh` and `curl` (or `wget`) — the right binary for your OS is downloaded from the latest release and checked against `SHA256SUMS`:

```sh
curl -fsSL https://raw.githubusercontent.com/neuralfoundry-coder/macbat/main/install.sh | sudo sh
```

It installs `/usr/local/bin/macbat` and starts the background service (limit 80 %, auto-discharge on).

- Check first without changing anything: `curl -fsSL …/install.sh | MACBAT_DRY_RUN=1 sh`
- Specific version: `… | sudo MACBAT_VERSION=0.3.0 sh`

**Packages** (from the [releases page](https://github.com/neuralfoundry-coder/macbat/releases)):

| | |
|---|---|
| Ubuntu / Debian (Intel Mac) | `sudo apt install ./macbat_0.3.0_amd64.deb` |
| macOS | `macbat-0.3.0-macos-universal.pkg` — unsigned: right-click → Open, or `sudo installer -pkg macbat-0.3.0-macos-universal.pkg -target /` |

**From source:** `make && sudo ./macbat install` (Linux needs a C compiler; macOS needs the Xcode command line tools).

## Usage

| | |
|---|---|
| `macbat` | settings screen (read-only unless run with `sudo`; press **S** to relaunch with sudo) |
| `macbat status` | status page — add `--plain` or `--json` for scripts |
| `sudo macbat limit 80` | charge limit 20–100 (`off` = 100) |
| `sudo macbat discharge on\|off` | auto-discharge down to the limit while plugged in |
| `sudo macbat restore` | resume charging and re-enable the adapter right now |
| `sudo macbat install` / `uninstall` | add / remove the service (uninstall restores 100 % and normal charging) |
| `macbat keys` | the relevant SMC keys (diagnostics) |

Settings screen keys: **↑ ↓** select · **← →** change · **Enter** apply / run · **R** refresh · **Q** quit.
Colours: `MACBAT_THEME=amber` for an amber screen, `NO_COLOR=1` for none.

### What the settings screen controls

| Section | Linux (Intel Mac) | macOS |
|---|---|---|
| Battery | charge limit, auto-discharge, resume charging now | same |
| Display | screen off when idle (console blanking), display brightness, keyboard backlight | display sleep |
| Sleep | lid close, power button, sleep when idle (systemd-logind) | system sleep, Power Nap, low power mode |
| Disk | SATA link power management (ALPM), disk spin-down (rotational disks) | disk sleep |
| Power saving | Wi-Fi power save, USB autosuspend, CPU governor, CPU turbo, PCIe ASPM | — |

How settings persist on Linux: battery and screen blanking live in `/etc/macbat.conf` and are re-applied by the service at boot; lid / power button / idle go to `/etc/systemd/logind.conf.d/90-macbat.conf`; brightness is restored by systemd. If **TLP** is installed, disk / Wi-Fi / USB / CPU / PCIe settings are written to `/etc/tlp.d/90-macbat.conf` so TLP and macbat never fight; otherwise the macbat service re-applies them at boot. On macOS the settings go through `pmset`.

`/etc/macbat.conf`:

```ini
limit=80
auto_discharge=on
hysteresis=3      # software limit (Apple Silicon): resume charging at limit-3 %
interval=30       # seconds between checks
screen_blank=600  # set from the settings screen
```

## How it works

macbat talks to the System Management Controller directly and only uses keys it finds on the machine.

| Platform | Charge limit | Auto-discharge (bypass adapter) | Battery data |
|---|---|---|---|
| Linux, Intel Mac | `BCLM` — enforced by the SMC itself | `ACEN`=0 (or `CH0I`=1) | `/sys/class/power_supply`, SMC |
| macOS, Apple Silicon | `CHTE` (or `CH0B`/`CH0C`), toggled by the service with hysteresis | `CHIE`=0x08 (or `CH0I`) | `AppleSmartBattery`, SMC |
| macOS, Intel | `BCLM` | `CH0I` | `AppleSmartBattery`, SMC |

- **Linux**: reads go through the `applesmc` driver. Writes use the SMC I/O ports (0x300/0x304) with the same protocol as the kernel driver; while writing, macbat briefly stops `mbpfan` and unloads `applesmc` so nothing else talks to the SMC, then reloads it, restarts `mbpfan` and verifies the value. Writes only happen when something changes. Requires kernel lockdown to be off (the default on Macs, which do not Secure-Boot Linux).
- **macOS**: uses the `AppleSMC` IOKit user client and IOKit sleep notifications.

### Safety

- When the service stops — or crashes (`ExecStopPost` on Linux) — charging and the adapter go back to normal.
- Before the machine sleeps, charging and the adapter go back to normal (systemd sleep hook / IOKit sleep notification), so a plugged-in laptop never drains while asleep.
- Below 10 % macbat never pauses charging or bypasses the adapter.
- If **AlDente** or **Battery Toolkit** is running, macbat refuses to touch charging (`--force` overrides) and says so on the status page.
- Changing the lid action is refused while the lid is closed (it would take effect immediately).
- Battery health is reported as *n/a (uncalibrated)* when the fuel gauge claims more than the design capacity, instead of a misleading value above 100 %.

These SMC keys are undocumented. Behaviour was verified on the machines below; use at your own risk.

### Tested on

| Machine | OS | What was verified |
|---|---|---|
| MacBookPro12,1 (13" 2015) | Ubuntu 24.04, kernel 6.8 | everything: `BCLM` limit, `ACEN` auto-discharge, service start/stop/restore, sleep hook, `.deb` install/remove, settings screen (brightness, keyboard backlight, blanking, ALPM, turbo via TLP) |
| Mac16,5 (M4 Max) | macOS 26.5 | status, SMC reads, settings screen (read-only), AlDente detection; `CHTE` / `CHIE` writes verified with the 0.1 prototype |

Intel Macs running macOS: implemented but not tested — reports welcome.

---

## 한국어 요약

Apple 노트북용 **배터리 충전 제한 · 배터리 상태 · 전원 설정** 도구입니다. 의존성 없는 네이티브 실행 파일 하나이고, 레트로 그린 스크린 UI를 씁니다.

```sh
curl -fsSL https://raw.githubusercontent.com/neuralfoundry-coder/macbat/main/install.sh | sudo sh   # 설치 + 서비스 시작
macbat            # 설정 화면 (↑↓ 선택, ←→ 변경, Enter 실행, Q 종료)
macbat status     # 상태 화면
```

- **충전 제한**: 인텔 맥 리눅스는 SMC 하드웨어 제한(`BCLM`), Apple Silicon은 충전 중지(`CHTE`)
- **자동 방전**: 제한보다 높으면 어댑터를 우회해서, 꽂아 둔 채로 제한치까지 방전
- **설정 화면**: 충전 제한, 입력 없을 때 화면 끄기, 밝기, 키보드 백라이트, 덮개 / 전원 버튼 / 유휴 잠자기, 디스크 절전(ALPM, 스핀다운), Wi-Fi / USB / CPU / PCIe 절전. 바로 적용되고 재부팅 후에도 유지됩니다(TLP가 있으면 TLP 설정으로 저장).
- **안전장치**: 서비스가 멈추거나 잠자기에 들어가면 정상 충전으로 복구합니다. 10% 이하에서는 충전을 막지 않습니다. AlDente가 실행 중이면 충전 설정에 손대지 않습니다.

비공개 SMC 키를 사용하므로 위 표의 기기에서 검증된 동작이며, 사용 책임은 사용자에게 있습니다.

## License

MIT
