# Architecture

How the `halow` component works today. This is the current reference; [HALOW_NOTES.md](HALOW_NOTES.md) is the original author's engineering notebook and is partly historical.

## Files

| File | Role |
|------|------|
| `components/halow/__init__.py` | YAML schema and validation, code generation, MM-IoT-SDK download, sdkconfig options, build flags, the ESPHome network patch |
| `components/halow/halow_component.h` | `HalowComponent` class, sensor setters, `HalowScanButton`, network hook declarations |
| `components/halow/halow_component.cpp` | Everything at runtime: SDK bring-up, state machine, reconnect timer, sensors, mDNS, channel scan, linker wraps |
| `components/halow/pre_build.py` | PlatformIO-toolchain-only script that links the firmware blobs. Unused with the default native ESP-IDF toolchain |

## Build time (`__init__.py`)

`CONFIG_SCHEMA` validates the YAML. Beyond normal types it enforces: `country_code` required and in the SDK's regulatory database; `ssid` 1-32 and `password` ≤100 characters; a password for `SAE`; `type: MM6108` and `mode: STATION` only; pins distinct and 0-48; `max_tx_power` 1-30.

`to_code()` then:

1. Passes config to the C++ object (`set_ssid`, `set_country_code`, pins, sensors, ...) and defines `USE_HALOW`.
2. `_ensure_sdk()` downloads the MM-IoT-SDK pinned to `MM_IOT_SDK_COMMIT` (`git init` + `fetch --depth 1 <sha>`), or uses the existing checkout. Warns if the default checkout is at another commit.
3. Registers SDK folders as ESP-IDF components: `morselib`, `mm_shims`, `mmipal`, `mmutils`, `mmpktmem`, `mmregdb`.
4. Sets sdkconfig options:

   | Option | Why |
   |--------|-----|
   | `CONFIG_MM_*` pins, `CONFIG_MM_BCF_FILE`, `CONFIG_MM_FW_FILE`, `CONFIG_MMHAL_CHIP_TYPE_MM6108` | Read by the SDK's `mm_shims` |
   | `CONFIG_FREERTOS_HZ=1000`, `CONFIG_FREERTOS_TIMER_TASK_PRIORITY=10` | Required by the SDK |
   | `CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=4096` | Reconnects run SDK calls and logging on the timer task |
   | `CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB` (S3 only) | SDK recommendation |
   | `CONFIG_MBEDTLS_NIST_KW_C`, `CONFIG_MBEDTLS_CMAC_C` | Morse supplicant crypto; ESPHome 2026.x disables CMAC |
   | `CONFIG_LWIP_NETIF_STATUS_CALLBACK`, `CONFIG_LWIP_STATS` | mmipal link callbacks; packet counters for `tx_pps`/`rx_pps` |
   | `CONFIG_MDNS_PREDEF_NETIF_{STA,AP,ETH}=n` | No Wi-Fi/Ethernet esp_netif exists for mDNS to attach to |

5. Adds build flags: `-DLWIP_NETIF_LINK_CALLBACK=1`, `-Wl,--wrap=tcpip_input` (RX byte counting), `-Wl,--wrap=tcpip_init` (see below).
6. `_patch_network_util()` writes patched copies of ESPHome's `network/util.h` and `util.cpp` into the build tree. See [ESPHOME_COMPAT.md](ESPHOME_COMPAT.md).
7. Registers `pre_build.py` for the PlatformIO toolchain only.

At import, `_install_address_hook()` also makes ESPHome's `CORE.address` fall back to `halow.use_address`, so the CLI and Device Builder can find the device for wireless logs and OTA (see [ESPHOME_COMPAT.md](ESPHOME_COMPAT.md)). This is host-side only and doesn't change the firmware.

## Runtime

### Tasks and shared state

Four execution contexts touch the component. Keep this in mind for any change:

| Context | Runs |
|---------|------|
| ESPHome main loop | `setup()`, `loop()`, sensor updates, `request_channel_scan()`, `publish_scan_results_()`, `start_mdns_()` |
| lwIP tcpip thread | `mmipal_link_status_cb`, `__wrap_tcpip_input`, `halow_linkoutput_hook` |
| Morse SDK tasks | `link_state_cb`, `sta_status_cb`, `halow_scan_rx_cb`, `halow_scan_complete_cb` |
| FreeRTOS timer task | `do_halow_reconnect` |

They communicate through file-static `volatile` flags (`s_link_up`, `s_link_down_event`, `s_sta_connected`, `s_reconnecting`, `s_scan_complete`, `s_scan_running`), `s_last_attempt_ms`, the byte counters, and `s_scan_entries` guarded by `s_scan_lock`. The SDK's own callbacks must not block or call `mmwlan_*` APIs.

### `setup()`

1. Register `halow_shutdown_handler` (runs before `esp_restart()`, e.g. after OTA: STA disable, shutdown, deinit, SPI reset, hold MM6108 in reset).
2. Reset the SPI2 peripheral and free any stale bus.
3. Disarm GPIO interrupts on the IRQ and BUSY pins. A software reset doesn't clear the GPIO matrix; a LOW_LEVEL IRQ left armed storms as soon as `mmhal_init()` installs the ISR service.
4. Pulse RESET_N.
5. `mmhal_init()`, `mmwlan_init()`, optional `mmwlan_override_max_tx_power()`, `mmwlan_set_channel_list()` for the country, link-state callback, `mmwlan_boot()` (loads `mm6108.mbin` and the BCF over SPI, about 0.7 s).
6. Power save off (the BUSY pin is unreliable in this firmware), sub-bands as configured.
7. `mmipal_init()` (DHCP or static; static also sets DNS via `mmipal_set_dns_server()`), link-status callback, reconnect timer (one-shot, 15 s), scan mutex.
8. `start_connect_()` → `mmwlan_sta_enable()`. **This powers up the RF section**; see [HARDWARE.md](HARDWARE.md#power).

### State machine and reconnects

```
STOPPED ──setup() ok──► CONNECTING ──STA connected + IP──► CONNECTED
                           │   ▲                               │
   60 s since last attempt │   │ link up + IP                  │ link down
   and no timer pending    ▼   │                               ▼
                   schedule_reconnect() ──► timer (15 s) ◄── CONNECTING
                                              │
                                   do_halow_reconnect():
                                   sta_disable() (failure tolerated)
                                   sta_enable()
```

**Invariant: after the first `sta_enable()` in `setup()`, only the timer task calls `mmwlan_sta_disable/enable`.** `loop()` only schedules the timer. The original author found that calling these from the main loop wedges the SDK, and running them from two tasks at once races. `s_last_attempt_ms` is set by every `sta_enable()` and on link loss; `loop()` uses it for the 60 s connect timeout. The link-down callback schedules the timer immediately; `loop()`'s timeout is the backstop.

### Network integration

ESPHome components (API, MQTT, OTA, `status`, `http_request`, ...) ask `network::is_connected()` whether the network is up. The build-time patch makes that, and `network::get_ip_addresses()`, call `halow::network_is_connected()` / `halow::network_get_ip_addresses()`, defined at the end of `halow_component.cpp`.

`__wrap_tcpip_init`: ESPHome's network component calls `esp_netif_init()`, which starts lwIP. `mmipal_init()` calls `tcpip_init()` again, which would abort. The wrapper lets the first call through and turns later calls into `tcpip_callback(initfunc, arg)` on the running thread. The wrapper lives in `halow_component.cpp` because `--wrap` targets in a file nobody else references get dropped by the linker.

### mDNS

mmipal creates a raw lwIP `netif`, but the mDNS library needs an `esp_netif_t`. On first connect `start_mdns_()` finds the netif by MAC, wraps it in a minimal `esp_netif_obj` (from the internal header `esp_netif_lwip_internal.h`, included relative to esp_netif's public include dir), registers it and announces. `mdns_init()` is a no-op if ESPHome's mdns component already started the responder, so ESPHome's services and TXT records are kept; `_esphomelib._tcp` is only added if missing. Later connects re-announce.

### Sensors

`update_sensors_()` runs every 10 s while connected.

- RSSI: `mmwlan_get_rssi()`.
- Packet rates: deltas of `mmipal_get_link_packet_counts()`.
- Throughput: deltas of the byte counters. TX is counted by wrapping `netif->linkoutput` once the link is up; RX by `__wrap_tcpip_input`, filtered to the netif named `MM`.
- MCS, bandwidth, TX success and link quality: deltas of `mmwlan_get_rc_stats()` per rate entry. The rate with the most new packets is the current one.
- Drops, CCMP failures, HW restarts: `mmwlan_get_umac_stats()`.
- IP, gateway, subnet and BSSID are republished every cycle; SSID, MAC and firmware version once.

### Channel scan

The `scan_channel` button calls `request_channel_scan()` (only while connected) → `mmwlan_scan_request()`. Results arrive on an SDK task and are copied into `s_scan_entries` under `s_scan_lock` (max 32). When the complete callback sets `s_scan_complete`, `loop()` calls `publish_scan_results_()`: sort by RSSI, pick the noise floor (our AP's channel, else the quietest), and publish compact JSON truncated to whole entries within Home Assistant's 255-character state limit.

## Things not to change casually

- Don't call `mmwlan_sta_*` from `loop()` or from SDK/lwIP callbacks.
- Don't `mdns_free()`; it destroys ESPHome's records.
- Don't remove the GPIO interrupt disarm in `setup()`.
- Don't make the network patch silently skip on a mismatch; a build error is the safe failure.
- Keep `MM_IOT_SDK_COMMIT` pinned; bump it deliberately and re-test (firmware and BCF must come from the same SDK version).
