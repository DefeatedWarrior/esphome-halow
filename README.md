# esphome-halow

ESPHome external component for **Wi-Fi HaLow** (IEEE 802.11ah) using the [Morse Micro MM6108](https://www.morsemicro.com/chips/). It replaces `wifi:` with `halow:`: configure the SSID, password and region in YAML, and the device joins a HaLow access point with full IP networking, so the Home Assistant API, OTA updates, mDNS and MQTT all work over sub-1 GHz radio with up to ~1 km range.

> **This is a fork** of [sweitzja/esphome-halow](https://github.com/sweitzja/esphome-halow). It makes the component build and run on **ESPHome 2026.x** (the native ESP-IDF toolchain used by the Home Assistant ESPHome Builder) and fixes bugs found in review, including Home Assistant connections being dropped on 2026.x. Every change is listed with its reasoning in [PATCHES.md](PATCHES.md). The state before the review fixes is kept on the [`pre-improvements`](https://github.com/DefeatedWarrior/esphome-halow/tree/pre-improvements) branch.

### Why HaLow over LoRa?

HaLow uses standard IP networking: TCP, UDP, DHCP, mDNS, MQTT and the ESPHome API all work natively, with no protocol translation, gateway bridges or TTN accounts. A HaLow device is just another IP device on your network that Home Assistant sees directly, OTA updates work over the same link, and bidirectional control is fast (~7 ms). LoRa still wins on raw range (10-15 km vs ~1 km) and power for simple one-way sensors; for anything that needs reliable two-way communication, HaLow is much cleaner.

## Status

| | Original component | This fork |
|---|---|---|
| ESPHome | before 2026.x (PlatformIO toolchain) | **2026.9.1** (native ESP-IDF 5.5.5) |
| Builds, boots MM6108, joins WPA3-SAE AP, DHCP, mDNS | Tested | Tested |
| Home Assistant API, OTA over HaLow, reconnect after range loss / AP restart, 20-min soak | Tested | Fixed for 2026.x and compile-verified; **not yet re-tested on hardware** |

Details: [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#test-history).

## Hardware

| Part | Status | Notes |
|------|--------|-------|
| [Seeed Wio-WM6180 HaLow module for XIAO](https://www.seeedstudio.com/Wio-WM6180-Wi-Fi-HaLow-Module-for-XIAO-p-6395.html) | Tested | MM6108 (MF16858 module); default pins match |
| Seeed XIAO ESP32-S3 | Tested | Other ESP32-S3 boards should work with `*_pin` options |
| ESP32-C3 / C6 / P4 | Untested | The MM-IoT-SDK ships libraries for these |
| [GL-iNet HaLowLink 2](https://www.gl-inet.com/products/halowlink2/) access point | Tested | Needs bridging to your LAN, see [Access Point Setup](#access-point-setup-required) |
| Other 802.11ah APs | Should work | WPA3-SAE, OWE or open |

**Power:** the HaLow module's transmit amplifier runs from the **5 V** pin, which on a XIAO is the USB supply. It draws a surge when the radio powers up at "Connecting". Use a good USB cable and port or a powered hub; a weak 5 V supply shows up as the board resetting at that moment (see [Troubleshooting](#troubleshooting)).

Default pins (Seeed XIAO ESP32-S3 + Wio-WM6180):

| Function | GPIO | XIAO pin |
|----------|------|----------|
| SPI SCK / MISO / MOSI | 7 / 8 / 9 | D8 / D9 / D10 |
| SPI CS | 4 | D3 |
| SPI IRQ | 3 | D2 |
| RESET_N | 1 | D0 |
| WAKE | 2 | D1 |
| BUSY | 5 | D4 |

More in [docs/HARDWARE.md](docs/HARDWARE.md).

## Quick start (Home Assistant ESPHome Builder)

1. Set up your access point to bridge HaLow onto your LAN: [Access Point Setup](#access-point-setup-required).
2. Add your HaLow network to the builder's **Secrets**:
   ```yaml
   halow_ssid: "your-halow-ssid"
   halow_password: "your-halow-password"
   ```
3. Create a device with this YAML (adjust the name and `country_code`):
   ```yaml
   esphome:
     name: halow-sensor
     friendly_name: HaLow Sensor

   esp32:
     board: seeed_xiao_esp32s3
     framework:
       type: esp-idf

   external_components:
     - source: github://DefeatedWarrior/esphome-halow
       components: [halow]

   halow:                       # replaces wifi:
     ssid: !secret halow_ssid
     password: !secret halow_password
     country_code: "US"         # required; must match your region and AP
     rssi:
       name: "HaLow RSSI"
     link_quality:
       name: "HaLow Link Quality"
     ip_address:
       name: "HaLow IP"

   logger:
   api:
   ota:
     - platform: esphome
   ```
4. Install over USB the first time. The first compile also downloads the Morse Micro SDK (about 50 MB), pinned to the tested version.

[example.yaml](example.yaml) has every option and sensor.

## Access Point Setup (Required)

The HaLowLink 2 **must be in AP mode** and configured for **bridging** so that Home Assistant
and other LAN devices can reach HaLow devices. Without this setup, the ESP32 will connect
to the HaLow AP but will not be reachable from your network — HA won't be able to discover
or communicate with the device.

1. Set the HaLowLink 2 to **AP mode** using the mode button or web UI
2. Connect your LAN cable to the HaLowLink 2's **WAN port** (not LAN — the wizard bridges WAN↔HaLow)
3. Access the web UI at `https://<halowlink-ip>` (check your router's DHCP leases for the IP)
4. Go to **Wizard** and select **"HaLow devices get IP on your existing router's network"**
5. SSH in and disable masquerade:
   ```bash
   ssh root@<halowlink-ip>   # password is on the device label
   uci set firewall.wlan.masq=0
   uci commit firewall
   /etc/init.d/firewall restart
   ```

**Important**: Step 5 is critical. Even in bridge mode, the HaLowLink 2 enables NAT (masquerade)
on the HaLow interface by default. Without disabling it, HaLow devices can reach the internet
but nothing on your LAN can reach them — no ping, no API, no OTA, no HA discovery.

## Configuration

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `ssid` | string | **required** | HaLow AP SSID (1-32 characters) |
| `password` | string | *(empty)* | Passphrase, up to 100 characters. Required for `SAE` |
| `country_code` | string | **required** | Regulatory domain: `AU`, `CA`, `EU`, `GB`, `IN`, `JP`, `KR`, `NZ` or `US`. Must match the AP |
| `security` | enum | `SAE` | Security type: `SAE` (WPA3), `OWE`, or `OPEN` |
| `type` | enum | `MM6108` | Chip type. Only `MM6108` is supported |
| `mode` | enum | `STATION` | Operating mode. Only `STATION` is supported |
| `bcf_file` | string | `bcf_mf16858.mbin` | Board configuration file; checked against the SDK at compile time |
| `sub_bands` | bool | `false` | `false` = 8 MHz max throughput, `true` = 1-2 MHz max range |
| `max_tx_power` | int | *(regulatory max)* | Cap TX power, 1-30 dBm. Can only lower power, never exceed the regulatory limit |
| `manual_ip` | schema | *(DHCP)* | Static IP config (static_ip, gateway, subnet, dns1, dns2). Set `dns1` if the device needs name resolution (e.g. SNTP, HTTP) |
| `mm_iot_sdk_path` | string | `~/.esphome/mm-iot-esp32` | Where the MM-IoT-SDK is downloaded / found |
| `clk_pin` | int | `7` | SPI clock GPIO |
| `mosi_pin` | int | `9` | SPI MOSI GPIO |
| `miso_pin` | int | `8` | SPI MISO GPIO |
| `cs_pin` | int | `4` | SPI chip select GPIO |
| `irq_pin` | int | `3` | SPI interrupt GPIO |
| `reset_pin` | int | `1` | Module reset GPIO (active low) |
| `wake_pin` | int | `2` | Module wake GPIO |
| `busy_pin` | int | `5` | Module busy GPIO |

Pins must be distinct ESP32-S3 GPIOs (0-48).

### Sensors

All sensors are optional. Add only the ones you need to your YAML. All sensors
accept the standard ESPHome sensor configuration options (`name`, `id`, `filters`,
`on_value`, etc.). Sensors update every 10 seconds while connected.

#### Signal Sensors

##### `rssi`
**Type**: Numeric sensor (dBm) | **Device class**: `signal_strength`

Received Signal Strength Indicator — measures signal power from the AP.

| RSSI Range | Signal Quality | Typical Scenario |
|-----------|----------------|------------------|
| -20 to -40 dBm | Excellent | Same room as AP, antenna tight |
| -40 to -60 dBm | Good | Adjacent room, 10-50m |
| -60 to -75 dBm | Fair | Through walls, 50-200m |
| -75 to -85 dBm | Weak | Edge of range, packet loss likely |
| Below -85 dBm | Critical | Connection will drop |

```yaml
rssi:
  name: "HaLow RSSI"
```

##### `link_quality`
**Type**: Text sensor

Human-friendly summary of the radio link, derived from MCS and TX success rate.
This is the single sensor most users should monitor — it combines the raw radio
metrics into a plain-English assessment.

| Value | MCS | TX Success Rate | What it means |
|-------|-----|-----------------|---------------|
| **Excellent** | 5-7 | >95% | Full speed, strong signal. Best throughput. |
| **Good** | 3-4 | >90% | Moderate speed, reliable. Normal operation. |
| **Fair** | 1-2 | >80% | Reduced speed but usable. Some retransmissions. |
| **Poor** | 0 | >50% | Minimum speed, marginal link. Expect latency spikes. |
| **Critical** | any | <50% | Severe packet loss. Connection at risk of dropping. |

```yaml
link_quality:
  name: "HaLow Link Quality"
```

#### Radio Adaptation Sensors

The HaLow radio **automatically adapts** its modulation and coding in real time based
on channel conditions — similar to how LTE switches between bars of signal. These
sensors let you observe the adaptation.

##### `mcs`
**Type**: Numeric sensor (0-7)

Modulation and Coding Scheme — how the radio trades speed for reliability.
The equivalent of LoRa's spreading factor, but changed automatically per-frame.

| MCS | Modulation | Approx Data Rate (8 MHz) | Use Case |
|-----|-----------|--------------------------|----------|
| 7 | 64-QAM | 32.5 Mbps | Strong signal, max throughput |
| 6 | 64-QAM | 26 Mbps | Strong signal |
| 5 | 16-QAM | 19.5 Mbps | Good signal |
| 4 | 16-QAM | 13 Mbps | Moderate signal |
| 3 | QPSK | 9.75 Mbps | Moderate signal |
| 2 | QPSK | 6.5 Mbps | Weak signal |
| 1 | BPSK | 3.25 Mbps | Weak signal |
| 0 | BPSK | 1.625 Mbps | Weakest signal, max range |

The rate controller probes upward conservatively (one MCS step every ~10s) and drops
instantly on packet loss. During testing, we observed MCS climb from 0 back to 7 over
approximately 90 seconds after restoring a good antenna connection.

```yaml
mcs:
  name: "HaLow MCS"
```

##### `bandwidth`
**Type**: Numeric sensor (MHz)

Channel width the radio is currently using. With `sub_bands: false` (default), this
stays at the full operating bandwidth negotiated with the AP (typically 8 MHz).
With `sub_bands: true`, the radio may drop to 1, 2, or 4 MHz for better sensitivity
at extreme range.

| Bandwidth | Relative Throughput | Relative Range |
|-----------|-------------------|----------------|
| 8 MHz | Highest | Shortest |
| 4 MHz | ~50% | ~1.4x |
| 2 MHz | ~25% | ~2x |
| 1 MHz | ~12.5% | ~2.8x |

```yaml
bandwidth:
  name: "HaLow Bandwidth"
```

##### `tx_success_rate`
**Type**: Numeric sensor (%)

Percentage of frames delivered to the AP without retransmission in the last update
period (10 seconds). This is calculated from delta-based tracking of the MM6108's
rate control statistics — it shows current performance, not lifetime average.

| Success Rate | Meaning |
|-------------|---------|
| 95-100% | Clean channel, no interference |
| 80-95% | Some retransmissions, still reliable |
| 50-80% | Significant packet loss, link degrading |
| Below 50% | Severe loss, connection at risk |

```yaml
tx_success_rate:
  name: "HaLow TX Success Rate"
```

#### Traffic Sensors

##### `tx_pps` / `rx_pps`
**Type**: Numeric sensor (packets per second)

Transmit and receive packet rates over the HaLow link. Useful for HA dashboard
charts showing traffic patterns. Typical values with HA connected:

| Condition | TX PPS | RX PPS |
|-----------|--------|--------|
| Idle (HA connected) | 1-3 | 15-20 |
| Sensor updates | 3-5 | 20-30 |
| OTA upload | 50-100 | 10-20 |

```yaml
tx_pps:
  name: "HaLow TX Rate"
rx_pps:
  name: "HaLow RX Rate"
```

##### `tx_kbps` / `rx_kbps`
**Type**: Numeric sensor (kilobits per second)

Throughput sensors measuring actual data transferred over the HaLow link.
TX bytes are counted at the LWIP link-output layer; RX bytes are counted
via a wrapped `tcpip_input` hook.

```yaml
tx_kbps:
  name: "HaLow TX Throughput"
rx_kbps:
  name: "HaLow RX Throughput"
```

#### Diagnostic Sensors

##### `noise_floor`
**Type**: Numeric sensor (dBm)

Background noise level measured during the last channel scan. Updated each time the
`scan_channel` button is pressed. Use this to identify interference on your channel.

| Noise Floor | Meaning |
|-------------|---------|
| -90 to -95 | Clean — no interference |
| -80 to -85 | Light interference |
| -70 to -75 | Moderate — consider changing AP channel |
| -60 or higher | Heavy interference — change channel immediately |

```yaml
noise_floor:
  name: "HaLow Noise Floor"
```

##### `tx_frames_dropped` / `rx_frames_dropped`
**Type**: Numeric sensor (total increasing)

Counts of frames dropped due to TX or RX queue overflow in the UMAC. Non-zero values
indicate the radio can't keep up — usually caused by sustained interference or
extremely high traffic.

##### `ccmp_failures`
**Type**: Numeric sensor (total increasing)

Count of CCMP (AES encryption) decryption failures. Non-zero values indicate
bit errors from interference corrupting encrypted frames.

##### `hw_restarts`
**Type**: Numeric sensor (total increasing)

Count of MM6108 hardware restart events. Should remain at 0 during normal operation.
Non-zero indicates severe issues (firmware crash, SPI failure, or extreme interference).

#### Channel Scan

##### `scan_channel`
**Type**: Button

Triggers an on-demand channel scan. The MM6108 scans all available channels and
reports every AP it finds. Results are published to the `scan_results` text sensor
and the `noise_floor` sensor is updated from the scan data.

##### `scan_results`
**Type**: Text sensor (JSON)

Compact JSON array of the APs found during the last channel scan, strongest first.
Home Assistant limits text sensor states to 255 characters, so only as many whole
entries as fit are included (typically three or four); every result is also written
to the debug log.

```json
[{"ssid":"my-ap","rssi":-41,"noise":-92,"mhz":924.0,"bw":8}]
```

Fields: `ssid`, `rssi` (dBm), `noise` (noise floor in dBm),
`mhz` (center frequency in MHz), `bw` (bandwidth in MHz).

The `noise_floor` sensor uses the reading from the channel of the AP you're
connected to, or the quietest channel if your AP wasn't seen.

```yaml
scan_channel:
  name: "HaLow Channel Scan"
scan_results:
  name: "HaLow Scan Results"
```

#### Network Info Sensors

##### `ip_address`
**Type**: Text sensor | Shows the device's current IPv4 address (DHCP or static).

##### `gateway_address`
**Type**: Text sensor | Shows the gateway/router IP address.

##### `subnet_mask`
**Type**: Text sensor | Shows the network subnet mask.

##### `connected_ssid`
**Type**: Text sensor | Shows the SSID of the connected HaLow AP. Published once on connect.

##### `bssid`
**Type**: Text sensor | Shows the MAC address of the connected AP (BSSID).

##### `mac_address`
**Type**: Text sensor | Shows the MM6108's own MAC address. Published once on connect.

##### `firmware_version`
**Type**: Text sensor | Shows the MM6108 firmware version (e.g., "1.17.6"). Published once on connect.

```yaml
ip_address:
  name: "HaLow IP"
gateway_address:
  name: "HaLow Gateway"
mac_address:
  name: "HaLow MAC"
firmware_version:
  name: "HaLow FW Version"
```

## Troubleshooting

**The board resets at "Connecting to '...'"** — the log stops right after `Connecting to '<ssid>' (SAE)...`, the USB serial port drops, and the next boot reports a power-on reset (`debug:` component: *Reset Reason: Power On*). `mmwlan_sta_enable()` powers up the MM6108's RF section at that moment; if the 3.3 V rail collapses the ESP32 cold-boots. This is a power or hardware problem, not firmware: lowering `max_tx_power` doesn't help because it happens before any transmission. Check the hat is fully seated with no bent pins, the antenna is firmly attached, and try a different USB cable, port or a powered hub. In testing, an electrolytic capacitor across the 5 V rail and GND stopped the resets (mind the polarity: stripe to GND).

**It connects but Home Assistant can't reach it** — the AP is probably routing/NATing the HaLow network instead of bridging it. Follow [Access Point Setup](#access-point-setup-required), including disabling masquerade. You can check from another machine on your LAN with `ping <device-ip>`.

**"Connect timeout" with no `STA state: CONNECTING`** — the device never found the AP. Check `country_code` matches the AP's region: the channel plans don't overlap in the way you might expect (for example, a `US` device won't find an `AU` AP).

**The device reboots every 15 minutes** — that's ESPHome's API `reboot_timeout`: with no Home Assistant client connected for 15 minutes it restarts. It means HA can't reach the device (see the previous item). While you sort out the network, set `api: reboot_timeout: 0s` to stop it.

**Changes to this repo don't show up in the Home Assistant ESPHome Builder** — external components are cached for a day. Add `refresh: 1min` (or `always`) under the `external_components` source.

**"halow: could not find is_connected() in ESPHome's network component"** — your ESPHome version restructured the network component and this component needs updating; it refuses to build rather than produce firmware that silently drops API connections.

## Known Limitations

- **Power save disabled** — The MM6108 BUSY pin has a firmware bug (confirmed by Morse Micro). Power save is disabled to ensure reliable bidirectional communication. The radio therefore draws full receive current all the time. Monitor [firmware releases](https://github.com/MorseMicro/firmware_binaries/releases/) for a fix.
- **TWT not yet implemented** — Requires working BUSY pin for battery operation. A GPIO-based workaround exists but is not integrated.
- **Station mode, MM6108 only** — AP mode and the MM8108 are not implemented; the config rejects them.
- **Tested on ESP32-S3 only** — the SDK also supports ESP32-C3, C6 and P4, but this component hasn't been run on them.

## How it works

```
                    SPI (40 MHz)
ESP32-S3 ◄────────────────────────► MM6108 (HaLow SoC)
  ├─ ESPHome                          ├─ 802.11ah MAC/PHY
  ├─ lwIP (via Morse mmipal)          ├─ WPA3-SAE
  ├─ Home Assistant API               ├─ Sub-1 GHz radio
  └─ OTA updates                      └─ Firmware loaded over SPI at every boot
```

At boot the component resets the MM6108, loads its firmware and board configuration (BCF) over SPI, brings up an lwIP interface, and connects as a station. A FreeRTOS timer handles reconnection. At build time it splices HaLow into ESPHome's network status checks so the API, MQTT and OTA treat it as a network interface. Full details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Documentation

| Document | For |
|----------|-----|
| [PATCHES.md](PATCHES.md) | Every change this fork makes, why, and how it was verified |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How the component works: build steps, tasks, state machine, reconnects, mDNS, sensors |
| [docs/HARDWARE.md](docs/HARDWARE.md) | Pins, power rails, firmware/BCF, failure signatures, reset reasons |
| [docs/ESPHOME_COMPAT.md](docs/ESPHOME_COMPAT.md) | ESPHome internals the component depends on, and what to check when ESPHome updates |
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | Building, verifying, flashing, logs, branches, test history |
| [docs/HALOW_NOTES.md](docs/HALOW_NOTES.md) | The original author's engineering notebook (partly historical) |

## Dependencies

The [Morse Micro MM-IoT-SDK](https://github.com/MorseMicro/mm-iot-esp32) is downloaded on first compile, pinned to commit `96d9bf4` (morselib 2.10.4, MM6108 firmware 1.17.6). It provides:

- **morselib**: prebuilt WLAN driver and supplicant
- **mm_shims**: ESP-IDF hardware layer (SPI, GPIO, FreeRTOS)
- **mmipal**: lwIP network interface
- **mmutils**, **mmpktmem**, **mmregdb**: utilities, packet memory, regulatory database
- **morsefirmware**: MM6108 firmware and board configuration files

## References

- [Morse Micro MM-IoT-SDK](https://github.com/MorseMicro/mm-iot-esp32)
- [Seeed MM-IoT-SDK fork](https://github.com/Seeed-Studio/mm-iot-esp32) (older, used by Seeed's examples)
- [Morse Micro morse-firmware](https://github.com/MorseMicro/morse-firmware): additional BCF files
- [Seeed Wiki: Getting Started with Wi-Fi HaLow for XIAO](https://wiki.seeedstudio.com/getting_started_with_wifi_halow_module_for_xiao/)
- [IEEE 802.11ah](https://en.wikipedia.org/wiki/IEEE_802.11ah)
- [Xiao-Halow-to-WiFi-Bridge](https://github.com/gtgreenw/Xiao-Halow-to-WiFi-Bridge): the reconnect approach is modelled on this project

## Credits

Original component by [sweitzja](https://github.com/sweitzja/esphome-halow). ESPHome 2026.x support and review fixes in this fork.

## License

Apache-2.0. Morse Micro binary libraries are subject to the [Morse Micro BDL](https://github.com/Seeed-Studio/mm-iot-esp32/blob/main/LICENSES/LicenseRef-MorseMicroBDL.txt).
