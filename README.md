<div align="center">
  <img src="assets/c5vrx-phase8-logo.png" alt="C5VRX Phase8 logo" width="760" />

  <p><strong>ESP32-C5 Analog 5.8 GHz FPV Receiver</strong></p>
  <p>From live 5.8 GHz FPV video to real-time analog CVBS with one Seeed Studio XIAO ESP32-C5 and a passive resistor DAC.</p>

  <p>
    <a href="https://twotoz.github.io/C5VRX/"><img src="https://img.shields.io/badge/Web%20Flasher-Online-1f6feb?style=flat" alt="Web Flasher" /></a>
    <a href="https://discord.gg/3YNgJRHmzD"><img src="https://img.shields.io/badge/Discord-Join-5865F2?logo=discord&amp;logoColor=white" alt="Join the C5VRX Discord" /></a>
    <img src="https://img.shields.io/badge/status-production%20proven-success" alt="Production proven" />
    <img src="https://img.shields.io/badge/chip-ESP32--C5-111111" alt="ESP32-C5" />
    <img src="https://img.shields.io/badge/RF-5.8%20GHz%20(48%20channels)-6f42c1" alt="5.8 GHz" />
    <img src="https://img.shields.io/badge/output-analog%20CVBS%20NTSC-orange" alt="Analog CVBS" />
    <img src="https://img.shields.io/badge/architecture-Zero--EOF%20Circular%20GDMA-blueviolet" alt="Zero-EOF GDMA" />
    <img src="https://img.shields.io/badge/license-GPL--3.0--only-blue" alt="GPL-3.0-only" />
  </p>
</div>

---

## Current firmware

The `main` build defaults to the live adjacent Phase8 demodulator and the
ESP32-C5 **native hardware AGC**. Phase8 maps the full signed adjacent phase
delta (-128 through +127) to the 6-bit DAC. A transition across the +/-180
degree phase boundary can still alias. With native AGC the firmware never
disables Espressif's AGC and makes zero gain writes. Direct Gain V3 remains
available as the firmware gain fallback: press `N` on the console to reboot
into it, and `N` again to return to native AGC.

In an operator walk test (issue #119) native AGC gave a clean picture from
close range to the point where the picture degraded. At that same spot, Direct
Gain V3 sat at gain 62 with every Q4 sample in the four origin cells. See
[docs/phase8-range-envelope.md](docs/phase8-range-envelope.md). This is one
walk test, not a controlled attenuation sweep.

Known native-AGC issues, to be fixed next (#117 Phase 4): raw captures show
the vendor loop stepping gain within a video line (Phase8 noise lines, dark
colours shifting towards blue), and occasionally parking at a very low gain.
In that state Phase8 loses sync and the screen goes white until the gain
recovers or the receiver restarts. Close-range resolution is also lower than
with Direct Gain V3 because the native loop targets a smaller Q4 radius.

The combined build was flashed and observed with the VTX on. The operator
reported a clean picture; USB telemetry showed 98-99% coherence, no clipping,
and no RX/TX transport errors during that observation. This is a live hardware
check, not a characterization of every signal level or flight condition. The
menu and console still label V3 as a test profile because further range and
transition testing remains useful.

## Demodulator and gain notes

- [Direct Gain V3 design](docs/direct-gain-v3-core.md) describes the Q4
  observer, gain model, settle logic, and known hardware tuning limits.
- [Direct Gain V3 measurement oracle](docs/direct-gain-v3-oracle.md) documents
the guarded USB measurements and their interpretation.
- [Range and demod quality](docs/range-demod-quality-v2.md) records the
  measurement model and validation limits for CVBS sync and demod quality.
- [Range v2](docs/range-v2.md), [range notes](docs/range-v2-knowledge.md),
and [trajectory v2](docs/trajectory-v2.md) are research references; their
experimental ideas are not all part of the default live path.
- [Pre-Q4 lab](docs/pre-q4-lab.md) documents explicit RF/DAC noise checks and
vendor gain-table characterization. Those lab actions are not run as part of
normal video reception.

## Web Flasher (Zero-Install Browser Flashing)

Flash your Seeed Studio XIAO ESP32-C5 directly from your browser (Google Chrome, Microsoft Edge, Brave, Opera) with zero installation required:

**[Open the C5VRX Web Flasher](https://twotoz.github.io/C5VRX/)**

The production web flasher is hosted entirely by **GitHub Pages** at
[twotoz.github.io/C5VRX](https://twotoz.github.io/C5VRX/). There is no VPS,
application server, or separate production web host in the current deployment.

- **Automatic Latest Firmware**: Automatically selects the newest immutable semantic-version release.
- **One-Click Flashing**: Flashes the universal merged production image (`bootloader + partitions + app` at `0x0`) over Web Serial. Firmware is mirrored into the GitHub Pages artifact and fetched same-origin, so browser CORS redirects are not part of the production flash path.
- **Selectable Releases**: The **Releases** tab contains only semantic-version releases such as `v3.0.0`, `v3.0.1`, and newer.
- **Separate PR Builds Tab**: Same-repository pull requests publish a temporary `pr-<number>` GitHub prerelease. Experimental builds appear only under **PR Builds**, never in the normal Releases list, and require an explicit warning confirmation before flashing.
- **Offline / Local Execution**: You can also run the web flasher locally:
  ```bash
  python tools/open_webflasher.py
  ```
  *(Starts a local HTTP server at `http://localhost:8080/web/index.html` and opens your browser.)*

### Automatic releases and versioning

Every push or merged PR to `main` builds the production firmware and publishes a new immutable GitHub release with a semantic version tag.

- `BREAKING CHANGE` or a conventional-commit `!` creates a **major** bump.
- `feat:` / `feat(scope):` creates a **minor** bump.
- `fix:`, `chore:`, `docs:`, and other changes create a **patch** bump.
- If no stable release exists yet, the current `v3.0.0-rc1` line is promoted to `v3.0.0`.
- The resolved version is written to `version.txt` before the ESP-IDF build, so the firmware metadata and GitHub release use the same version.
- Release assets remain attached to their immutable version tag; the old mutable `main` release/tag is retired automatically after the first versioned release succeeds.

### Website deployment

Firmware releases and website deployment are intentionally separate:

- `.github/workflows/build.yml` validates and builds firmware, then publishes a versioned release after a successful push to `main`. For same-repository PRs it also maintains a clearly marked temporary prerelease (`pr-<number>`) and removes it when the PR closes.
- `.github/workflows/deploy-web.yml` is the **only production website deployment**. It always checks out trusted `main`, then packages the web flasher plus a generated firmware mirror into one GitHub Pages artifact.
- `tools/prepare_pages_site.sh` mirrors the newest semantic firmware releases and all active `pr-<number>` prereleases under `firmware/<tag>/`, and generates `firmware/releases.json`.
- The browser reads that Pages manifest and downloads binaries from the same `twotoz.github.io` origin. This avoids GitHub Release storage CORS redirects entirely.
- Successful Production CI triggers a Pages mirror refresh, so newly published/updated PR builds and releases become available without deploying unmerged PR web code.
- When a PR closes or merges, CI deletes its temporary prerelease/tag; the next successful cleanup-triggered Pages refresh removes it from the mirror.

---

## Community

Questions, build photos, feedback, or development discussion? **[Join the C5VRX Discord](https://discord.gg/3YNgJRHmzD)**.

---

## What is C5VRX?

**C5VRX-3** turns the **Seeed Studio XIAO ESP32-C5** (ESP32-C5 RISC-V SoC) into a standalone 5.8 GHz analog video (FPV) receiver.

It captures raw Wi-Fi PHY I/Q samples from the 5 GHz RF front-end at 40 MS/s, demodulates wideband FM in the ESP32-C5 **BitScrambler**, and outputs analog CVBS through **PARLIO TX** and a 6-bit passive resistor DAC ladder. The video-standard detector supports PAL and NTSC timing.

```text
5.8 GHz Analog FPV (48 Channels)
        │
        ▼
ESP32-C5 RF / MODEM_DIAG Bus (40 MS/s Q4/I4)
        │
        ▼
PARLIO RX @ 40 MS/s (POS sample edge, pure continuous hardware GDMA)
        │
        ▼
Circular GDMA Ring (32 KiB in HP SRAM, Zero-EOF patched)
        │
        ▼
Phase8 adjacent-demod BitScrambler (fm_phase8_hr_live.bsasm: full signed delta, 20 MS/s [D,D])
        │
        ▼
PARLIO TX @ 40 MHz ([D,D] mode -> 20 MS/s unique CVBS output)
        │
        ▼
6-bit Resistor DAC Ladder + 470 pF Filter -> 75-ohm Goggles
```

The continuous pixel path runs in AHB GDMA, BitScrambler, and PARLIO TX. A background CPU task observes completed IQ buffers and controls receiver settings; it does not move individual video pixels.

---

## Key Innovations & Architectural Highlights

### 1. The Breakthrough: Zero-EOF Circular GDMA
- **The Problem**: In continuous loop mode, the stock ESP-IDF PARLIO TX driver injected a GDMA EOF (`suc_eof = 1`) on every cyclic ring wrap (confirmed in [espressif/esp-idf#19091](https://github.com/espressif/esp-idf/issues/19091)). This triggered periodic hardware stalls, causing a 1-second vertical sync drop and jagged horizontal line jitter ("kartels").
- **The Solution**: C5VRX-3 patches `dw0.suc_eof = 0` across the descriptor ring in SRAM after driver initialization, paired with 64-byte aligned cache synchronization (`sync_dma_c2m`).
- **The Result**: Truly gapless, infinite circular streaming with zero wrap bubbles, rock-solid vertical sync lock, and crystal-clear horizontal alignment.

### 2. Gain control: native hardware AGC (default), Direct Gain V3 (fallback)
- By default `rf_start()` never calls `phy_disable_agc()` / `phy_rfagc_disable()`, releases forced gain and FFT scale once, and refuses every firmware gain write. Espressif's AGC owns RF/BB/fine gain.
- The on-screen menu offers only native AGC (RF FRONTEND page: gain shown as `NATIVE HW AGC`; long press changes bandwidth). The firmware fallback is reachable only from the serial console: `N` stores the choice in NVS and reboots. The fallback below applies only after selecting firmware gain control.
- The Direct Gain V3 fast observer measures centered Q4 P50/P90/P95, phase coherence, clipping, and origin occupancy from completed RX buffers.
- V3 is the sole automatic gain writer in the default profile. It chooses physical RF/BB/Fine gain tuples and waits for settled observations after writes.
- Healthy measurements produce a zero-write hold. A live check with the VTX on showed a clean picture and no clipping or transport errors; broader range and transition testing remains useful.

### 3. Default RF settings
- The default Direct Gain V3 profile uses BW40 and AFC off. This keeps gain as the changing RF control during normal operation.
- Other receiver profiles and lab modes remain selectable over the serial console. Some experimental profiles can use automatic bandwidth or AFC while acquiring a carrier.

### 4. Phase8 adjacent demodulator
- `fm_phase8_hr_live.bsasm` uses adjacent I/Q samples at 20 MS/s and maps the full signed phase-delta range (-128 through +127 bins) across the 64 DAC levels.
- The mapping preserves direction across the signed range. A phase step that crosses the +/-180 degree representation boundary remains ambiguous and can alias.
- Phase8 is selected by default in the current build and was viewed live with Direct Gain V3.

### Demodulator A/B (PR #122)

The VIDEO OUTPUT menu cycles **Golden**, **P8 VIDEO32**, and **P8 FULL** with
long press; the selection is saved and takes effect on menu exit. Serial
`u` toggles Golden / P8 VIDEO32 with a reboot; `d` returns to the original
P8 FULL. Native hardware AGC owns gain in every mode. Fresh installations
keep P8 FULL; existing Golden/Phase8 choices migrate.

P8 VIDEO32 is an experimental bounded video-range mapping: pedestal 20,
saturated gain and tapered extreme tails. It uses 32 Phase8-derived endpoint
states to keep two bundles per pair, so it **does not retain full 8-bit phase
state**. It improves the digital video swing, but simulated fine grain is not
lower than P8 FULL at identical IQ. Test loaded CVBS levels and picture/range
before selecting it for flight. No unverified AGC-target register is enabled.

### Known limits
- A signed adjacent-phase estimate cannot distinguish an actual step beyond 180 degrees from its wrapped equivalent.
- The hardware observation documented above covers a working live picture at the tested VTX and receiver setup. It does not establish performance at all distances, channels, antenna orientations, or gain transitions.

---

## Hardware Pinout (Seeed Studio XIAO ESP32-C5)

Connect a 6-bit binary-weighted resistor DAC ladder to the XIAO pins, meeting at the `VIDEO` node:

| XIAO Pin | ESP32-C5 GPIO | Bit Weight | Series Resistor |
|:---:|:---:|:---:|:---:|
| **D4** | GPIO 23 | Bit 0 (LSB) | 8.2 kΩ |
| **D5** | GPIO 24 | Bit 1 | 3.9 kΩ |
| **D6** | GPIO 11 | Bit 2 | 2.0 kΩ |
| **D7** | GPIO 12 | Bit 3 | 1.0 kΩ |
| **D8** | GPIO 8  | Bit 4 | 470 Ω |
| **D9** | GPIO 9  | Bit 5 (MSB) | 240 Ω |
| **GND** | GND | Ground | Ground reference |

### Output network and controls
1. **Video level**: The reference circuit uses a 200 ohm shunt at `VIDEO`; the connected display or goggles may add their own 75 ohm termination. Check the resulting level with the load you use.
2. **Output filter**: The reference circuit uses a 470 pF ceramic capacitor from `VIDEO` to `GND`. Check image sharpness with your display and termination.
3. **BOOT button**: A short click switches channel. A long press opens the menu when enabled; short clicks move through menu choices and a long press applies one. On the CHANNEL page, a long press scans the available channels and selects the strongest coherent carrier. If Safe Flight mode blocks the menu, hold BOOT for three seconds to restore the default Phase8/6BIT@40/Direct Gain profile and open the recovery menu.
4. **Persistent settings**: Channel, RF bandwidth, AFC, video-standard/output, AGC/gain, and the BOOT-menu preference are saved and restored after restart.

---

## Interactive Serial Console Hotkeys

Connecting to the USB serial console (115200 baud) provides live telemetry and single-key controls:

| Key | Action |
|:---:|:---|
| `c` / `C` | Cycle FPV channel / band (48 standard channels: RaceBand, Boscam A/B/E, FatShark, LowBand) |
| `+` / `-` | Manual RF gain step (±2 index) |
| `a` / `s` / `m` | Switch AGC mode: **Active** (auto-adapting) / **Shadow** (dry-run) / **Manual** (fixed) |
| `f` | Cycle AFC Mode: **Auto Centering** (±1.5 MHz) / **Hold** / **Off** (0 kHz) |
| `,` / `.` | Fine-tune carrier frequency offset in ±50 kHz steps |
| `0` | Reset frequency offset to 0 kHz |
| `e` | Toggle RX sample clock edge (POS / NEG) |
| `d` | Print the live diagnostics summary and available console commands |
| `R` | Run the guarded RSSI and centered-Q4 measurement probe |
| `D` / `I` / `Y` | Select Direct Gain V3 / Direct Gain V1 / ARC V3 profiles (firmware gain mode only) |
| `N` | Reboot switching between native hardware AGC (default) and firmware gain control |
| `E` | Print one `P8ENV` Q4 envelope / origin-collapse row |
| `Q` / `T` | Raw Q4/I4 dump (4 x 64 consecutive samples) / read-only AGC register dump |

---

## Build & Flash Guide

### Prerequisites
- **Option A (Docker - Recommended)**: Docker Desktop or Docker engine installed.
- **Option B (Native ESP-IDF)**: [ESP-IDF v6.0.x](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32c5/get-started/) installed with Python 3.10+.

---

### Step 1: Build the Firmware

#### Option A: Build via Docker (Zero-Install Toolchain)
No ESP-IDF installation required on your host machine. Run from the repository root:

**Linux / macOS / Git Bash:**
```bash
docker run --rm -v "${PWD}:/workspace" -w /workspace espressif/idf:v6.0.2 idf.py build
```

**Windows PowerShell:**
```powershell
docker run --rm -v "${PWD}:/workspace" -w /workspace espressif/idf:v6.0.2 idf.py build
```

#### Option B: Build with Native ESP-IDF v6.0+
If you have ESP-IDF installed locally:

**Linux / macOS:**
```bash
. $IDF_PATH/export.sh
idf.py build
```

**Windows (ESP-IDF PowerShell Environment):**
```powershell
export.ps1
idf.py build
```

The build produces three critical binaries in `build/`:
- `build/bootloader/bootloader.bin` (at flash offset `0x2000`)
- `build/partition_table/partition-table.bin` (at flash offset `0x8000`)
- `build/c5vrx3.bin` (at flash offset `0x10000`)

---

### Step 2: Validate the firmware sources
CI runs the architectural validator and host demodulation checks. To run the repository validator locally:
```bash
python tools/validate_build.py
```
*(All architectural checks must pass.)*

---

### Step 3: Flash to ESP32-C5

#### Option A: Web Flasher (Zero-Install In-Browser Flasher)
Launch the web flasher directly in your browser (Google Chrome, Microsoft Edge, Brave):
**[Open C5VRX Web Flasher on GitHub Pages](https://twotoz.github.io/C5VRX/)**

#### Option B: Zero-Friction Auto-Flash (Python Watcher)
Run the auto-flash watcher:
```bash
python tools/auto_flash.py
```
*Plug in or reset your Seeed Studio XIAO ESP32-C5 into download mode (hold BOOT while tapping RESET), and the watcher will detect the COM port, flash the firmware, and automatically trigger a watchdog reset into the application!*

#### Option C: Direct Flash Script
Specify your COM port (or omit to auto-detect):
```bash
python tools/flash.py COM10
```

#### Option D: Native ESP-IDF Flasher
```bash
idf.py -p COM10 flash
```

---

### Step 4: Interactive Serial Monitor & Diagnostics
Launch the dedicated low-latency serial monitor:
```bash
python tools/monitor.py COM10
```
Use the interactive hotkeys (`c` to cycle channels, `+`/`-` for manual gain, `a` for active AGC, `d` for hardware diagnostics).

---

## Repository Structure

```text
C5VRX/
+-- main/
|   +-- video.c, video.h             # Video pipeline, gain controller, menu, console
|   +-- fm_phase8_hr_live.bsasm      # Default live Phase8 demodulator
|   +-- direct_gain_v3.c, .h         # Default automatic gain controller
|   +-- phase8_gain_lut.h            # Generated Phase8 coherence/gain lookup
|   +-- rf.c, rf.h                   # ESP32-C5 RF and PHY controls
+-- assets/                          # Project branding
+-- docs/                            # Design notes and hardware measurements
+-- tools/                           # Build checks, generators, and flash utilities
+-- web/                             # Browser Web Serial flasher
+-- sdkconfig.defaults               # ESP-IDF defaults
+-- partitions.csv                   # Flash partition table
```

---

## License

C5VRX is open-source software licensed under the **GNU General Public License v3.0 only** (`GPL-3.0-only`).

See [LICENSE](LICENSE) for full licensing terms. The C5VRX name and logos have separate terms; see [assets/BRANDING.md](assets/BRANDING.md).
