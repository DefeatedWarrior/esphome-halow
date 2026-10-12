# esphome-halow: patches for ESPHome 2026.x

The upstream [`sweitzja/esphome-halow`](https://github.com/sweitzja/esphome-halow) component (commit `1b0b3a7`, May 2026) was built and tested on ESPHome's older PlatformIO toolchain with ESP-IDF 5.5.2. ESPHome 2026.x builds ESP32 projects with its own native ESP-IDF toolchain by default and starts the network stack itself. Those two changes break the component in four separate ways:

| # | Symptom | Stage | File |
|---|---------|-------|------|
| 1 | `fatal error: esp_netif_lwip_internal.h: No such file or directory` | Compile | `halow_component.cpp` |
| 2 | `undefined reference to mbedtls_cipher_cmac_starts` (and `_update`, `_finish`) | Link | `__init__.py` |
| 3 | Boot loop: interrupt watchdog panic in `gpio_intr_service` | Runtime | `halow_component.cpp` |
| 4 | Boot loop: `ESP_ERROR_CHECK failed: ESP_ERR_INVALID_ARG` in `esp_vfs_lwip_sockets_register` | Runtime | `halow_component.cpp`, `__init__.py` |

All four fixes are needed. Every fix is confined to `components/halow/`; the Morse Micro SDK and ESPHome itself are untouched.

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

- **Reaching the device from the main LAN, and the Home Assistant API connection.** The test Mac sat on a different subnet with no route to the HaLow subnet, so this couldn't be checked. It depends on the AP bridging to the LAN; see the upstream README's "Access Point Setup".
- **OTA over HaLow**, and recovery after an AP restart or after going out of range.
- **Long-running stability.**
- **The PlatformIO toolchain** (`toolchain: platformio`) with these patches. Fixes #1 and #2 should be harmless there. Fix #4 does nothing when only one `tcpip_init()` call is made.

## Files changed

```
components/halow/__init__.py          +5   (CMAC sdkconfig option, --wrap=tcpip_init)
components/halow/halow_component.cpp  +29 -1  (header path, GPIO disarm, tcpip_init wrapper)
```
