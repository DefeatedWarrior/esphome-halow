# Development

How to build, verify, flash and release changes to this component.

## Environment

- **ESPHome pinned to the tested version** in a virtualenv:
  ```bash
  python3 -m venv .venv && .venv/bin/pip install "esphome==2026.9.1" pyflakes
  ```
  The first compile downloads ESP-IDF to `~/Library/Caches/esphome/idf` (macOS) or `~/.cache/esphome/idf`; later builds reuse it.
- **MM-IoT-SDK** is downloaded by the component on first compile to `~/.esphome/mm-iot-esp32`, pinned to `MM_IOT_SDK_COMMIT`.
- **Git remotes:** `origin` is the fork (`DefeatedWarrior/esphome-halow`), `upstream` is `sweitzja/esphome-halow`.

## Test config

Point a YAML at the working tree, not GitHub, so local edits are compiled:

```yaml
esphome:
  name: halowtest
esp32:
  variant: esp32s3
  framework:
    type: esp-idf
external_components:
  - source:
      type: local
      path: /absolute/path/to/esphome-halow/components
    components: [halow]
logger:
api:
ota:
  - platform: esphome
debug:                       # logs the reset reason at boot
text_sensor:
  - platform: debug
    reset_reason: {name: Reset Reason}
binary_sensor:
  - platform: status         # ON only if network::is_connected() sees HaLow
    name: Status
halow:
  ssid: !secret halow_ssid
  password: !secret halow_password
  country_code: AU
  # add every sensor (see example.yaml) to compile all code paths
```

Build output goes to `.esphome/build/<name>/` next to the YAML.

## Verification checklist

Run before pushing to `main`:

1. **Lint:** `.venv/bin/python -m pyflakes components/halow/__init__.py tools/*.py` (`pre_build.py` reports `Import`/`env` as undefined; those are PlatformIO globals).
2. **Network patch across ESPHome versions:** `.venv/bin/python tools/test_network_patch.py` (add new versions as arguments).
3. **Full compile** with every sensor, the scan button, `max_tx_power`, and a `manual_ip` with `dns1`, so every code path builds: `.venv/bin/esphome compile test.yaml`. No warnings from `components/halow/`.
4. **Patch is in the build:**
   ```bash
   grep -c USE_HALOW .esphome/build/<name>/src/esphome/components/network/util.h   # expect 2
   NM=$(ls ~/Library/Caches/esphome/idf/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-nm)
   $NM -C $(find .esphome/build/<name>/build -name api_server.cpp.obj) | grep halow   # expect U esphome::halow::network_is_connected()
   ```
5. **Config validation:** each of these must fail with a clear message: `country_code: XX`, no `country_code`, `type: MM8108`, `mode: AP`, a 33-character `ssid`, two pins on the same GPIO, `max_tx_power: 40`, `security: SAE` without a password. `country_code: au` must pass (normalised to `AU`). `esphome config` must show `use_address` defaulting to `<name>.local` (or the `manual_ip` address).
6. **Device address:** with `use_address: <ip>` set, run `esphome logs test.yaml --device OTA` twice, and again after a `compile`. Every run must print `Starting log output from <ip>` (stop it with Ctrl-C), never "Loaded validated config cache" or "could not be resolved". The second run is what catches the cache problem.
7. **Clean build from GitHub**, as Home Assistant does, after pushing a branch: `source: github://DefeatedWarrior/esphome-halow@<branch>` in a fresh folder.
8. **Hardware** (when possible): flash, watch the log (below) for `HaLow connected to`, `Status: ON`, and Home Assistant staying connected past 15 minutes.

## Flashing and logs

```bash
.venv/bin/esphome upload test.yaml --device /dev/cu.usbmodem101
.venv/bin/python tools/serial_watch.py 90 watch.log     # survives resets, unlike `esphome logs`
sed 's/\x1b\[[0-9;]*m//g' watch.log | grep -E "===|halow|Guru|abort|reset"
```

- `=== lost port ===` lines mark full chip resets. A panic or `esp_restart()` does **not** drop the port, but a power-on reset does.
- Reset reasons are in [HARDWARE.md](HARDWARE.md#failure-signatures). Boot output before the port reopens is lost; for a reset loop, log `esp_reset_reason()` periodically from a temporary diagnostic build instead.
- ESPHome's crash handler prints the previous boot's backtrace (`*** CRASH DETECTED ON PREVIOUS BOOT ***`) and decodes it when logging through `esphome logs`. To decode by hand:
  ```bash
  A=$(ls ~/Library/Caches/esphome/idf/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-addr2line)
  $A -pfiaC -e .esphome/build/<name>/build/<name>.elf 0x4200e50f 0x42076c4d
  ```
- **Diagnostic build pattern** (used to isolate the power-on reset): in a throwaway copy of the component, log `esp_reset_reason()` at the top of `setup()` and in a 1 s `set_interval`, and wrap the final `start_connect_()` in `set_timeout(15000, ...)`. That separates "fails during setup" from "fails when the radio powers up".

## Branches and releases

- `main`: what Home Assistant builds by default (`github://DefeatedWarrior/esphome-halow`). Keep it working.
- `pre-improvements`: `main` before the Part 2/3 review fixes (four original ESPHome 2026.x fixes only). Kept as a fallback.
- Do larger changes on a branch; test with `@branch` and `refresh: 1min` in Home Assistant; then fast-forward `main`.
- Record every behavioural change in [PATCHES.md](../PATCHES.md): problem, fix, how it was verified, and what wasn't.
- To pull in upstream changes: `git fetch upstream && git merge upstream/main`, then re-run the checklist; the upstream author still targets the PlatformIO toolchain.

## Standalone test apps (`tools/`)

The original author's bring-up tools, built outside ESPHome:

| Tool | Build | What it does |
|------|-------|--------------|
| `tools/spi_probe` | PlatformIO, Arduino | Raw SPI/SDIO register probe of the MM6108 (no firmware load). Checks wiring. |
| `tools/sta_connect` | ESP-IDF + `MMIOT_ROOT` | Boots the MM6108 and associates with WPA3-SAE. Set SSID/passphrase in `main/src/sta_connect.c`. |
| `tools/iperf` | ESP-IDF + `MMIOT_ROOT` | Full lwIP + DHCP + iperf server, for throughput testing. |

For the ESP-IDF ones: `export MMIOT_ROOT=~/.esphome/mm-iot-esp32`, then `idf.py set-target esp32s3 build flash monitor` with ESP-IDF 5.5 sourced.

## Test history

Original author, ESPHome before 2026.x (PlatformIO toolchain, ESP-IDF 5.5.2), XIAO ESP32-S3 + Wio-WM6180 + HaLowLink 2, US:

| Test | Result |
|------|--------|
| SPI/SDIO registers, firmware boot | Pass: FW 1.17.6, morselib 2.10.4, chip `0x306` |
| WPA3-SAE association, DHCP | Pass: link up in ~10 s |
| ESPHome API from LAN, mDNS, HA (2026.4.2) | Pass |
| LAN ping (power save off) | 10/10, 7-11 ms |
| 20-minute soak | Heap stable |
| Range walk / AP power cycle | Recovered in 16 s / 53 s |
| OTA over HaLow | 1.05 MB in ~10 s, 3 cycles clean |

This fork, ESPHome 2026.9.1 (native ESP-IDF 5.5.5), XIAO ESP32-S3 + Wio-WM6180 + HaLowLink 2, AU:

| Test | Result |
|------|--------|
| Fixes #1-#4: compile, boot, WPA3-SAE, DHCP, mDNS | Pass (2026-10-12), before the hardware began resetting |
| Fixes #5-#21: compile, validation, network patch on 4 ESPHome releases, SDK download | Pass |
| `main` on hardware (2026-10-13, after a 5 V capacitor fix): boot, connect, DHCP, mDNS via ESPHome's own records, connect-timeout → timer reconnect cycle (5 cycles, no crash or overlap) | Pass |
| Home Assistant API over HaLow (fix #5 at runtime), scan, OTA over HaLow, recovery after link loss, static IP DNS, long soak | **Not yet tested** (HA's network can't reach the HaLow network yet) |
