# ESPHome compatibility

The component reaches into ESPHome and ESP-IDF internals in a few places. When ESPHome releases a new version, these are what can break. Tested version: **ESPHome 2026.9.1, ESP-IDF 5.5.5, native ESP-IDF toolchain** (the Home Assistant ESPHome Builder default).

## Upgrade checklist

1. Run the patch test against the new release: `.venv/bin/python esphome-halow/tools/test_network_patch.py <new-version> 2026.9.1`.
2. Install the new ESPHome in the venv and compile a full-feature config (see [DEVELOPMENT.md](DEVELOPMENT.md#verification-checklist)). Read any warnings from `components/halow/`.
3. Work through each integration point below.
4. Flash a board and check the boot log reaches `HaLow connected`, then that Home Assistant stays connected for more than 15 minutes (proves the network patch at runtime).
5. Update the tested version here, in the README and in CLAUDE.md.

## Integration points

### 1. `network::is_connected()` / `get_ip_addresses()` (critical)

**What:** ESPHome's API server disconnects every client when `network::is_connected()` is false, then reboots after `reboot_timeout`. MQTT, `http_request`, the `status` binary sensor, `udp`, `wireguard` and others use the same check. It only knows ESPHome's own interfaces.

**History:** up to 2025.x it was a normal function in `network/util.cpp` and the original component overrode it with `-Wl,--wrap`. In 2026.4 it became `ESPHOME_ALWAYS_INLINE` in `util.h`, and `get_use_address()` was replaced by `get_use_address_to()`. The wraps silently stopped working.

**Now:** `_patch_network_util()` reads the installed ESPHome's `network/util.h` and `util.cpp`, inserts a `USE_HALOW` branch at the top of `is_connected()` and `get_ip_addresses()` plus a declaration before the namespace, and registers the copies with `esp32.add_extra_build_file()`. ESP32's `copy_files()` runs after `writer.copy_src_tree()`, so they overwrite the originals in `.esphome/build/<name>/src/`. Matching is by regex: `\bbool is_connected\(\) \{` and `\bIPAddresses get_ip_addresses\(\) \{`, in either file. No match raises `EsphomeError`.

**Check:** `tools/test_network_patch.py`; after a build, `grep -n USE_HALOW .esphome/build/<name>/src/esphome/components/network/util.h`; and that `nm -C` on `api_server.cpp.obj` shows `U esphome::halow::network_is_connected()`.

**If it breaks:** look for a new interface hook in ESPHome (an official way to register a network provider would be better than patching). `get_use_address_to()` already returns `<name>.local`, so it needs no hook.

### 2. lwIP initialisation (`--wrap=tcpip_init`)

**What:** ESPHome's network component calls `esp_netif_init()` (which calls `tcpip_init()`) before HaLow's `setup()`. `mmipal_init()` calls `tcpip_init()` again; a second `lwip_init()` aborts in `esp_vfs_lwip_sockets_register` with `ESP_ERR_INVALID_ARG`.

**Check:** if ESPHome stops calling `esp_netif_init()`, or calls it after HaLow, the wrapper still works (first call goes through). If the boot log shows that abort again, the wrapper isn't linked: it must live in a translation unit that is always linked (`halow_component.cpp`).

### 3. sdkconfig defaults

ESPHome 2026.x writes a trimmed sdkconfig. Options the Morse SDK needs that ESPHome may turn off: `CONFIG_MBEDTLS_CMAC_C` (link error `undefined reference to mbedtls_cipher_cmac_*`), `CONFIG_MBEDTLS_NIST_KW_C`. After an upgrade, check `.esphome/build/<name>/sdkconfig.<name>` still has every option `to_code()` sets.

### 4. Internal ESP-IDF header

`esp_netif_lwip_internal.h` (for the mDNS netif wrapper) is included as `"../lwip/esp_netif_lwip_internal.h"`, relative to esp_netif's public include dir. It only includes public headers. If ESP-IDF moves or changes `struct esp_netif_obj`, mDNS registration breaks. The fields used are `lwip_netif`, `flags`, `if_key`, `if_desc` and `route_prio`.

### 5. mDNS library

Relies on `mdns_init()` returning `ESP_OK` without re-initialising when the responder is already running (espressif/mdns 1.12), and on `mdns_service_exists()`.

### 6. ESPHome Python APIs used

`esp32.add_idf_component`, `add_idf_sdkconfig_option`, `add_extra_build_file`, `get_esp32_variant`, `VARIANT_ESP32S3`, `CORE.relative_build_path`, `CORE.using_toolchain_platformio` (read with `getattr`, default `True`), `cg.add_platformio_option`, `sensor.sensor_schema`, `text_sensor.text_sensor_schema`, `button.button_schema`.

### 7. ESPHome C++ APIs used

`App.get_name()`, `App.get_friendly_name()` (returns `StringRef` in 2026.x; only `.c_str()` is used), `network::IPAddress`/`IPAddresses`, `setup_priority::WIFI`, `ESPHOME_VERSION`, `ESPHOME_BOARD`. `get_mac_address()` moved to `alloc_helpers.h` in 2026.x and is due to change again, so the MAC TXT record uses `esp_efuse_mac_get_default()` instead.

### 8. Device address for logs and OTA (`CORE.address`)

**What:** the CLI and Device Builder pick the host for wireless logs and OTA from `CORE.address`, a property that only reads `use_address` from the `wifi`, `ethernet` and `openthread` sections. For a halow-only device it was `None`, so wireless logs/OTA from the builder failed with "could not be resolved", even on the same network.

**Now:** `halow:` has a `use_address` option (defaulting like wifi's: the static IP, else `<name>.local`), and `_install_address_hook()` wraps the `CORE.address` property at module import so it falls back to `halow.use_address`. If `address` stops being a property, the hook does nothing (logged at debug level).

**Validated-config cache:** for `logs` and `upload`, ESPHome 2026.x loads `.esphome/storage/<file>.validated.json` and skips validation, so this module (and the hook) never loads. `_disable_validated_config_cache()` therefore replaces `compiled_config.save_compiled_config` (and the reference `writer.py` imported) with a version that deletes the cache file for halow configs instead of writing it. If ESPHome renames these functions the patch does nothing and the "second run fails" bug returns, so re-test after upgrades.

**Check:** with `use_address: <ip>`, run `esphome logs test.yaml --device OTA` **twice** (and once after a `compile`). Every run must print `Starting log output from <ip>` and none may print "Loaded validated config cache"; no `.validated.json` should exist for the config.

**Upstream fix:** let `CORE.address` fall back to the storage sidecar's `address` or to any component's `use_address`. Then both hooks can be removed.

### 9. PlatformIO toolchain

Deprecated for ESP32, and due to be removed in ESPHome 2027.2.0. `pre_build.py` only matters there. Once it's gone, delete `pre_build.py` and its registration.

## Toolchain notes

- The native ESP-IDF toolchain doesn't use `~/.platformio`. Anything that assumes that path is PlatformIO-only.
- In Home Assistant OS, ESP-IDF lives under `/data/cache/idf/` and the MM-IoT-SDK under `/root/.esphome/mm-iot-esp32`. `/root` can be reset by add-on updates, which is why the SDK download is pinned.
- External components from GitHub are cached for a day unless `refresh:` is set.
