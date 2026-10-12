# esphome-halow: patches for ESPHome 2026.x

The upstream [`sweitzja/esphome-halow`](https://github.com/sweitzja/esphome-halow) component (commit `1b0b3a7`, May 2026) was built and tested on ESPHome's older PlatformIO toolchain with ESP-IDF 5.5.2. ESPHome 2026.x builds ESP32 projects with its own native ESP-IDF toolchain by default and starts the network stack itself. Those two changes break the component in four separate ways:

| # | Symptom | Stage | File |
|---|---------|-------|------|
| 1 | `fatal error: esp_netif_lwip_internal.h: No such file or directory` | Compile | `halow_component.cpp` |
| 2 | `undefined reference to mbedtls_cipher_cmac_starts` (and `_update`, `_finish`) | Link | `__init__.py` |
| 3 | Boot loop: interrupt watchdog panic in `gpio_intr_service` | Runtime | `halow_component.cpp` |
| 4 | Boot loop: `ESP_ERROR_CHECK failed: ESP_ERR_INVALID_ARG` in `esp_vfs_lwip_sockets_register` | Runtime | `halow_component.cpp`, `__init__.py` |

All four fixes are needed. Every fix is confined to `components/halow/`; the Morse Micro SDK and ESPHome itself are untouched.

[Part 2](#part-2-fixes-from-code-review) covers a second round of fixes from a full review of the component, including one more ESPHome 2026.x break that would have dropped every Home Assistant connection.

## Test setup

- **ESPHome** 2026.9.1, native ESP-IDF toolchain, ESP-IDF 5.5.5 (the Home Assistant OS builder's default)
- **Host:** Seeed XIAO ESP32-S3 (8 MB flash, 8 MB PSRAM)
- **Radio:** Seeed XIAO HaLow Hat (MM6108), Morse firmware 1.17.6, morselib 2.10.4-esp32, chip ID `0x306`
- **Network:** an 802.11ah AP using WPA3-SAE in the Australian region (`country_code: "AU"`)

---

## 1. Missing internal ESP-IDF header (compile error)

**Symptom**

```
halow_component.cpp:42:10: fatal error: esp_netif_lwip_internal.h: No such file or directory
```

**Cause.** The component needs `struct esp_netif_obj` from ESP-IDF's private header `components/esp_netif/lwip/esp_netif_lwip_internal.h`, which it uses to wrap the HaLow netif for mDNS. `__init__.py` added that folder to the include path, but it hardcoded PlatformIO's copy of the framework:

```python
esp_netif_internal = os.path.join(
    os.path.expanduser("~/.platformio"), "packages", "framework-espidf",
    "components", "esp_netif", "lwip"
)
if os.path.isdir(esp_netif_internal):
    cg.add_build_flag(f"-I{esp_netif_internal}")
```

The native toolchain keeps ESP-IDF somewhere else (for example `/data/cache/idf/frameworks/5.5.5` in HAOS). That folder doesn't exist, so the `-I` flag is skipped without any warning and the `#include` fails.

**Fix.** Include the header relative to `esp_netif`'s public `include/` directory, which every ESP-IDF build already has on the include path. That header only pulls in other public headers (`esp_netif.h`, `esp_netif_ppp.h`, `lwip/esp_netif_net_stack.h`, `dhcpserver/dhcpserver.h`), so no extra include path is needed. The old `-I` block in `__init__.py` is left in place; it's harmless.

```diff
--- a/components/halow/halow_component.cpp
+++ b/components/halow/halow_component.cpp
@@ -39,7 +39,9 @@
 // Internal header for esp_netif_obj struct layout — needed to create
 // a minimal wrapper around mmipal's raw LWIP netif for mDNS registration.
 // The struct layout has been stable since ESP-IDF v4.x.
-#include "esp_netif_lwip_internal.h"
+// Reached relative to esp_netif's public include dir so it resolves under both
+// the PlatformIO and native ESP-IDF toolchains.
+#include "../lwip/esp_netif_lwip_internal.h"
 }
```

---

## 2. mbedTLS CMAC disabled (link error)

**Symptom**

```
libmm_shims.a(crypto_mbedtls_mm.c.obj): in function `mmint_omac1_aes_vector':
crypto_mbedtls_mm.c:715: undefined reference to `mbedtls_cipher_cmac_starts'
crypto_mbedtls_mm.c:737: undefined reference to `mbedtls_cipher_cmac_update'
crypto_mbedtls_mm.c:735: undefined reference to `mbedtls_cipher_cmac_finish'
```

**Cause.** Morse's crypto shim implements OMAC1 (AES-CMAC) with mbedTLS. WPA3-SAE and protected management frames need it. ESPHome 2026.x's generated `sdkconfig` leaves `# CONFIG_MBEDTLS_CMAC_C is not set`, so those functions are never compiled.

**Fix.** Turn the option on alongside the existing `NIST_KW` crypto option:

```diff
--- a/components/halow/__init__.py
+++ b/components/halow/__init__.py
@@ -444,6 +444,7 @@
     # Kconfig: ESP32-S3
     add_idf_sdkconfig_option("CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB", True)
     add_idf_sdkconfig_option("CONFIG_MBEDTLS_NIST_KW_C", True)
+    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CMAC_C", True)  # OMAC1 for WPA3/PMF; ESPHome disables it by default
```

---

## 3. GPIO interrupt storm on reboot (boot loop)

**Symptom.** Right after `MM6108 hardware reset complete`, the board panics, reboots and repeats:

```
Guru Meditation Error: Core 1 panic'ed (Interrupt wdt timeout on CPU1).
  gpio_intr_service          gpio.c:519
  gpio_isr_register_on_core_static
  esp_intr_alloc
```

**Cause.** The MM6108's SPI IRQ line is configured as a **LOW_LEVEL** GPIO interrupt (`mmhal_wlan_set_spi_irq_enabled()` in the Morse shim). A software reset (a panic, an `abort()`, or a watchdog) doesn't reset the ESP32-S3's GPIO interrupt registers, and the MM6108 isn't power-cycled either. The next boot therefore starts with the IRQ pin still armed while the radio holds it low. `mmhal_init()` then calls `gpio_install_isr_service()`, and the interrupt fires straight away before any handler is attached. ESP-IDF clears the status bit, but a level-triggered pin fires again at once, so the CPU never gets out of the ISR and the interrupt watchdog trips.

In this case the soft reset that started the loop was most likely the abort from fix #4, which happens after the radio has booted and armed its IRQ. Even with #4 fixed, any later crash or watchdog reset would have caused the same loop, so this fix is still needed.

**Fix.** Disarm the IRQ and BUSY pins at the start of `setup()`, before the SDK installs its ISR service. The SDK re-enables them itself once its handlers are registered.

```diff
--- a/components/halow/halow_component.cpp
+++ b/components/halow/halow_component.cpp
@@ -231,6 +251,14 @@
   periph_module_reset(PERIPH_SPI2_MODULE);
   spi_bus_free(SPI2_HOST);  // Ignore error — may not be initialized
+
+  // GPIO interrupt config survives a software reset. If the previous boot left the
+  // IRQ pin armed as LOW_LEVEL, installing the ISR service in mmhal_init() storms
+  // with no handler attached and trips the interrupt watchdog. Disarm both pins.
+  for (int pin : {this->spi_irq_pin_, this->busy_pin_}) {
+    gpio_intr_disable((gpio_num_t) pin);
+    gpio_set_intr_type((gpio_num_t) pin, GPIO_INTR_DISABLE);
+  }
 
   // Hardware-reset the MM6108 to ensure clean chip state.
```

---

## 4. lwIP initialised twice (boot loop)

**Symptom.** The radio firmware boots and the log reaches `Using DHCP`, then:

```
ESP_ERROR_CHECK failed: esp_err_t 0x102 (ESP_ERR_INVALID_ARG)
  esp_vfs_lwip_sockets_register   vfs_lwip.c:112
  sys_init                        sys_arch.c:419
  lwip_init                       init.c:355
  tcpip_init                      tcpip.c:652
  mmipal_init                     mmipal_lwip.c:791
  esphome::halow::HalowComponent::setup()
```

**Cause.** ESPHome 2026.x has a `network` component that calls `esp_netif_init()` early in boot, and that starts the lwIP tcpip thread through `tcpip_init()`. Later, Morse's `mmipal_init()` calls `tcpip_init()` again on its own. The second call re-runs `lwip_init()`, which tries to register the lwIP socket VFS a second time. ESP-IDF rejects the duplicate registration, and `ESP_ERROR_CHECK` aborts.

**Fix.** Wrap `tcpip_init` at link time, the same way the component already wraps `tcpip_input`. The first call (ESPHome's) goes through unchanged. Any later call (Morse's) does not re-initialise lwIP; it queues `mmipal`'s init-done callback onto the tcpip thread that's already running, which is what `mmipal_init()` is waiting for.

The wrapper lives in `halow_component.cpp` rather than `network_wrap.cpp`. The only references to `__wrap_tcpip_init` come from `libesp_netif.a` and `libmmipal.a`, which are linked after the component's library, so a definition in `network_wrap.cpp` was never pulled in and the link failed. `halow_component.cpp` is always linked.

```diff
--- a/components/halow/halow_component.cpp
+++ b/components/halow/halow_component.cpp
@@ -187,6 +189,24 @@
   }
 }
 
+// ESPHome's network component calls esp_netif_init(), which starts the lwIP
+// tcpip thread. mmipal_init() then calls tcpip_init() again, and lwIP aborts
+// re-registering its socket VFS. Let the first call through; for later calls
+// just run the init-done callback on the already-running tcpip thread.
+extern "C" {
+  extern void __real_tcpip_init(tcpip_init_done_fn initfunc, void *arg);
+  void __wrap_tcpip_init(tcpip_init_done_fn initfunc, void *arg) {
+    static bool s_tcpip_started = false;
+    if (!s_tcpip_started) {
+      s_tcpip_started = true;
+      __real_tcpip_init(initfunc, arg);
+      return;
+    }
+    if (initfunc != nullptr)
+      tcpip_callback(initfunc, arg);
+  }
+}
+
 // --- Component Lifecycle ---
```

```diff
--- a/components/halow/__init__.py
+++ b/components/halow/__init__.py
@@ -477,6 +478,10 @@
     # Wrap tcpip_input for RX byte counting — the SDK calls tcpip_input()
     # directly instead of going through netif->input
     cg.add_build_flag("-Wl,--wrap=tcpip_input")
+
+    # Wrap tcpip_init so mmipal_init() doesn't re-initialise the lwIP stack
+    # that ESPHome's network component already started
+    cg.add_build_flag("-Wl,--wrap=tcpip_init")
```

---

## Configuration notes (not code changes)

- **`country_code` must match the AP's region.** With `"US"` (902–928 MHz) the board could not find an AU AP and timed out after 60 s on every attempt. With `"AU"` (915–928 MHz) it associated in about 2.5 s. `AU` is in Morse's regulatory database (`mmregdb.c`).
- **Toolchain.** No `toolchain:` override is needed; the patched component builds with ESPHome's default native ESP-IDF toolchain.
- **Component source.** These fixes are published in the [`DefeatedWarrior/esphome-halow`](https://github.com/DefeatedWarrior/esphome-halow) fork:

  ```yaml
  external_components:
    - source: github://DefeatedWarrior/esphome-halow
      components: [halow]
  ```

## Results

**Verified** on the hardware above, over the USB serial log:

- It compiles and links: 59.4% flash (1,090,835 B), 25.9% RAM.
- It boots with no panics or boot loops, and ESPHome's safe-mode boot counter resets after 60 s.
- The MM6108 firmware loads: `Morse FW 1.17.6, morselib 2.10.4-esp32, chip 0x306`.
- WPA3-SAE association: `STA state: CONNECTING` → `CONNECTED` → `HaLow link UP`.
- It gets an address by DHCP and sees the gateway.
- mDNS registers `halow1.local` and the `_esphomelib._tcp` service on the HaLow interface.

**Not yet verified:**

- **Reaching the device from the main LAN, and the Home Assistant API connection.** The test Mac sat on a different subnet with no route to the HaLow subnet, so this couldn't be checked. (Code review later showed the API connection would have failed on 2026.x anyway; see [Part 2, fix 5](#5-esphome-2026x-drops-every-api-connection).) It depends on the AP bridging to the LAN; see the upstream README's "Access Point Setup".
- **OTA over HaLow**, and recovery after an AP restart or after going out of range.
- **Long-running stability.**
- **The PlatformIO toolchain** (`toolchain: platformio`) with these patches. Fixes #1 and #2 should be harmless there. Fix #4 does nothing when only one `tcpip_init()` call is made.

## Files changed

```
components/halow/__init__.py          +5   (CMAC sdkconfig option, --wrap=tcpip_init)
components/halow/halow_component.cpp  +29 -1  (header path, GPIO disarm, tcpip_init wrapper)
```

---

# Part 2: fixes from code review

A full review of the component turned up one more ESPHome 2026.x break and a set of bugs that predate it. All of these are on the `improvements` branch.

**How they were verified:** everything compiles and links on ESPHome 2026.9.1 (59.8% flash, 26.0% RAM with every sensor enabled). Config validation was exercised with good and bad configs. The network patch was confirmed in the build output (the API server's object file calls `halow::network_is_connected()`). The pinned SDK download was run into an empty folder. **None of the runtime behaviour in this part has been tested on hardware yet**: the test board was resetting whenever its radio powered up (a hardware/power fault, see the README's Troubleshooting), so nothing past "Connecting" could be exercised.

| # | Problem | Severity |
|---|---------|----------|
| 5 | ESPHome 2026.x drops every API connection | Critical |
| 6 | Reconnect race between the main loop and the timer | High |
| 7 | Timer reconnect ignored the configured security type | High |
| 8 | SSID/password lengths unchecked (buffer overflow) | High |
| 9 | Morse SDK download not pinned | High |
| 10 | `mode: AP` and `type: MM8108` accepted but non-functional | Medium |
| 11 | Scan results exceed Home Assistant's 255-char limit, unsynchronised | Medium |
| 12 | mDNS teardown discarded ESPHome's discovery records | Medium |
| 13 | `country_code` silently defaulted to `US` | Medium |
| 14 | Smaller cleanups | Low |

## 5. ESPHome 2026.x drops every API connection

**Problem.** ESPHome's API server checks `network::is_connected()` on every loop and, if it's false, disconnects all clients ("Network down; disconnect"). With no client ever staying connected it then reboots the device after `reboot_timeout` (15 minutes by default). MQTT, `http_request`, the `status` binary sensor and others use the same check. `is_connected()` only knows ESPHome's built-in interfaces, so the original component overrode it with linker `--wrap`. In ESPHome 2026.x `is_connected()` became `ESPHOME_ALWAYS_INLINE` in `network/util.h`, so there is no symbol to wrap, and `get_use_address()` was replaced by `get_use_address_to()`. All three wraps silently did nothing; `nm` on the firmware shows no `__wrap_` symbols at all.

**Fix.** `_patch_network_util()` in `__init__.py` reads ESPHome's own `network/util.h` and `util.cpp`, inserts a `USE_HALOW` branch at the top of `is_connected()` and `get_ip_addresses()`, and registers the patched copies with `add_extra_build_file()`. ESP32 `copy_files()` runs after ESPHome copies its sources, so they replace the originals in the build tree. Because it patches whatever ESPHome version is installed, it should survive minor changes; if it can't find the functions it stops the build with a clear error instead of producing firmware that drops connections. `network_wrap.cpp` and its `--wrap` flags are removed. `get_use_address_to()` needs no hook, since it already returns `<name>.local`.

```cpp
ESPHOME_ALWAYS_INLINE inline bool is_connected() {
#ifdef USE_HALOW
  if (halow::network_is_connected())
    return true;
#endif  // USE_HALOW
  ...
```

## 6. Reconnect race

**Problem.** When the link dropped, `loop()` moved to `CONNECTING` without resetting the connect timer, so on the next pass the 60 s timeout had already "expired". It went to `STOPPED` and called `mmwlan_sta_enable()` from the main loop 5 s later, while the FreeRTOS timer the link-down callback had started did `sta_disable()` + `sta_enable()` 10 s after that. Two tasks were driving the radio at once, and the main-loop call is exactly what the author's own notes say wedges the SDK.

**Fix.** After the initial connect in `setup()`, only the timer task calls `mmwlan_sta_disable/enable`. `loop()` tracks the time of the last attempt (`s_last_attempt_ms`, updated by every `sta_enable` call and reset on link loss), and on a timeout it only schedules the timer if it isn't already pending (`schedule_reconnect()`). The `STOPPED` retry path is gone. The timer task's stack is raised to 4096 bytes (`CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH`), since these SDK calls and their logging now run on it.

## 7. Timer reconnect ignored `security`

**Problem.** `do_halow_reconnect()` hardcoded `MMWLAN_SAE`, so `OWE` and `OPEN` networks could connect once but never reconnect.

**Fix.** The initial connect and the reconnect now both build their arguments with `fill_sta_args()`, which uses the configured security type.

## 8. SSID/password lengths unchecked

**Problem.** The SSID and passphrase were `memcpy`'d into the SDK's fixed 32- and 100-byte buffers using the string's length, and the schema accepted any length. A 33+ character SSID overwrote the stack.

**Fix.** The schema limits `ssid` to 1–32 and `password` to 100 characters, and `fill_sta_args()` clamps to the buffer sizes as well. `password` is now optional (it isn't used for `OPEN`/`OWE`) but required for `SAE`.

## 9. Morse SDK download not pinned

**Problem.** The SDK was fetched with `git clone --depth 1` of whatever was on the default branch. Any new Morse commit could change the firmware or break the build, and the HAOS add-on can wipe `/root/.esphome` on updates, which triggers a fresh clone.

**Fix.** The download now fetches exactly `MM_IOT_SDK_COMMIT` (`96d9bf4`, morselib 2.10.4, firmware 1.17.6). An existing checkout at a different commit logs a warning. The BCF file name is also checked against the SDK at compile time.

## 10. `mode: AP` and `type: MM8108`

**Problem.** Both were accepted and documented, but there is no AP code at all, and `MM8108` still loaded `mm6108.mbin` with an MM6108 board config.

**Fix.** Config validation rejects them with an explanation. The README lists station mode and MM6108 as the supported configuration.

## 11. Scan results

**Problem.** The scan JSON listed every AP with long field names, which easily exceeds Home Assistant's 255-character text sensor limit. The string was also appended to from the SDK's task while the main loop read and cleared it, with no lock. The noise floor kept the *highest* (worst) reading, contradicting its comment.

**Fix.** Results are collected into a mutex-protected list (capped at 32). On completion they are sorted strongest first and published as compact JSON (`ssid`, `rssi`, `noise`, `mhz`, `bw`), stopping at the last whole entry that fits in 255 characters. SSIDs are sanitised for JSON. The noise floor comes from the channel of the connected AP, or the quietest channel if that AP wasn't seen. Every result is still written to the debug log.

## 12. mDNS

**Problem.** `start_mdns_()` called `mdns_free()` and re-initialised the responder, discarding the services ESPHome's mdns component had registered, along with their full TXT records (`mac`, `friendly_name`, `api_encryption`, ...). It then re-added a minimal `_esphomelib._tcp` record. Nothing was re-announced after a reconnect.

**Fix.** `mdns_init()` is a no-op when ESPHome has already started the responder, so the component no longer frees it; it registers its HaLow netif wrapper alongside ESPHome's services. `_esphomelib._tcp` is only added (now with `mac` and `friendly_name`) if ESPHome didn't register it. On reconnects the address is re-announced.

## 13. `country_code` default

**Problem.** `country_code` defaulted to `US` and wasn't validated. A device outside the US that omitted it couldn't find its AP, which is what happened during testing, and would transmit on the wrong band.

**Fix.** `country_code` is required and must be one of the regions in the pinned SDK's regulatory database: `AU`, `CA`, `EU`, `GB`, `IN`, `JP`, `KR`, `NZ`, `US` (case-insensitive).

## 14. Smaller changes

- **New `max_tx_power` option** (1–30 dBm), using `mmwlan_override_max_tx_power()`. It can only lower power below the regulatory limit.
- Pins are validated as distinct ESP32-S3 GPIOs (0–48).
- Removed the unused `full_radio_reset_()`, the unused traffic-watchdog members and `set_scan_button()`.
- `sta_status_cb` bounds-checks the state before indexing its name table.
- Removed the `-I~/.platformio/...` include path (superseded by fix 1). `pre_build.py` is only registered for the PlatformIO toolchain.
- README: pin table fixed (five XIAO labels were off by one), Troubleshooting section added, configuration table and scan-result docs updated, ESP32-C6 marked unsupported.
