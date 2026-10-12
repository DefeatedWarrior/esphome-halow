"""ESPHome component for Wi-Fi HaLow (IEEE 802.11ah) via Morse Micro MM6108."""

import logging
import os
import re
import subprocess
from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_PASSWORD,
    CONF_SSID,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_DECIBEL_MILLIWATT,
)
from esphome.core import coroutine_with_priority, CORE, EsphomeError
from esphome.components.esp32 import (
    add_extra_build_file,
    add_idf_component,
    add_idf_sdkconfig_option,
)

_LOGGER = logging.getLogger(__name__)

DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["network", "sensor", "text_sensor", "button"]
CONFLICTS_WITH = ["wifi"]

CONF_COUNTRY_CODE = "country_code"
CONF_SECURITY = "security"
CONF_BCF_FILE = "bcf_file"
CONF_MM_IOT_SDK_PATH = "mm_iot_sdk_path"
CONF_MANUAL_IP = "manual_ip"
CONF_STATIC_IP = "static_ip"
CONF_GATEWAY = "gateway"
CONF_SUBNET = "subnet"
CONF_DNS1 = "dns1"
CONF_DNS2 = "dns2"
CONF_CLK_PIN = "clk_pin"
CONF_MOSI_PIN = "mosi_pin"
CONF_MISO_PIN = "miso_pin"
CONF_CS_PIN = "cs_pin"
CONF_IRQ_PIN = "irq_pin"
CONF_RESET_PIN = "reset_pin"
CONF_WAKE_PIN = "wake_pin"
CONF_BUSY_PIN = "busy_pin"

# Sensor config keys
CONF_RSSI_SENSOR = "rssi"
CONF_TX_PPS = "tx_pps"
CONF_RX_PPS = "rx_pps"
CONF_TX_KBPS = "tx_kbps"
CONF_RX_KBPS = "rx_kbps"
CONF_MCS = "mcs"
CONF_BANDWIDTH = "bandwidth"
CONF_TX_SUCCESS_RATE = "tx_success_rate"
CONF_LINK_QUALITY = "link_quality"
CONF_NOISE_FLOOR = "noise_floor"
CONF_TX_FRAMES_DROPPED = "tx_frames_dropped"
CONF_RX_FRAMES_DROPPED = "rx_frames_dropped"
CONF_CCMP_FAILURES = "ccmp_failures"
CONF_HW_RESTARTS = "hw_restarts"
CONF_SCAN_RESULTS = "scan_results"
CONF_SCAN_BUTTON = "scan_channel"
CONF_SUB_BANDS = "sub_bands"
CONF_IP_ADDRESS_SENSOR = "ip_address"
CONF_GATEWAY_SENSOR = "gateway_address"
CONF_SUBNET_SENSOR = "subnet_mask"
CONF_SSID_SENSOR = "connected_ssid"
CONF_BSSID_SENSOR = "bssid"
CONF_MAC_ADDRESS_SENSOR = "mac_address"
CONF_FW_VERSION_SENSOR = "firmware_version"
CONF_TYPE = "type"
CONF_MODE = "mode"

CONF_MAX_TX_POWER = "max_tx_power"

# Morse Micro MM-IoT-SDK, pinned to the commit this component is tested against
# (morselib 2.10.4-esp32, MM6108 firmware 1.17.6). Bump deliberately.
MM_IOT_SDK_REPO = "https://github.com/MorseMicro/mm-iot-esp32.git"
MM_IOT_SDK_COMMIT = "96d9bf43616347e39789081e877ba5224f6bff8d"
DEFAULT_SDK_PATH = "~/.esphome/mm-iot-esp32"

# Country codes with S1G channel lists in the pinned SDK's regulatory database
# (framework/src/mmregdb/mmregdb.c)
COUNTRY_CODES = ["AU", "CA", "EU", "GB", "IN", "JP", "KR", "NZ", "US"]

ESP32S3_MAX_GPIO = 48


def _validate_chip_type(value):
    value = cv.string_strict(value).upper()
    if value == "MM8108":
        raise cv.Invalid(
            "MM8108 is not supported yet: the component only loads MM6108 firmware "
            "and board configuration"
        )
    if value != "MM6108":
        raise cv.Invalid("type must be MM6108")
    return value


def _validate_mode(value):
    value = cv.string_strict(value).upper()
    if value != "STATION":
        raise cv.Invalid("Only STATION mode is supported (AP mode is not implemented)")
    return value


def _validate_country_code(value):
    value = cv.string_strict(value).upper()
    if value not in COUNTRY_CODES:
        raise cv.Invalid(
            f"Unsupported country code '{value}'. HaLow channel plans are region "
            f"specific; supported: {', '.join(COUNTRY_CODES)}"
        )
    return value


def _validate_config(config):
    if config[CONF_SECURITY] == "SAE" and not config[CONF_PASSWORD]:
        raise cv.Invalid("A password is required for SAE (WPA3) security", [CONF_PASSWORD])
    seen = {}
    for key in (
        CONF_CLK_PIN,
        CONF_MOSI_PIN,
        CONF_MISO_PIN,
        CONF_CS_PIN,
        CONF_IRQ_PIN,
        CONF_RESET_PIN,
        CONF_WAKE_PIN,
        CONF_BUSY_PIN,
    ):
        pin = config[key]
        if pin in seen:
            raise cv.Invalid(f"GPIO{pin} is used for both {seen[pin]} and {key}", [key])
        seen[pin] = key
    return config

halow_ns = cg.esphome_ns.namespace("halow")
HalowComponent = halow_ns.class_("HalowComponent", cg.Component)
HalowScanButton = halow_ns.class_(
    "HalowScanButton", cg.esphome_ns.namespace("button").class_("Button")
)

MANUAL_IP_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_STATIC_IP): cv.ipv4address,
        cv.Required(CONF_GATEWAY): cv.ipv4address,
        cv.Required(CONF_SUBNET): cv.ipv4address,
        cv.Optional(CONF_DNS1, default="0.0.0.0"): cv.ipv4address,
        cv.Optional(CONF_DNS2, default="0.0.0.0"): cv.ipv4address,
    }
)

SECURITY_TYPES = {"SAE": "SAE", "OWE": "OWE", "OPEN": "OPEN"}


def _sensor_schema(**kwargs):
    """Import sensor schema lazily to avoid circular deps."""
    import esphome.components.sensor as sensor_ns

    return sensor_ns.sensor_schema(**kwargs)


def _text_sensor_schema(**kwargs):
    """Import text_sensor schema lazily to avoid circular deps."""
    import esphome.components.text_sensor as ts_ns

    return ts_ns.text_sensor_schema(**kwargs)


def _button_schema(class_, **kwargs):
    """Import button schema lazily to avoid circular deps."""
    import esphome.components.button as button_ns

    return button_ns.button_schema(class_, **kwargs)


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HalowComponent),
            cv.Optional(CONF_TYPE, default="MM6108"): _validate_chip_type,
            cv.Optional(CONF_MODE, default="STATION"): _validate_mode,
            cv.Required(CONF_SSID): cv.All(cv.string, cv.Length(min=1, max=32)),
            cv.Optional(CONF_PASSWORD, default=""): cv.All(
                cv.string, cv.Length(max=100)
            ),
            # Required: HaLow channel plans differ per region and the radio must use
            # the one the AP (and local law) uses
            cv.Required(CONF_COUNTRY_CODE): _validate_country_code,
            cv.Optional(CONF_SECURITY, default="SAE"): cv.enum(
                SECURITY_TYPES, upper=True
            ),
            cv.Optional(
                CONF_BCF_FILE, default="bcf_mf16858.mbin"
            ): cv.string_strict,
            cv.Optional(CONF_MM_IOT_SDK_PATH, default=DEFAULT_SDK_PATH): cv.string,
            cv.Optional(CONF_SUB_BANDS, default=False): cv.boolean,
            # Cap TX power below the regulatory maximum (dBm)
            cv.Optional(CONF_MAX_TX_POWER): cv.int_range(min=1, max=30),
            cv.Optional(CONF_MANUAL_IP): MANUAL_IP_SCHEMA,
            # SPI pins — defaults match XIAO HaLow Hat
            cv.Optional(CONF_CLK_PIN, default=7): cv.int_range(
                min=0, max=ESP32S3_MAX_GPIO
            ),
            cv.Optional(CONF_MOSI_PIN, default=9): cv.int_range(
                min=0, max=ESP32S3_MAX_GPIO
            ),
            cv.Optional(CONF_MISO_PIN, default=8): cv.int_range(
                min=0, max=ESP32S3_MAX_GPIO
            ),
            cv.Optional(CONF_CS_PIN, default=4): cv.int_range(
                min=0, max=ESP32S3_MAX_GPIO
            ),
            cv.Optional(CONF_IRQ_PIN, default=3): cv.int_range(
                min=0, max=ESP32S3_MAX_GPIO
            ),
            cv.Optional(CONF_RESET_PIN, default=1): cv.int_range(
                min=0, max=ESP32S3_MAX_GPIO
            ),
            cv.Optional(CONF_WAKE_PIN, default=2): cv.int_range(
                min=0, max=ESP32S3_MAX_GPIO
            ),
            cv.Optional(CONF_BUSY_PIN, default=5): cv.int_range(
                min=0, max=ESP32S3_MAX_GPIO
            ),
            # Numeric sensors
            cv.Optional(CONF_RSSI_SENSOR): _sensor_schema(
                unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_TX_PPS): _sensor_schema(
                unit_of_measurement="pps",
                accuracy_decimals=1,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:upload-network",
            ),
            cv.Optional(CONF_RX_PPS): _sensor_schema(
                unit_of_measurement="pps",
                accuracy_decimals=1,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:download-network",
            ),
            cv.Optional(CONF_TX_KBPS): _sensor_schema(
                unit_of_measurement="kbps",
                accuracy_decimals=1,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:upload-network",
            ),
            cv.Optional(CONF_RX_KBPS): _sensor_schema(
                unit_of_measurement="kbps",
                accuracy_decimals=1,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:download-network",
            ),
            cv.Optional(CONF_MCS): _sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:signal-variant",
            ),
            cv.Optional(CONF_BANDWIDTH): _sensor_schema(
                unit_of_measurement="MHz",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:signal-variant",
            ),
            cv.Optional(CONF_TX_SUCCESS_RATE): _sensor_schema(
                unit_of_measurement="%",
                accuracy_decimals=1,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:check-network",
            ),
            # Text sensors
            cv.Optional(CONF_IP_ADDRESS_SENSOR): _text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:ip-network",
            ),
            cv.Optional(CONF_GATEWAY_SENSOR): _text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:router-wireless",
            ),
            cv.Optional(CONF_SUBNET_SENSOR): _text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_SSID_SENSOR): _text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:wifi",
            ),
            cv.Optional(CONF_BSSID_SENSOR): _text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_MAC_ADDRESS_SENSOR): _text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_FW_VERSION_SENSOR): _text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:chip",
            ),
            cv.Optional(CONF_LINK_QUALITY): _text_sensor_schema(
                icon="mdi:wifi-strength-outline",
            ),
            cv.Optional(CONF_SCAN_RESULTS): _text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:radar",
            ),
            # UMAC diagnostic sensors
            cv.Optional(CONF_NOISE_FLOOR): _sensor_schema(
                unit_of_measurement="dBm",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:sine-wave",
            ),
            cv.Optional(CONF_TX_FRAMES_DROPPED): _sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:alert-circle-outline",
            ),
            cv.Optional(CONF_RX_FRAMES_DROPPED): _sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:alert-circle-outline",
            ),
            cv.Optional(CONF_CCMP_FAILURES): _sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:shield-alert-outline",
            ),
            cv.Optional(CONF_HW_RESTARTS): _sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:restart-alert",
            ),
            # Channel scan button
            cv.Optional(CONF_SCAN_BUTTON): _button_schema(
                HalowScanButton,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:radar",
            ),
        }
    ),
    cv.only_on_esp32,
    _validate_config,
)


@coroutine_with_priority(60.0)  # Same priority as WiFi/Ethernet
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Core configuration
    cg.add(var.set_ssid(config[CONF_SSID]))
    cg.add(var.set_password(config[CONF_PASSWORD]))
    cg.add(var.set_country_code(config[CONF_COUNTRY_CODE]))
    cg.add(var.set_security_type(config[CONF_SECURITY]))
    cg.add(var.set_sub_bands_enabled(config[CONF_SUB_BANDS]))
    if CONF_MAX_TX_POWER in config:
        cg.add(var.set_max_tx_power(config[CONF_MAX_TX_POWER]))

    # Pin configuration
    cg.add(var.set_spi_clk_pin(config[CONF_CLK_PIN]))
    cg.add(var.set_spi_mosi_pin(config[CONF_MOSI_PIN]))
    cg.add(var.set_spi_miso_pin(config[CONF_MISO_PIN]))
    cg.add(var.set_spi_cs_pin(config[CONF_CS_PIN]))
    cg.add(var.set_spi_irq_pin(config[CONF_IRQ_PIN]))
    cg.add(var.set_reset_pin(config[CONF_RESET_PIN]))
    cg.add(var.set_wake_pin(config[CONF_WAKE_PIN]))
    cg.add(var.set_busy_pin(config[CONF_BUSY_PIN]))

    # Static IP
    if manual_ip := config.get(CONF_MANUAL_IP):
        cg.add(
            var.set_manual_ip(
                str(manual_ip[CONF_STATIC_IP]),
                str(manual_ip[CONF_GATEWAY]),
                str(manual_ip[CONF_SUBNET]),
                str(manual_ip.get(CONF_DNS1, "0.0.0.0")),
                str(manual_ip.get(CONF_DNS2, "0.0.0.0")),
            )
        )

    # Sensors
    import esphome.components.sensor as sensor_ns
    import esphome.components.text_sensor as ts_ns

    for conf_key, setter in [
        (CONF_RSSI_SENSOR, var.set_rssi_sensor),
        (CONF_TX_PPS, var.set_tx_pps_sensor),
        (CONF_RX_PPS, var.set_rx_pps_sensor),
        (CONF_TX_KBPS, var.set_tx_kbps_sensor),
        (CONF_RX_KBPS, var.set_rx_kbps_sensor),
        (CONF_MCS, var.set_mcs_sensor),
        (CONF_BANDWIDTH, var.set_bandwidth_sensor),
        (CONF_TX_SUCCESS_RATE, var.set_tx_success_rate_sensor),
        (CONF_NOISE_FLOOR, var.set_noise_floor_sensor),
        (CONF_TX_FRAMES_DROPPED, var.set_tx_frames_dropped_sensor),
        (CONF_RX_FRAMES_DROPPED, var.set_rx_frames_dropped_sensor),
        (CONF_CCMP_FAILURES, var.set_ccmp_failures_sensor),
        (CONF_HW_RESTARTS, var.set_hw_restarts_sensor),
    ]:
        if conf_key in config:
            sens = await sensor_ns.new_sensor(config[conf_key])
            cg.add(setter(sens))

    for conf_key, setter in [
        (CONF_IP_ADDRESS_SENSOR, var.set_ip_address_sensor),
        (CONF_GATEWAY_SENSOR, var.set_gateway_sensor),
        (CONF_SUBNET_SENSOR, var.set_subnet_sensor),
        (CONF_SSID_SENSOR, var.set_ssid_sensor),
        (CONF_BSSID_SENSOR, var.set_bssid_sensor),
        (CONF_MAC_ADDRESS_SENSOR, var.set_mac_address_sensor),
        (CONF_FW_VERSION_SENSOR, var.set_fw_version_sensor),
        (CONF_LINK_QUALITY, var.set_link_quality_sensor),
        (CONF_SCAN_RESULTS, var.set_scan_results_sensor),
    ]:
        if conf_key in config:
            sens = await ts_ns.new_text_sensor(config[conf_key])
            cg.add(setter(sens))

    # Scan button
    if CONF_SCAN_BUTTON in config:
        import esphome.components.button as button_ns

        btn = await button_ns.new_button(config[CONF_SCAN_BUTTON])
        cg.add(btn.set_parent(var))

    cg.add_define("USE_HALOW")

    # --- MM-IoT-SDK Integration ---

    sdk_path = os.path.abspath(os.path.expanduser(config[CONF_MM_IOT_SDK_PATH]))
    framework_path = _ensure_sdk(sdk_path)

    # Register IDF components
    components = [
        ("morselib", "morselib"),
        ("mm_shims", "mm_shims"),
        ("mmipal", "src/mmipal"),
        ("mmutils", "src/mmutils"),
        ("mmpktmem", "src/mmpktmem"),
    ]
    # mmregdb exists in upstream SDK but not in Seeed fork
    mmregdb_path = os.path.join(framework_path, "src", "mmregdb")
    if os.path.isdir(mmregdb_path):
        components.append(("mmregdb", "src/mmregdb"))
        cg.add_define("USE_HALOW_REGDB")
    for name, subpath in components:
        add_idf_component(name=name, path=os.path.join(framework_path, subpath))

    # Kconfig: Pins
    add_idf_sdkconfig_option("CONFIG_MM_RESET_N", config[CONF_RESET_PIN])
    add_idf_sdkconfig_option("CONFIG_MM_WAKE", config[CONF_WAKE_PIN])
    add_idf_sdkconfig_option("CONFIG_MM_BUSY", config[CONF_BUSY_PIN])
    add_idf_sdkconfig_option("CONFIG_MM_SPI_SCK", config[CONF_CLK_PIN])
    add_idf_sdkconfig_option("CONFIG_MM_SPI_MOSI", config[CONF_MOSI_PIN])
    add_idf_sdkconfig_option("CONFIG_MM_SPI_MISO", config[CONF_MISO_PIN])
    add_idf_sdkconfig_option("CONFIG_MM_SPI_CS", config[CONF_CS_PIN])
    add_idf_sdkconfig_option("CONFIG_MM_SPI_IRQ", config[CONF_IRQ_PIN])

    # Kconfig: Firmware files — differs between upstream and Seeed SDK
    kconfig_path = os.path.join(framework_path, "mm_shims", "Kconfig")
    with open(kconfig_path) as f:
        kconfig_content = f.read()
    if "MM_BCF_FILE" in kconfig_content:
        # Upstream SDK: string-based BCF/FW config
        bcf_dir = os.path.join(framework_path, "morsefirmware", "mm6108", "bcfs")
        if not os.path.isfile(os.path.join(bcf_dir, config[CONF_BCF_FILE])):
            available = sorted(os.listdir(bcf_dir)) if os.path.isdir(bcf_dir) else []
            raise EsphomeError(
                f"BCF file '{config[CONF_BCF_FILE]}' not found in the MM-IoT-SDK. "
                f"Available: {', '.join(available)}"
            )
        add_idf_sdkconfig_option("CONFIG_MM_BCF_FILE", config[CONF_BCF_FILE])
        add_idf_sdkconfig_option("CONFIG_MM_FW_FILE", "mm6108.mbin")
    else:
        # Seeed SDK: choice-based BCF config
        bcf = config[CONF_BCF_FILE]
        if "mf16858_us" in bcf:
            add_idf_sdkconfig_option("CONFIG_MM_BCF_MF16858_US", True)
        elif "mf08651_us" in bcf:
            add_idf_sdkconfig_option("CONFIG_MM_BCF_MF08651_US", True)
        elif "mf08551" in bcf:
            add_idf_sdkconfig_option("CONFIG_MM_BCF_MF08551", True)
        elif "mf08251" in bcf:
            add_idf_sdkconfig_option("CONFIG_MM_BCF_MF08251", True)
    # Chip type (only MM6108 passes validation)
    add_idf_sdkconfig_option("CONFIG_MMHAL_CHIP_TYPE_MM6108", True)

    # Kconfig: FreeRTOS
    add_idf_sdkconfig_option("CONFIG_FREERTOS_HZ", 1000)
    add_idf_sdkconfig_option("CONFIG_FREERTOS_TIMER_TASK_PRIORITY", 10)
    # Reconnects (mmwlan_sta_disable/enable + logging) run in the timer task;
    # the IDF default stack is too small for that
    add_idf_sdkconfig_option("CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH", 4096)

    # Kconfig: ESP32-S3
    add_idf_sdkconfig_option("CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_NIST_KW_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CMAC_C", True)  # OMAC1 for WPA3/PMF; ESPHome disables it by default

    # Kconfig: LWIP settings for mmipal
    add_idf_sdkconfig_option("CONFIG_LWIP_NETIF_STATUS_CALLBACK", True)
    add_idf_sdkconfig_option("CONFIG_LWIP_STATS", True)  # Needed for TX/RX packet counters
    cg.add_build_flag("-DLWIP_NETIF_LINK_CALLBACK=1")

    # Disable mDNS predefined netif search — we register our own netif manually.
    # Without this, mdns_init() tries to register WiFi/ETH event handlers which
    # fail because no standard WiFi or Ethernet driver is active.
    add_idf_sdkconfig_option("CONFIG_MDNS_PREDEF_NETIF_STA", False)
    add_idf_sdkconfig_option("CONFIG_MDNS_PREDEF_NETIF_AP", False)
    add_idf_sdkconfig_option("CONFIG_MDNS_PREDEF_NETIF_ETH", False)

    # Make ESPHome's network::is_connected() / get_ip_addresses() aware of HaLow,
    # so the API, MQTT, OTA etc. see the network as up
    _patch_network_util()

    # Wrap tcpip_input for RX byte counting — the SDK calls tcpip_input()
    # directly instead of going through netif->input
    cg.add_build_flag("-Wl,--wrap=tcpip_input")

    # Wrap tcpip_init so mmipal_init() doesn't re-initialise the lwIP stack
    # that ESPHome's network component already started
    cg.add_build_flag("-Wl,--wrap=tcpip_init")

    # Firmware blob generation for the (deprecated) PlatformIO toolchain; the
    # native ESP-IDF toolchain embeds the blobs via the SDK's CMakeLists
    if getattr(CORE, "using_toolchain_platformio", True):
        cg.add_platformio_option(
            "extra_scripts",
            [os.path.join(os.path.dirname(__file__), "pre_build.py")],
        )


def _git(*args, cwd=None):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)


def _ensure_sdk(sdk_path):
    """Download the pinned MM-IoT-SDK if needed and return its framework path."""
    framework_path = os.path.join(sdk_path, "framework")
    if not os.path.isdir(framework_path):
        _LOGGER.info(
            "Downloading MM-IoT-SDK %s to %s ...", MM_IOT_SDK_COMMIT[:7], sdk_path
        )
        os.makedirs(sdk_path, exist_ok=True)
        steps = [
            ("init", "-q"),
            ("remote", "add", "origin", MM_IOT_SDK_REPO),
            ("fetch", "-q", "--depth", "1", "origin", MM_IOT_SDK_COMMIT),
            ("checkout", "-q", "FETCH_HEAD"),
        ]
        for step in steps:
            result = _git(*step, cwd=sdk_path)
            if result.returncode != 0:
                raise EsphomeError(
                    f"Failed to download MM-IoT-SDK (git {step[0]}): "
                    f"{result.stderr.strip()}\nManually run: git clone "
                    f"{MM_IOT_SDK_REPO} {sdk_path} && git -C {sdk_path} checkout "
                    f"{MM_IOT_SDK_COMMIT}"
                )
        _LOGGER.info("MM-IoT-SDK downloaded successfully")
        return framework_path

    head = _git("rev-parse", "HEAD", cwd=sdk_path)
    if head.returncode == 0 and head.stdout.strip() != MM_IOT_SDK_COMMIT:
        _LOGGER.warning(
            "MM-IoT-SDK at %s is at %s, but this component is tested with %s. "
            "Delete that folder to re-download the tested version.",
            sdk_path,
            head.stdout.strip()[:7],
            MM_IOT_SDK_COMMIT[:7],
        )
    return framework_path


_HALOW_DECL = (
    "\n#ifdef USE_HALOW\n"
    "namespace esphome {\nnamespace halow {\n"
    "bool network_is_connected();\n"
    "network::IPAddresses network_get_ip_addresses();\n"
    "}  // namespace halow\n}  // namespace esphome\n"
    "#endif  // USE_HALOW\n\n"
)
_IS_CONNECTED_HOOK = (
    "\n#ifdef USE_HALOW\n"
    "  if (halow::network_is_connected())\n    return true;\n"
    "#endif  // USE_HALOW\n"
)
_GET_IPS_HOOK = (
    "\n#ifdef USE_HALOW\n"
    "  if (halow::network_is_connected())\n"
    "    return halow::network_get_ip_addresses();\n"
    "#endif  // USE_HALOW\n"
)
_NAMESPACE_RE = re.compile(r"^namespace esphome(::network)? \{", re.MULTILINE)


def _patch_network_util():
    """Splice HaLow into ESPHome's network::is_connected() and get_ip_addresses().

    ESPHome only knows its built-in interfaces (wifi, ethernet, modem, ...) and,
    since 2026.x, inlines is_connected() into every caller, so it can't be
    overridden at link time. Instead, copies of network/util.h and util.cpp with
    a USE_HALOW branch are written into the build tree after ESPHome copies its
    own sources. If ESPHome restructures these files the build fails here, loudly,
    rather than producing firmware that drops every API connection.
    """
    import esphome.components.network as network_component

    src_dir = Path(network_component.__file__).parent
    patched = {}
    for name in ("util.h", "util.cpp"):
        path = src_dir / name
        if path.is_file():
            patched[name] = path.read_text(encoding="utf-8")

    def add_hook(pattern, hook, what):
        for name, text in patched.items():
            match = re.search(pattern, text)
            if match is None:
                continue
            ns = _NAMESPACE_RE.search(text)
            if ns is None or ns.start() > match.start():
                break
            text = text[: match.end()] + hook + text[match.end() :]
            if _HALOW_DECL not in text:
                text = text[: ns.start()] + _HALOW_DECL + text[ns.start() :]
            patched[name] = text
            return
        raise EsphomeError(
            f"halow: could not find {what} in ESPHome's network component "
            f"({src_dir}). This ESPHome version is not supported yet."
        )

    add_hook(r"\bbool is_connected\(\) \{", _IS_CONNECTED_HOOK, "is_connected()")
    add_hook(
        r"\bIPAddresses get_ip_addresses\(\) \{",
        _GET_IPS_HOOK,
        "get_ip_addresses()",
    )

    out_dir = Path(CORE.relative_build_path("halow_patches"))
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, text in patched.items():
        out = out_dir / name
        if not out.is_file() or out.read_text(encoding="utf-8") != text:
            out.write_text(text, encoding="utf-8")
        add_extra_build_file(f"src/esphome/components/network/{name}", out)
