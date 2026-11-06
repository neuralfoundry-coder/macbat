# macbat

Battery charge limiter and battery status CLI for Apple laptops — one Python file, no dependencies.

- **Linux on Intel MacBooks** (e.g. a MacBook Pro 2015 turned into a home server): hardware charge limit through the SMC, plus *auto-discharge* — when the battery is above the limit, macbat bypasses the AC adapter so the battery drains down to the limit, even while plugged in.
- **macOS on Apple Silicon**: the same limit / auto-discharge behaviour, implemented by pausing charging and bypassing the adapter through the SMC.
- **`macbat` (no arguments)** prints a status page: charge, mode, adapter, limit, health, cycles, voltage/current/power, temperature, time remaining.

```
$ macbat
macbat 0.1.0  ·  MacBookPro12,1  ·  Linux · Intel Mac (applesmc)
  Battery   97%  [███████████████████│████]  discharging to 80% (adapter bypassed)
  Power     AC adapter bypassed by macbat (running on battery)
  Limit     80%  ·  hardware (BCLM)  ·  auto-discharge: on (ACEN)
  Health    103.1% (7008/6800 mAh) · 18 cycles
  Electric  12.46 V · -1.05 A · -13.1 W · 28.0 °C
  Time      to empty 6h23m  ·  to full -
  Service   active
```

```
$ macbat
macbat 0.1.0  ·  Mac16,5  ·  macOS · Apple Silicon
  Battery   80%  [███████████████████│░░░░]  holding at limit (charging paused)
  Power     AC connected · 140 W adapter
  Limit     80%  ·  software (CHTE)  ·  auto-discharge: on (CHIE)
  Health    96.2% (8250/8579 mAh) · 97 cycles
  Electric  12.33 V · +0.00 A · +0.0 W · 30.4 °C
  Service   active
```

## Install

Requires Python 3.8+ (Ubuntu's `python3`, or Homebrew / Xcode CLT Python on macOS).

```sh
git clone https://github.com/neuralfoundry-coder/macbat.git
cd macbat
sudo python3 ./macbat install      # copies to /usr/local/bin/macbat and starts the service
macbat                             # status
```

`install` writes `/etc/macbat.conf` (limit 80%, auto-discharge on) if it does not exist, and starts
a background service: `macbat.service` (systemd) on Linux, `com.neuralfoundry.macbat` (launchd) on macOS.
Running `install` again upgrades the binary and restarts the service.

## Usage

| Command | |
|---|---|
| `macbat` / `macbat status [--json]` | battery + limit status |
| `sudo macbat limit 80` | set the charge limit (20–100); `limit off` = 100 |
| `sudo macbat discharge on\|off` | auto-discharge down to the limit while plugged in |
| `sudo macbat restore` | immediately resume charging and re-enable the adapter |
| `sudo macbat install` / `uninstall` | add / remove the service (uninstall restores 100% and normal charging) |
| `macbat keys` | show the relevant SMC keys (diagnostics; needs sudo on Linux) |

Configuration lives in `/etc/macbat.conf` and is re-read by the service within ~2 s:

```ini
limit=80
auto_discharge=on
hysteresis=3     # software limit: resume charging at limit-3 %
interval=30      # seconds between checks
```

## How it works

macbat talks to the System Management Controller directly and only uses keys it finds on the machine.

| Platform | Charge limit | Auto-discharge (bypass adapter) | Battery data |
|---|---|---|---|
| Linux, Intel Mac | `BCLM` (hardware limit, enforced by the SMC itself) | `ACEN`=0, or `CH0I`=1 | `/sys/class/power_supply`, SMC |
| macOS, Apple Silicon | `CHTE` (or `CH0B`/`CH0C`) toggled by the service, with hysteresis | `CHIE`=0x08, or `CH0I` | `ioreg AppleSmartBattery`, SMC |
| macOS, Intel | `BCLM` | `CH0I` | `ioreg`, SMC |

- **Linux**: reads go through the `applesmc` driver. Writes use the SMC I/O ports (0x300/0x304, via `/dev/port`) with the same protocol as the kernel driver; while writing, macbat briefly stops `mbpfan` and unloads `applesmc` so nothing else talks to the SMC, then reloads it, restarts `mbpfan`, and verifies the value through the driver. Writes only happen when something changes (crossing the limit, config changes). Requires kernel lockdown to be off (the default on Macs, which do not use Secure Boot with Linux).
- **macOS**: uses the `AppleSMC` IOKit user client through `ctypes` (no compiled helper). Reading works as any user; writing needs root.

### Safety

- When the service stops (or crashes — `ExecStopPost` on Linux), charging and the adapter are restored to normal.
- Before the machine sleeps, charging and the adapter are restored (systemd sleep hook on Linux, IOKit sleep notification on macOS), so a plugged-in laptop never drains while asleep.
- Below 10 % macbat never pauses charging or bypasses the adapter.
- `sudo macbat uninstall` restores 100 % and normal charging.

### Notes

- These SMC keys are undocumented. Behaviour was verified on the machines below; use at your own risk.
- macOS 26 has its own charge-limit setting (System Settings → Battery). Use one or the other.
- On Apple Silicon, while the adapter is bypassed, macOS reports the charger as disconnected — that is expected.

### Tested on

| Machine | OS | Result |
|---|---|---|
| MacBookPro12,1 (13" 2015, i5-5257U) | Ubuntu 24.04, kernel 6.8 | `BCLM` limit, `ACEN` auto-discharge, service, sleep hook |
| Mac16,5 (M4 Max) | macOS 26.5 | status, `CHTE` limit, `CHIE` discharge, daemon loop + restore on stop (launchd install path not yet exercised) |

Intel Macs running macOS: implemented (`BCLM`/`CH0I`) but not tested yet — reports welcome.

---

## 한국어 요약

Apple 노트북용 **배터리 충전 제한 + 배터리 상태 확인** 명령어입니다. 파이썬 파일 하나로 동작합니다.

- **리눅스를 설치한 인텔 맥북** (예: 홈서버로 쓰는 맥북프로 2015): SMC의 `BCLM`으로 하드웨어 충전 제한, 제한보다 높으면 어댑터를 우회(`ACEN`)해서 꽂혀 있어도 제한치까지 **자동 방전**합니다.
- **Apple Silicon macOS**: `CHTE`(충전 중지)와 `CHIE`(어댑터 우회)로 동일하게 동작합니다.
- `macbat` 만 입력하면 배터리 %, 상태, 어댑터, 제한값, 배터리 건강도, 사이클, 전압·전류·전력, 온도, 남은 시간을 보여 줍니다.

```sh
sudo python3 ./macbat install   # 설치 + 서비스 시작 (기본 80%, 자동 방전 켜짐)
macbat                          # 상태 보기
sudo macbat limit 80            # 충전 제한 변경 (20~100, off)
sudo macbat discharge on|off    # 제한 초과 시 자동 방전
sudo macbat restore             # 즉시 정상 충전으로 복구
sudo macbat uninstall           # 서비스 제거 + 100% 정상 충전으로 복구
```

서비스가 멈추거나 잠자기에 들어갈 때는 항상 정상 충전 상태로 되돌리고, 10% 이하에서는 충전을 막지 않습니다.
비공개 SMC 키를 사용하므로 위 표의 기기에서 검증된 동작이며, 사용 책임은 사용자에게 있습니다.

## License

MIT
