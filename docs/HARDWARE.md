# Hardware

## Host: Seeed XIAO ESP32-S3

- ESP32-S3 rev 0.2, 8 MB flash, 8 MB PSRAM.
- USB is the chip's built-in USB-Serial-JTAG (VID `0x303a`, PID `0x1001`); macOS shows it as `/dev/cu.usbmodem*`, Linux as `/dev/ttyACM*`. The port disappears whenever the chip resets fully, and the device name can change between USB ports or hubs.
- `esp32: variant: esp32s3` without a `board:` builds for 4 MB flash; `board: seeed_xiao_esp32s3` uses all 8 MB. The partition table differs, so switching between them needs a USB flash, not OTA.

## HaLow module: Seeed Wio-WM6180 for XIAO

MM6108 SoC on an MF16858 module. Plugs onto the XIAO headers.

### Pins

Verified against Seeed's SDK (`Seeed-Studio/mm-iot-esp32`, `framework/mm_shims/Kconfig`) and identical in Morse's upstream HAL.

| Function | GPIO | XIAO label | Direction | How the SDK uses it |
|----------|------|------------|-----------|---------------------|
| SPI SCK | 7 | D8 | out | SPI2 at 40 MHz |
| SPI MISO | 8 | D9 | in | |
| SPI MOSI | 9 | D10 | out | |
| SPI CS | 4 | D3 | out | manual chip select |
| SPI IRQ | 3 | D2 | in | data-ready, LOW_LEVEL interrupt (GPIO3 is also an S3 strapping pin) |
| RESET_N | 1 | D0 | out | held low until `mmhal_wlan_init()`, pulsed for hard reset |
| WAKE | 2 | D1 | out | asserted to wake the chip |
| BUSY | 5 | D4 | in, pull-down | rising-edge interrupt; unreliable in firmware 1.17.6, hence power save off |

The SDK's hardware layer (`mm_shims`) has no other pins: no RF switch, power enable or antenna select. The RF front end is controlled by the MM6108 itself according to the BCF. Seeed's HAL and Morse's upstream HAL were compared line by line in October 2026 and behave identically (same pins, 40 MHz SPI, reset timing, interrupt setup, and the external-crystal hook defaulting to "not required").

### Power

From Seeed's wiki: the module needs **two supplies**. **3.3 V** powers the digital rails; **5 V** powers the RF front end (TX power amplifier) on a separate rail. On a XIAO, the 5 V pin is the USB supply itself (VBUS). With only 3.3 V the module loads firmware and receives, but can't transmit at rated power.

The RF section is powered up by `mmwlan_sta_enable()` ("power on the transceiver then initiate connection"), i.e. at the `Connecting to '...'` log line, not during `mmwlan_boot()`.

### Firmware and board config

| File | Version / details |
|------|-------------------|
| `mm6108.mbin` | 1.17.6 (~400 KB), from the pinned MM-IoT-SDK |
| `bcf_mf16858.mbin` | `{"module": "mm6108-mf16858", "status": "Regulatory Certified", "regions_certified": {"US": "certified", "CA": "certified", "AU": "compliant"}, "updated": "2025-10-28"}` |

Seeed's own SDK defaults to `bcf_mf16858_us` (an older, smaller BCF with its own older firmware). The upstream one above is newer and covers AU. Firmware and BCF must come from the same SDK version.

## Access point: GL-iNet HaLowLink 2

- MM8108-based, WPA3-SAE. Must bridge HaLow to the LAN for Home Assistant to reach devices: LAN cable in the **WAN** port, the wizard's "HaLow devices get IP on your existing router's network", and masquerade off (`uci set firewall.wlan.masq=0`). See the README's Access Point Setup.
- The device's `country_code` must match the AP's region. Supported by the pinned SDK: AU, CA, EU, GB, IN, JP, KR, NZ, US. A US device does not find an AU AP (different channelisation), and just times out.

## Failure signatures

| Symptom in the serial log | Cause | Status |
|---------------------------|-------|--------|
| `fatal error: esp_netif_lwip_internal.h` (build) | PlatformIO-only include path | Fixed (PATCHES.md #1) |
| `undefined reference to mbedtls_cipher_cmac_*` (link) | ESPHome 2026.x disables CMAC | Fixed (#2) |
| `Interrupt wdt timeout on CPU1` in `gpio_intr_service` right after `MM6108 hardware reset complete`, boot loop | IRQ pin left armed across a soft reset | Fixed (#3) |
| `ESP_ERROR_CHECK failed: ESP_ERR_INVALID_ARG` in `esp_vfs_lwip_sockets_register` after `Using DHCP` | lwIP initialised twice | Fixed (#4) |
| `Network down; disconnect` from the API, reboot every 15 min | `network::is_connected()` doesn't know HaLow | Fixed (#5) |
| `Connect timeout` with no `STA state: CONNECTING` | AP not found: wrong `country_code`, AP off or out of range | Config |
| Reboot every 15 min, no API errors | Home Assistant can't reach the device (`api: reboot_timeout`) | Network/AP setup |
| Log stops right after `Connecting to '...'`, USB port drops, next boot's reset reason is **1 (power-on)**, no crash record | Supply collapses when the RF front end powers up | Hardware, see below |

Reset reasons (`esp_reset_reason()`): 1 power-on (also the EN pin), 3 software, 4 panic, 5 interrupt WDT, 6 task WDT, 7 other WDT, 9 brownout, 11 USB (esptool/serial reset). A panic or software restart does **not** drop the USB serial port; a power-on reset does.

## Investigation log: power-on reset at "Connecting" (2026-10-12/13)

Board: XIAO ESP32-S3 + Wio-WM6180, HaLowLink 2 AP, AU region.

- 23:42: firmware with fixes #1-#4, `country_code: US`: scanned and retried for over 60 s with no reset (AP not found).
- 23:45: same with `AU`: **connected**, DHCP lease, mDNS up, stable past 60 s.
- From ~23:55 every boot resets about 0.4-1.5 s after `Connecting to '<ssid>' (SAE)...`. The USB device name changed between these (`usbmodem1101` → `usbmodem101`).
- Recreated the exact 23:45 build (same sequence, same code): still resets. So not the build.
- Delayed connect by 15 s with a heartbeat: stable for 15 s with the radio booted, resets the instant `sta_enable()` runs. Reset reason 1 (power-on), no panic, no assert output.
- TX power capped at 10 dBm: still resets. US country code (AP not reachable): still resets. Back on the `usbmodem1101` port: still resets.
- No persistent state exists that could carry over between flashes (only ESPHome's NVS preferences; the Morse config store isn't linked).
- Morse SDK's internal assert path calls `esp_restart()`, which would give reason 3 and keep the USB port, so ruled out.
- Seeed's HAL compared with upstream: identical. Seeed's wiki then showed the 5 V PA rail comes straight from USB, which explains a power-on reset at RF power-up regardless of TX power.

**Conclusion:** a power problem on the 5 V path feeding the module's TX amplifier.

- 2026-10-13 17:24: **an electrolytic capacitor added across the 5 V rail stopped the power-on resets.** The board now stays up after `Connecting` (20+ s observed); the only resets in the log were USB-requested (`rst:0x15`, "Reset Reason: USB peripheral") from the web serial page. Still open: no `STA state: CONNECTING` appeared within 20 s, so association isn't confirmed yet. Needs an uninterrupted log of at least 2 minutes (the first connect timeout is at 60 s).
- 2026-10-13 17:27-17:33: no power resets. The board didn't find the AP for 6 minutes (first attempt plus 5 timer reconnects, 75 s apart, no crash). 17:34: after a USB reset it **connected in 2 s**, got a DHCP lease and started mDNS, and stayed up. The 17:24 and 17:27 boots were also fresh restarts and failed, so a restart alone isn't what fixed it; most likely the AP wasn't answering until then (possibly holding off the MAC after the earlier reset loop). Watch for a repeat.
