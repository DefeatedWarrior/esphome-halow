# CLAUDE.md: esphome-halow

ESPHome external component for Wi-Fi HaLow (802.11ah) on ESP32-S3 + Morse Micro MM6108 (Seeed Wio-WM6180 module for XIAO). `halow:` replaces `wifi:`. This repo is a fork of `sweitzja/esphome-halow` that targets **ESPHome 2026.x with the native ESP-IDF toolchain** (what the Home Assistant ESPHome Builder uses).

## Read first

| Need | File |
|------|------|
| How the code works (tasks, state machine, reconnect invariant, mDNS, sensors) | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| What ESPHome internals we depend on; what breaks on ESPHome upgrades | [docs/ESPHOME_COMPAT.md](docs/ESPHOME_COMPAT.md) |
| Build, verify, flash, read logs, branches | [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) |
| Pins, power, firmware/BCF, failure signatures, reset reasons, open hardware issue | [docs/HARDWARE.md](docs/HARDWARE.md) |
| Every change made by the fork, with reasoning and verification status | [PATCHES.md](PATCHES.md) |
| Original author's notebook (why things were done; partly historical) | [docs/HALOW_NOTES.md](docs/HALOW_NOTES.md) |

## Layout

```
components/halow/   __init__.py (schema, codegen, SDK download, network patch)
                    halow_component.{h,cpp} (runtime), pre_build.py (PlatformIO only)
docs/               reference docs (above)
tools/              serial_watch.py, test_network_patch.py, and the original
                    bring-up apps: spi_probe/, sta_connect/, iperf/
example.yaml        every option and sensor (uses the local component path)
PATCHES.md          change log with rationale
```

## Rules

- **After `setup()`, only the reconnect timer task calls `mmwlan_sta_disable/enable`.** `loop()` schedules it via `schedule_reconnect()`. Calling the SDK from the main loop wedges it.
- **Never `mdns_free()`**: it destroys ESPHome's mDNS services.
- **The ESPHome network patch must fail loudly.** If `_patch_network_util()` can't find its anchors, raise; never skip silently. Without it the API drops every client.
- **Keep the MM-IoT-SDK pinned** (`MM_IOT_SDK_COMMIT`). Firmware and BCF must come from the same SDK version.
- SDK and lwIP callbacks must not block or call `mmwlan_*`.
- Don't put personal network details (SSIDs, passwords, IPs) in this repo. User-specific notes live outside it (see the workspace CLAUDE.md one level up, if present).
- Record every behavioural change in PATCHES.md: problem, fix, how verified, what wasn't verified.

## Before pushing to `main`

Follow the checklist in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#verification-checklist). At minimum: pyflakes, `tools/test_network_patch.py`, a full-feature compile on the pinned ESPHome version, and a check that the patched `util.h` is in the build. `main` is what users' Home Assistant builds pull; test larger changes on a branch first (`github://DefeatedWarrior/esphome-halow@branch`). `pre-improvements` is the fallback branch.

## Quick facts

- Tested: ESPHome 2026.9.1, ESP-IDF 5.5.5, MM-IoT-SDK `96d9bf4` (morselib 2.10.4, firmware 1.17.6), BCF `bcf_mf16858.mbin`.
- Pins: SCK 7, MISO 8, MOSI 9, CS 4, IRQ 3, RESET 1, WAKE 2, BUSY 5.
- `country_code` is required: AU, CA, EU, GB, IN, JP, KR, NZ, US.
- Power save is disabled (BUSY pin unreliable in this firmware). Sub-bands off by default (8 MHz).
- The RF front end (TX amplifier) runs from the 5 V/USB rail and powers up at `mmwlan_sta_enable()`.
