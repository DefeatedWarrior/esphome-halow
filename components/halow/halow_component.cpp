/**
 * ESPHome component for Wi-Fi HaLow (IEEE 802.11ah) via Morse Micro MM6108.
 *
 * Wraps the MM-IoT-SDK to provide HaLow network connectivity within ESPHome.
 * The MM6108 communicates over SPI and requires firmware + BCF loaded at boot.
 *
 * State machine: STOPPED -> CONNECTING -> CONNECTED (with auto-reconnect)
 * SDK callbacks fire from FreeRTOS tasks; we use volatile flags polled in loop().
 * After the initial connect in setup(), every mmwlan_sta_disable/enable call is
 * made from the reconnect timer task — loop() only ever schedules that timer.
 */

#include "halow_component.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

#include <algorithm>
#include <vector>

// MM-IoT-SDK headers
extern "C" {
#include "mmhal.h"
#include "mmosal.h"
#include "mmwlan.h"
#include "mmwlan_stats.h"
#include "mmipal.h"
#ifdef USE_HALOW_REGDB
#include "mmregdb.h"
#else
#include "mmwlan_regdb.def"
#endif
}

// ESP-IDF headers for mDNS integration
extern "C" {
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_private/periph_ctrl.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "soc/periph_defs.h"
#include "freertos/semphr.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "mdns.h"
// Internal header for esp_netif_obj struct layout — needed to create
// a minimal wrapper around mmipal's raw LWIP netif for mDNS registration.
// The struct layout has been stable since ESP-IDF v4.x.
// Reached relative to esp_netif's public include dir so it resolves under both
// the PlatformIO and native ESP-IDF toolchains.
#include "../lwip/esp_netif_lwip_internal.h"
}

namespace esphome {
namespace halow {

static const char *const TAG = "halow";

static const uint32_t CONNECT_TIMEOUT_MS = 60000;
static const uint32_t SENSOR_UPDATE_INTERVAL_MS = 10000;

HalowComponent *global_halow_component = nullptr;  // NOLINT

// --- SDK Callbacks and Reconnect Timer ---
// Modeled after Xiao-Halow-to-WiFi-Bridge project which has working reconnection.
// Key insight: reconnect must run from a FreeRTOS timer, NOT from the main loop
// or from within a callback. The 15-second delay lets the SDK clean up internally.

static volatile bool s_link_up = false;
static volatile bool s_link_down_event = false;
static volatile bool s_sta_connected = false;
static volatile bool s_reconnecting = false;
static TimerHandle_t s_reconnect_timer = nullptr;
// millis() of the last mmwlan_sta_enable() call; loop() uses it for the connect timeout
static volatile uint32_t s_last_attempt_ms = 0;

// Forward declarations
static void do_halow_reconnect(TimerHandle_t t);
static void schedule_reconnect();

static void link_state_cb(enum mmwlan_link_state state, void *arg) {
  s_link_up = (state == MMWLAN_LINK_UP);
  if (state == MMWLAN_LINK_UP) {
    ESP_LOGI(TAG, "HaLow link UP");
  } else {
    s_link_down_event = true;
    ESP_LOGW(TAG, "HaLow link DOWN");
  }
}

// mmipal link status callback — fires on link up/down from LWIP layer
static void mmipal_link_status_cb(const struct mmipal_link_status *status) {
  if (status->link_state == MMIPAL_LINK_UP) {
    s_link_up = true;
    s_reconnecting = false;
    ESP_LOGI(TAG, "mmipal link UP (IP: %s)", status->ip_addr);
  } else {
    s_link_up = false;
    s_link_down_event = true;
    ESP_LOGW(TAG, "mmipal link DOWN");
    // Start reconnect timer (15 seconds) — same approach as Xiao-Halow-to-WiFi-Bridge
    if (!s_reconnecting) {
      schedule_reconnect();
    }
  }
}

static void sta_status_cb(enum mmwlan_sta_state state) {
  static const char *const STATES[] = {"DISABLED", "CONNECTING", "CONNECTED"};
  const unsigned idx = (unsigned) state;
  ESP_LOGI(TAG, "STA state: %s", idx < 3 ? STATES[idx] : "UNKNOWN");
  s_sta_connected = (state == MMWLAN_STA_CONNECTED);
}

// Build STA arguments from the component config. Shared by the initial connect
// and the timer-driven reconnect so both honour the configured security type.
// Lengths are clamped to the SDK buffers (the YAML schema also enforces them).
static void fill_sta_args(const HalowComponent *comp, struct mmwlan_sta_args *args) {
  const std::string &security = comp->get_security_type();
  size_t ssid_len = std::min<size_t>(strlen(comp->get_ssid()), MMWLAN_SSID_MAXLEN);
  args->ssid_len = ssid_len;
  memcpy(args->ssid, comp->get_ssid(), ssid_len);

  if (security != "OPEN") {
    size_t pass_len = std::min<size_t>(strlen(comp->get_password()), MMWLAN_PASSPHRASE_MAXLEN);
    args->passphrase_len = pass_len;
    memcpy(args->passphrase, comp->get_password(), pass_len);
    args->passphrase[pass_len] = '\0';
  }

  if (security == "OWE") {
    args->security_type = MMWLAN_OWE;
  } else if (security == "OPEN") {
    args->security_type = MMWLAN_OPEN;
  } else {
    args->security_type = MMWLAN_SAE;
  }
}

// Ask the timer task to (re)connect, unless a reconnect is already queued.
static void schedule_reconnect() {
  if (s_reconnect_timer != nullptr && xTimerIsTimerActive(s_reconnect_timer) == pdFALSE) {
    if (xTimerStart(s_reconnect_timer, 0) == pdPASS) {
      ESP_LOGI(TAG, "Reconnect scheduled in 15s");
    }
  }
}

// Reconnect handler — runs from FreeRTOS timer task, NOT main loop
static void do_halow_reconnect(TimerHandle_t t) {
  (void) t;
  if (s_link_up) {
    // Already reconnected, nothing to do
    return;
  }

  s_reconnecting = true;
  ESP_LOGW(TAG, "=== RECONNECTING (from timer) ===");

  enum mmwlan_status status = mmwlan_sta_disable();
  if (status != MMWLAN_SUCCESS) {
    ESP_LOGW(TAG, "sta_disable failed (%d), will retry", status);
    s_reconnecting = false;
    // Restart timer to retry
    xTimerStart(s_reconnect_timer, 0);
    return;
  }

  // Re-enable with stored config
  struct mmwlan_sta_args sta_args = MMWLAN_STA_ARGS_INIT;
  auto *comp = esphome::halow::global_halow_component;
  if (comp == nullptr) {
    s_reconnecting = false;
    return;
  }
  fill_sta_args(comp, &sta_args);

  // Reset flags
  s_link_up = false;
  s_link_down_event = false;
  s_sta_connected = false;

  ESP_LOGI(TAG, "sta_enable from timer...");
  s_last_attempt_ms = millis();
  status = mmwlan_sta_enable(&sta_args, sta_status_cb);
  if (status != MMWLAN_SUCCESS) {
    ESP_LOGW(TAG, "sta_enable failed (%d), retrying in 15s", status);
    s_reconnecting = false;
    xTimerStart(s_reconnect_timer, 0);
    return;
  }
  // If sta_enable succeeds, the mmipal callback will set s_reconnecting = false
  // when link comes up
}

// --- Channel Scan State ---
// Results arrive on the SDK's task; loop() publishes them once the scan completes.
struct ScanEntry {
  char ssid[MMWLAN_SSID_MAXLEN + 1];
  uint8_t bssid[6];
  int16_t rssi;
  int8_t noise_dbm;
  uint32_t freq_hz;
  uint8_t bw_mhz;
};
static const size_t MAX_SCAN_ENTRIES = 32;
static const size_t MAX_TEXT_SENSOR_LEN = 255;  // Home Assistant state length limit
static std::vector<ScanEntry> s_scan_entries;
static SemaphoreHandle_t s_scan_lock = nullptr;
static volatile bool s_scan_complete = false;
static volatile bool s_scan_running = false;

// --- Byte Counters (for kbps sensors) ---
// TX: hook netif->output (etharp_output wrapper) to count outgoing bytes.
// RX: hook netif->linkoutput is TX-only. The SDK calls tcpip_input() directly for RX,
//     bypassing netif->input. So for RX bytes we estimate from packet counts × avg size,
//     or we wrap the linkoutput for TX and use a LWIP raw PCB for RX.
//     Simplest correct approach: wrap linkoutput for TX, use pcb recv for RX.
static volatile uint32_t s_tx_bytes = 0;
static volatile uint32_t s_rx_bytes = 0;
static netif_linkoutput_fn s_original_linkoutput = nullptr;

static err_t halow_linkoutput_hook(struct netif *netif, struct pbuf *p) {
  s_tx_bytes += p->tot_len;
  return s_original_linkoutput(netif, p);
}

// For RX, we wrap tcpip_input via --wrap linker flag. The SDK calls tcpip_input()
// directly instead of going through netif->input, so hooking netif->input doesn't work.
extern "C" {
  extern err_t __real_tcpip_input(struct pbuf *p, struct netif *inp);
  err_t __wrap_tcpip_input(struct pbuf *p, struct netif *inp) {
    // Only count bytes for the HaLow netif (name "MM")
    if (inp != nullptr && inp->name[0] == 'M' && inp->name[1] == 'M') {
      s_rx_bytes += p->tot_len;
    }
    return __real_tcpip_input(p, inp);
  }
}

// ESPHome's network component calls esp_netif_init(), which starts the lwIP
// tcpip thread. mmipal_init() then calls tcpip_init() again, and lwIP aborts
// re-registering its socket VFS. Let the first call through; for later calls
// just run the init-done callback on the already-running tcpip thread.
extern "C" {
  extern void __real_tcpip_init(tcpip_init_done_fn initfunc, void *arg);
  void __wrap_tcpip_init(tcpip_init_done_fn initfunc, void *arg) {
    static bool s_tcpip_started = false;
    if (!s_tcpip_started) {
      s_tcpip_started = true;
      __real_tcpip_init(initfunc, arg);
      return;
    }
    if (initfunc != nullptr)
      tcpip_callback(initfunc, arg);
  }
}

// --- Component Lifecycle ---

float HalowComponent::get_setup_priority() const {
  return setup_priority::WIFI;
}

void HalowComponent::set_manual_ip(const std::string &ip, const std::string &gw,
                                     const std::string &subnet, const std::string &dns1,
                                     const std::string &dns2) {
  this->use_static_ip_ = true;
  this->static_ip_ = ip;
  this->static_gw_ = gw;
  this->static_subnet_ = subnet;
  this->static_dns1_ = dns1;
  this->static_dns2_ = dns2;
}

// Shutdown handler — called before esp_restart() during OTA.
// Properly deinits the WLAN stack and SPI bus so the next boot starts clean.
static void halow_shutdown_handler() {
  ESP_LOGI(TAG, "Shutdown: cleaning up WLAN and SPI...");
  mmwlan_sta_disable();
  mmwlan_shutdown();
  // The SDK's deinit handles spi_bus_remove_device + spi_bus_free internally
  mmwlan_deinit();
  // Also reset the SPI peripheral hardware registers
  periph_module_reset(PERIPH_SPI2_MODULE);
  // Hold MM6108 in reset so it's clean for next boot
  gpio_set_level((gpio_num_t) CONFIG_MM_RESET_N, 0);
  ESP_LOGI(TAG, "Shutdown: cleanup complete");
}

void HalowComponent::setup() {
  ESP_LOGI(TAG, "Setting up Wi-Fi HaLow (MM6108)...");
  global_halow_component = this;

  // Register shutdown handler for clean OTA reboot.
  // This runs before esp_restart() and properly shuts down the WLAN stack + SPI bus.
  esp_register_shutdown_handler(halow_shutdown_handler);

  // Reset SPI peripheral hardware (handles cold boot and cases where
  // shutdown handler didn't run, e.g., watchdog or power-loss reboot)
  periph_module_reset(PERIPH_SPI2_MODULE);
  spi_bus_free(SPI2_HOST);  // Ignore error — may not be initialized

  // GPIO interrupt config survives a software reset. If the previous boot left the
  // IRQ pin armed as LOW_LEVEL, installing the ISR service in mmhal_init() storms
  // with no handler attached and trips the interrupt watchdog. Disarm both pins.
  for (int pin : {this->spi_irq_pin_, this->busy_pin_}) {
    gpio_intr_disable((gpio_num_t) pin);
    gpio_set_intr_type((gpio_num_t) pin, GPIO_INTR_DISABLE);
  }

  // Hardware-reset the MM6108 to ensure clean chip state.
  gpio_set_direction((gpio_num_t) this->reset_pin_, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t) this->reset_pin_, 0);
  vTaskDelay(pdMS_TO_TICKS(100));
  gpio_set_level((gpio_num_t) this->reset_pin_, 1);
  vTaskDelay(pdMS_TO_TICKS(100));
  ESP_LOGI(TAG, "MM6108 hardware reset complete");

  // Initialize MM-IoT-SDK subsystems
  mmhal_init();
  mmwlan_init();

  // Optional TX power cap. Must be set after mmwlan_init() and before the radio is
  // active; the SDK can only lower power below the regulatory limit, never raise it.
  if (this->max_tx_power_dbm_ > 0) {
    enum mmwlan_status tx_status = mmwlan_override_max_tx_power(this->max_tx_power_dbm_);
    if (tx_status == MMWLAN_SUCCESS) {
      ESP_LOGI(TAG, "Max TX power capped at %u dBm", this->max_tx_power_dbm_);
    } else {
      ESP_LOGW(TAG, "Could not set max TX power (%d)", (int) tx_status);
    }
  }

  // Set regulatory domain
  const struct mmwlan_s1g_channel_list *channel_list =
      mmwlan_lookup_regulatory_domain(get_regulatory_db(), this->country_code_.c_str());
  if (channel_list == nullptr) {
    ESP_LOGE(TAG, "Invalid country code: %s", this->country_code_.c_str());
    this->mark_failed();
    return;
  }
  if (mmwlan_set_channel_list(channel_list) != MMWLAN_SUCCESS) {
    ESP_LOGE(TAG, "Failed to set channel list");
    this->mark_failed();
    return;
  }

  // Register link state callback
  if (mmwlan_register_link_state_cb(link_state_cb, nullptr) != MMWLAN_SUCCESS) {
    ESP_LOGE(TAG, "Failed to register link state callback");
    this->mark_failed();
    return;
  }

  // Boot the MM6108 (loads firmware + BCF over SPI)
  struct mmwlan_boot_args boot_args = MMWLAN_BOOT_ARGS_INIT;
  if (mmwlan_boot(&boot_args) != MMWLAN_SUCCESS) {
    ESP_LOGE(TAG, "MM6108 boot failed");
    this->mark_failed();
    return;
  }

  // Disable power save — keeps radio awake so device responds to ARP/ping reliably.
  // Without this, the radio sleeps between beacons and misses inbound packets.
  mmwlan_set_power_save_mode(MMWLAN_PS_DISABLED);
  // Sub-band support: when enabled, the rate controller can drop to 1-2 MHz
  // sub-bands for better sensitivity at extreme range. When disabled, forces
  // full operating bandwidth (e.g., 8 MHz) for maximum throughput.
  mmwlan_set_subbands_enabled(this->sub_bands_enabled_);
  ESP_LOGI(TAG, "Power save disabled, sub-bands %s",
           this->sub_bands_enabled_ ? "enabled (max range)" : "disabled (max throughput)");

  // Read and log version info
  struct mmwlan_version version;
  if (mmwlan_get_version(&version) == MMWLAN_SUCCESS) {
    ESP_LOGI(TAG, "Morse FW %s, morselib %s, chip 0x%lx",
             version.morse_fw_version, version.morselib_version, version.morse_chip_id);
  }

  // Read and log MAC address
  uint8_t mac[6];
  if (mmwlan_get_mac_addr(mac) == MMWLAN_SUCCESS) {
    ESP_LOGI(TAG, "MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }

  // Initialize IP stack
  struct mmipal_init_args ip_args = MMIPAL_INIT_ARGS_DEFAULT;
  if (this->use_static_ip_) {
    ip_args.mode = MMIPAL_STATIC;
    strncpy(ip_args.ip_addr, this->static_ip_.c_str(), sizeof(ip_args.ip_addr) - 1);
    strncpy(ip_args.netmask, this->static_subnet_.c_str(), sizeof(ip_args.netmask) - 1);
    strncpy(ip_args.gateway_addr, this->static_gw_.c_str(), sizeof(ip_args.gateway_addr) - 1);
    ESP_LOGI(TAG, "Using static IP: %s", this->static_ip_.c_str());
  } else {
    ip_args.mode = MMIPAL_DHCP;
    ESP_LOGI(TAG, "Using DHCP");
  }
  if (mmipal_init(&ip_args) != MMIPAL_SUCCESS) {
    ESP_LOGE(TAG, "IP stack init failed");
    this->mark_failed();
    return;
  }

  // Register mmipal link status callback — this handles reconnection via a FreeRTOS timer.
  // The callback fires on link down, starts a 15-second timer, which then does
  // sta_disable() + sta_enable() from the timer task context (not the main loop).
  mmipal_set_link_status_callback(mmipal_link_status_cb);

  // Create reconnect timer (one-shot, 15 seconds)
  s_reconnect_timer = xTimerCreate("halow_reconn", pdMS_TO_TICKS(15000),
                                   pdFALSE, nullptr, do_halow_reconnect);
  if (s_reconnect_timer == nullptr) {
    ESP_LOGW(TAG, "Could not create reconnect timer");
  }

  s_scan_lock = xSemaphoreCreateMutex();

  this->setup_complete_ = true;
  this->state_ = HalowState::CONNECTING;
  this->start_connect_();
}

void HalowComponent::start_connect_() {
  struct mmwlan_sta_args sta_args = MMWLAN_STA_ARGS_INIT;
  fill_sta_args(this, &sta_args);

  // Reset connection state flags
  s_link_up = false;
  s_link_down_event = false;
  s_sta_connected = false;

  ESP_LOGI(TAG, "Connecting to '%s' (%s)...", this->ssid_.c_str(), this->security_type_.c_str());

  s_last_attempt_ms = millis();
  enum mmwlan_status status = mmwlan_sta_enable(&sta_args, sta_status_cb);
  if (status != MMWLAN_SUCCESS) {
    ESP_LOGE(TAG, "STA enable failed: %d", status);
  }
}

bool HalowComponent::check_ip_() {
  struct mmipal_ip_config ip_config;
  if (mmipal_get_ip_config(&ip_config) != MMIPAL_SUCCESS) {
    return false;
  }
  if (strcmp(ip_config.ip_addr, "0.0.0.0") == 0) {
    return false;
  }
  network::IPAddress new_ip(ip_config.ip_addr);
  if (!this->ip_addresses_[0].is_set() || this->ip_addresses_[0] != new_ip) {
    this->ip_addresses_[0] = new_ip;
    ESP_LOGI(TAG, "Got IP: %s, Gateway: %s, Netmask: %s",
             ip_config.ip_addr, ip_config.gateway_addr, ip_config.netmask);
  }
  return true;
}

void HalowComponent::loop() {
  if (!this->setup_complete_)
    return;

  switch (this->state_) {
    case HalowState::CONNECTING: {
      // Require STA connected AND valid IP before transitioning.
      // Don't use check_ip_() alone — DHCP lease can be cached from previous session.
      bool sta_ready = s_sta_connected || s_link_up;
      if (sta_ready && this->check_ip_()) {
        // Connected and got IP
        this->state_ = HalowState::CONNECTED;
        if (this->reconnect_count_ > 0) {
          uint32_t downtime_s = (millis() - this->disconnect_time_) / 1000;
          ESP_LOGI(TAG, "=== LINK RECOVERED === after %lus downtime (%lu attempts)",
                   (unsigned long) downtime_s, (unsigned long) this->reconnect_count_);
        }
        this->reconnect_count_ = 0;
        ESP_LOGI(TAG, "HaLow connected to '%s'", this->ssid_.c_str());
        this->last_sensor_update_ = 0;  // Force immediate sensor update

        // Ensure the LWIP netif link is marked UP so ARP responses work.
        // Must hold the LWIP core lock — netif_set_link_up asserts this on IDF 5.5.
        {
          uint8_t mac[6];
          mmwlan_get_mac_addr(mac);
          LOCK_TCPIP_CORE();
          for (struct netif *nif = netif_list; nif != nullptr; nif = nif->next) {
            if (memcmp(nif->hwaddr, mac, 6) == 0) {
              if (!netif_is_link_up(nif)) {
                netif_set_link_up(nif);
                ESP_LOGI(TAG, "Forced LWIP netif link UP");
              }
              // Install byte counter hooks (once)
              if (s_original_linkoutput == nullptr && nif->linkoutput != nullptr) {
                // TX: wrap linkoutput to count bytes at the frame level
                s_original_linkoutput = nif->linkoutput;
                nif->linkoutput = halow_linkoutput_hook;
                // RX: handled by __wrap_tcpip_input (linker wrap)
                ESP_LOGI(TAG, "Byte counter hooks installed");
              }
              break;
            }
          }
          UNLOCK_TCPIP_CORE();
        }

        // First connection registers the interface with mDNS; later ones re-announce
        this->start_mdns_();
      } else if (millis() - s_last_attempt_ms > CONNECT_TIMEOUT_MS) {
        // No link within the timeout. Hand the retry to the reconnect timer rather
        // than calling the SDK from here: sta_disable/enable from the main loop can
        // wedge the SDK, and the timer task serialises all reconnect attempts.
        this->reconnect_count_++;
        ESP_LOGW(TAG, "Connect timeout (attempt %lu)", (unsigned long) this->reconnect_count_);
        s_last_attempt_ms = millis();  // the timer refreshes this when it actually retries
        s_reconnecting = false;
        schedule_reconnect();
      }
      break;
    }

    case HalowState::STOPPED:
      break;

    case HalowState::CONNECTED: {
      // Disconnect detection: mmipal link_status_callback handles reconnection
      // via a FreeRTOS timer (do_halow_reconnect). We just update our state here.
      if (s_link_down_event || s_reconnecting) {
        this->state_ = HalowState::CONNECTING;
        this->ip_addresses_ = {};
        // Start the connect timeout from the moment the link was lost
        s_last_attempt_ms = millis();
        if (s_link_down_event) {
          this->reconnect_count_++;
          this->disconnect_time_ = millis();
          s_link_down_event = false;
          ESP_LOGW(TAG, "=== LINK LOST === (mmipal callback triggered reconnect timer)");
        }
      } else {
        // Update sensors periodically
        uint32_t now = millis();
        if (now - this->last_sensor_update_ > SENSOR_UPDATE_INTERVAL_MS) {
          this->update_sensors_();
          this->last_sensor_update_ = now;
        }
        // Check for scan completion
        if (s_scan_complete && s_scan_running) {
          s_scan_running = false;
          s_scan_complete = false;
          this->publish_scan_results_();
        }
      }
      break;
    }
  }
}

void HalowComponent::update_sensors_() {
#ifdef USE_SENSOR
  if (this->rssi_sensor_ != nullptr) {
    int32_t rssi = mmwlan_get_rssi();
    if (rssi != INT32_MIN) {
      this->rssi_sensor_->publish_state((float) rssi);
    }
  }
  if (this->tx_pps_sensor_ != nullptr || this->rx_pps_sensor_ != nullptr) {
    uint32_t tx = 0, rx = 0;
    mmipal_get_link_packet_counts(&tx, &rx);
    float interval_s = SENSOR_UPDATE_INTERVAL_MS / 1000.0f;
    // Use signed delta to catch counter resets (wraps to negative = skip)
    int32_t tx_delta = (int32_t)(tx - this->prev_tx_packets_);
    int32_t rx_delta = (int32_t)(rx - this->prev_rx_packets_);
    if (this->prev_tx_packets_ > 0 && tx_delta > 0 && tx_delta < 100000) {
      if (this->tx_pps_sensor_ != nullptr)
        this->tx_pps_sensor_->publish_state((float) tx_delta / interval_s);
    }
    if (this->prev_rx_packets_ > 0 && rx_delta > 0 && rx_delta < 100000) {
      if (this->rx_pps_sensor_ != nullptr)
        this->rx_pps_sensor_->publish_state((float) rx_delta / interval_s);
    }
    this->prev_tx_packets_ = tx;
    this->prev_rx_packets_ = rx;
  }
  if (this->tx_kbps_sensor_ != nullptr || this->rx_kbps_sensor_ != nullptr) {
    uint32_t tx_b = s_tx_bytes;
    uint32_t rx_b = s_rx_bytes;
    float interval_s = SENSOR_UPDATE_INTERVAL_MS / 1000.0f;
    int32_t tx_byte_delta = (int32_t)(tx_b - this->prev_tx_bytes_);
    int32_t rx_byte_delta = (int32_t)(rx_b - this->prev_rx_bytes_);
    if (this->prev_tx_bytes_ > 0 && tx_byte_delta > 0 && tx_byte_delta < 10000000) {
      if (this->tx_kbps_sensor_ != nullptr)
        this->tx_kbps_sensor_->publish_state((float) tx_byte_delta * 8.0f / 1000.0f / interval_s);
    }
    if (this->prev_rx_bytes_ > 0 && rx_byte_delta > 0 && rx_byte_delta < 10000000) {
      if (this->rx_kbps_sensor_ != nullptr)
        this->rx_kbps_sensor_->publish_state((float) rx_byte_delta * 8.0f / 1000.0f / interval_s);
    }
    this->prev_tx_bytes_ = tx_b;
    this->prev_rx_bytes_ = rx_b;
  }

  // Rate control stats: find the CURRENTLY active rate by comparing deltas
  // since last update. The rate with the most new packets sent since last
  // check is the one the radio is using right now.
  if (this->mcs_sensor_ != nullptr || this->bandwidth_sensor_ != nullptr ||
      this->tx_success_rate_sensor_ != nullptr) {
    struct mmwlan_rc_stats *rc = mmwlan_get_rc_stats();
    if (rc != nullptr && rc->n_entries > 0) {
      uint32_t n = rc->n_entries;
      if (n > MAX_RC_ENTRIES) n = MAX_RC_ENTRIES;

      // Find rate with most NEW packets since last snapshot
      uint32_t max_delta = 0;
      uint32_t best_idx = 0;
      uint32_t total_delta_sent = 0;
      uint32_t total_delta_success = 0;
      for (uint32_t i = 0; i < n; i++) {
        uint32_t delta = rc->total_sent[i] - this->prev_rc_sent_[i];
        if (delta > max_delta) {
          max_delta = delta;
          best_idx = i;
        }
        total_delta_sent += delta;
        total_delta_success += (rc->total_success[i] - this->prev_rc_success_[i]);
      }

      // Save snapshot for next update
      for (uint32_t i = 0; i < n; i++) {
        this->prev_rc_sent_[i] = rc->total_sent[i];
        this->prev_rc_success_[i] = rc->total_success[i];
      }

      if (max_delta > 0) {
        uint32_t info = rc->rate_info[best_idx];
        uint8_t bw_raw = (info >> 0) & 0xF;
        uint8_t mcs = (info >> 4) & 0xF;
        uint8_t gi = (info >> 8) & 0x1;

        if (this->mcs_sensor_ != nullptr)
          this->mcs_sensor_->publish_state((float) mcs);
        if (this->bandwidth_sensor_ != nullptr) {
          // BW encoding: 0=1MHz, 1=2MHz, 2=4MHz, 3=8MHz (per 802.11ah)
          float bw_mhz = (bw_raw == 0) ? 1.0f : (bw_raw == 1) ? 2.0f :
                         (bw_raw == 2) ? 4.0f : 8.0f;
          this->bandwidth_sensor_->publish_state(bw_mhz);
        }
        float success_pct = (total_delta_sent > 0)
            ? (100.0f * total_delta_success / total_delta_sent) : 100.0f;
        if (this->tx_success_rate_sensor_ != nullptr && total_delta_sent > 0)
          this->tx_success_rate_sensor_->publish_state(success_pct);

        // Link Quality: human-friendly summary derived from MCS + TX success rate
        // MCS 0-7 indicates radio adaptation, success rate indicates actual delivery
#ifdef USE_TEXT_SENSOR
        if (this->link_quality_sensor_ != nullptr) {
          const char *quality;
          if (mcs >= 5 && success_pct > 95.0f)
            quality = "Excellent";
          else if (mcs >= 3 && success_pct > 90.0f)
            quality = "Good";
          else if (mcs >= 1 && success_pct > 80.0f)
            quality = "Fair";
          else if (success_pct > 50.0f)
            quality = "Poor";
          else
            quality = "Critical";
          this->link_quality_sensor_->publish_state(quality);
        }
#endif
      }
      mmwlan_free_rc_stats(rc);
    }
  }

  // UMAC stats — interference and error indicators
  if (this->tx_frames_dropped_sensor_ != nullptr || this->rx_frames_dropped_sensor_ != nullptr ||
      this->ccmp_failures_sensor_ != nullptr || this->hw_restarts_sensor_ != nullptr) {
    struct mmwlan_stats_umac_data umac_stats;
    if (mmwlan_get_umac_stats(&umac_stats) == MMWLAN_SUCCESS) {
      if (this->tx_frames_dropped_sensor_ != nullptr)
        this->tx_frames_dropped_sensor_->publish_state((float) umac_stats.datapath_txq_frames_dropped);
      if (this->rx_frames_dropped_sensor_ != nullptr)
        this->rx_frames_dropped_sensor_->publish_state((float) umac_stats.datapath_rxq_frames_dropped);
      if (this->ccmp_failures_sensor_ != nullptr)
        this->ccmp_failures_sensor_->publish_state((float) umac_stats.datapath_rx_ccmp_failures);
      if (this->hw_restarts_sensor_ != nullptr)
        this->hw_restarts_sensor_->publish_state((float) umac_stats.hw_restart_counter);
    }
  }
#endif

#ifdef USE_TEXT_SENSOR
  // Dynamic values — update every cycle
  if (this->ip_address_sensor_ != nullptr || this->gateway_sensor_ != nullptr ||
      this->subnet_sensor_ != nullptr) {
    struct mmipal_ip_config ip_config;
    if (mmipal_get_ip_config(&ip_config) == MMIPAL_SUCCESS) {
      if (this->ip_address_sensor_ != nullptr)
        this->ip_address_sensor_->publish_state(ip_config.ip_addr);
      if (this->gateway_sensor_ != nullptr)
        this->gateway_sensor_->publish_state(ip_config.gateway_addr);
      if (this->subnet_sensor_ != nullptr)
        this->subnet_sensor_->publish_state(ip_config.netmask);
    }
  }
  if (this->bssid_sensor_ != nullptr) {
    uint8_t bssid[6];
    if (mmwlan_get_bssid(bssid) == MMWLAN_SUCCESS) {
      char buf[18];
      snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
               bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
      this->bssid_sensor_->publish_state(buf);
    }
  }

  // Static values — publish once
  if (this->ssid_sensor_ != nullptr && this->ssid_sensor_->get_raw_state().empty()) {
    this->ssid_sensor_->publish_state(this->ssid_);
  }
  if (this->mac_address_sensor_ != nullptr && this->mac_address_sensor_->get_raw_state().empty()) {
    uint8_t mac[6];
    if (mmwlan_get_mac_addr(mac) == MMWLAN_SUCCESS) {
      char buf[18];
      snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      this->mac_address_sensor_->publish_state(buf);
    }
  }
  if (this->fw_version_sensor_ != nullptr && this->fw_version_sensor_->get_raw_state().empty()) {
    struct mmwlan_version ver;
    if (mmwlan_get_version(&ver) == MMWLAN_SUCCESS) {
      this->fw_version_sensor_->publish_state(ver.morse_fw_version);
    }
  }
#endif
}

// esp_netif wrapper around mmipal's raw LWIP netif, created on first connect
static struct esp_netif_obj *s_mdns_netif = nullptr;

void HalowComponent::start_mdns_() {
  if (s_mdns_netif != nullptr) {
    // Already registered: re-announce so peers learn the (possibly new) address
    mdns_netif_action((esp_netif_t *) s_mdns_netif, MDNS_EVENT_ANNOUNCE_IP4);
    return;
  }

  // Find mmipal's LWIP netif by matching HaLow MAC address
  uint8_t mac[6];
  if (mmwlan_get_mac_addr(mac) != MMWLAN_SUCCESS) {
    ESP_LOGW(TAG, "mDNS: could not get MAC address");
    return;
  }

  struct netif *mm_netif = nullptr;
  for (struct netif *nif = netif_list; nif != nullptr; nif = nif->next) {
    if (memcmp(nif->hwaddr, mac, 6) == 0) {
      mm_netif = nif;
      break;
    }
  }
  if (mm_netif == nullptr) {
    ESP_LOGW(TAG, "mDNS: could not find HaLow LWIP netif");
    return;
  }

  // Create a minimal esp_netif_obj wrapper around the raw LWIP netif.
  // mdns_register_netif() stores this pointer and later calls
  // esp_netif_get_ip_info() which reads from esp_netif->lwip_netif.
  auto *fake = (struct esp_netif_obj *) calloc(1, sizeof(struct esp_netif_obj));
  if (fake == nullptr) {
    ESP_LOGW(TAG, "mDNS: alloc failed");
    return;
  }
  fake->lwip_netif = mm_netif;
  fake->flags = (esp_netif_flags_t)(ESP_NETIF_FLAG_AUTOUP);
  fake->if_key = strdup("MM_HALOW_DEF");
  fake->if_desc = strdup("halow");
  fake->route_prio = 50;

  // ESPHome's mdns component normally initialises the responder and registers its
  // services (with the full TXT set: mac, friendly_name, api_encryption, ...)
  // during setup. mdns_init() is a no-op in that case, so its records are kept.
  esp_err_t err = mdns_init();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
    free(fake->if_key);
    free(fake->if_desc);
    free(fake);
    return;
  }

  const char *hostname = App.get_name().c_str();
  mdns_hostname_set(hostname);
  mdns_instance_name_set(hostname);

  // Register our wrapper netif with mDNS
  err = mdns_register_netif((esp_netif_t *) fake);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mDNS register netif failed: %s", esp_err_to_name(err));
    free(fake->if_key);
    free(fake->if_desc);
    free(fake);
    return;
  }
  s_mdns_netif = fake;

  err = mdns_netif_action((esp_netif_t *) fake, MDNS_EVENT_ENABLE_IP4);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mDNS enable IPv4 failed: %s", esp_err_to_name(err));
  }
  err = mdns_netif_action((esp_netif_t *) fake, MDNS_EVENT_ANNOUNCE_IP4);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mDNS announce failed: %s", esp_err_to_name(err));
  }

  // Fallback in case ESPHome's mdns component didn't register the API service
  // (for example if its init failed). Home Assistant discovers ESPHome devices
  // through this record.
  if (!mdns_service_exists("_esphomelib", "_tcp", nullptr)) {
    uint8_t base_mac[6];
    char mac_str[13] = {0};
    if (esp_efuse_mac_get_default(base_mac) == ESP_OK) {
      snprintf(mac_str, sizeof(mac_str), "%02x%02x%02x%02x%02x%02x", base_mac[0], base_mac[1],
               base_mac[2], base_mac[3], base_mac[4], base_mac[5]);
    }
    std::string friendly_name = App.get_friendly_name().c_str();
    mdns_txt_item_t txt[] = {
        {(char *) "version", (char *) ESPHOME_VERSION},
        {(char *) "mac", mac_str},
        {(char *) "platform", (char *) "ESP32"},
        {(char *) "board", (char *) ESPHOME_BOARD},
        {(char *) "network", (char *) "halow"},
        {(char *) "friendly_name", (char *) friendly_name.c_str()},
    };
    err = mdns_service_add(nullptr, "_esphomelib", "_tcp", 6053, txt, sizeof(txt) / sizeof(txt[0]));
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "mDNS: registered _esphomelib._tcp service");
    } else {
      ESP_LOGW(TAG, "mDNS: service add failed: %s", esp_err_to_name(err));
    }
  }

  ESP_LOGI(TAG, "mDNS: '%s.local' active on HaLow interface", hostname);
}

void HalowComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Wi-Fi HaLow (MM6108):");
  ESP_LOGCONFIG(TAG, "  SSID: '%s'", this->ssid_.c_str());
  ESP_LOGCONFIG(TAG, "  Security: %s", this->security_type_.c_str());
  ESP_LOGCONFIG(TAG, "  Country: %s", this->country_code_.c_str());
  if (this->max_tx_power_dbm_ > 0) {
    ESP_LOGCONFIG(TAG, "  Max TX power: %u dBm", this->max_tx_power_dbm_);
  }
  ESP_LOGCONFIG(TAG, "  SPI Pins: CLK=%d MOSI=%d MISO=%d CS=%d",
                this->spi_clk_pin_, this->spi_mosi_pin_,
                this->spi_miso_pin_, this->spi_cs_pin_);
  ESP_LOGCONFIG(TAG, "  Control Pins: RESET=%d WAKE=%d IRQ=%d BUSY=%d",
                this->reset_pin_, this->wake_pin_,
                this->spi_irq_pin_, this->busy_pin_);
  if (this->use_static_ip_) {
    ESP_LOGCONFIG(TAG, "  Static IP: %s", this->static_ip_.c_str());
    ESP_LOGCONFIG(TAG, "  Gateway: %s", this->static_gw_.c_str());
    ESP_LOGCONFIG(TAG, "  Subnet: %s", this->static_subnet_.c_str());
  } else {
    ESP_LOGCONFIG(TAG, "  IP: DHCP");
  }
  if (this->is_connected()) {
    ESP_LOGCONFIG(TAG, "  Current IP: %s", this->get_ip_address_str().c_str());
  }
}

std::string HalowComponent::get_ip_address_str() const {
  if (this->ip_addresses_[0].is_set()) {
    char buf[64];
    this->ip_addresses_[0].str_to(buf);
    return std::string(buf);
  }
  return "0.0.0.0";
}

// --- Channel Scan ---

static void halow_scan_rx_cb(const struct mmwlan_scan_result *result, void *arg) {
  ScanEntry entry{};
  size_t ssid_len = std::min<size_t>(result->ssid_len, MMWLAN_SSID_MAXLEN);
  memcpy(entry.ssid, result->ssid, ssid_len);
  memcpy(entry.bssid, result->bssid, 6);
  entry.rssi = result->rssi;
  entry.noise_dbm = result->noise_dbm;
  entry.freq_hz = result->channel_freq_hz;
  entry.bw_mhz = result->bw_mhz;

  ESP_LOGD(TAG, "Scan: %s (%02X:%02X:%02X:%02X:%02X:%02X) RSSI=%d noise=%d freq=%luHz bw=%uMHz",
           entry.ssid, entry.bssid[0], entry.bssid[1], entry.bssid[2], entry.bssid[3], entry.bssid[4],
           entry.bssid[5], (int) entry.rssi, (int) entry.noise_dbm, (unsigned long) entry.freq_hz,
           entry.bw_mhz);

  if (s_scan_lock != nullptr && xSemaphoreTake(s_scan_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (s_scan_entries.size() < MAX_SCAN_ENTRIES)
      s_scan_entries.push_back(entry);
    xSemaphoreGive(s_scan_lock);
  }
}

static void halow_scan_complete_cb(enum mmwlan_scan_state state, void *arg) {
  ESP_LOGI(TAG, "Channel scan complete (state=%d)", (int) state);
  s_scan_complete = true;
}

void HalowComponent::request_channel_scan() {
  if (s_scan_running) {
    ESP_LOGW(TAG, "Scan already in progress");
    return;
  }
  if (this->state_ != HalowState::CONNECTED) {
    ESP_LOGW(TAG, "Cannot scan — not connected");
    return;
  }
  if (s_scan_lock == nullptr) {
    return;
  }

  ESP_LOGI(TAG, "Starting channel scan...");
  xSemaphoreTake(s_scan_lock, portMAX_DELAY);
  s_scan_entries.clear();
  xSemaphoreGive(s_scan_lock);
  s_scan_complete = false;
  s_scan_running = true;

  struct mmwlan_scan_req scan_req = MMWLAN_SCAN_REQ_INIT;
  scan_req.scan_rx_cb = halow_scan_rx_cb;
  scan_req.scan_complete_cb = halow_scan_complete_cb;
  scan_req.scan_cb_arg = nullptr;

  enum mmwlan_status status = mmwlan_scan_request(&scan_req);
  if (status != MMWLAN_SUCCESS) {
    ESP_LOGW(TAG, "Scan request failed: %d", (int) status);
    s_scan_running = false;
  }
}

void HalowComponent::publish_scan_results_() {
  std::vector<ScanEntry> entries;
  xSemaphoreTake(s_scan_lock, portMAX_DELAY);
  entries.swap(s_scan_entries);
  xSemaphoreGive(s_scan_lock);

  // Strongest first, so the entries that fit in the state are the most relevant
  std::sort(entries.begin(), entries.end(),
            [](const ScanEntry &a, const ScanEntry &b) { return a.rssi > b.rssi; });

  // Noise floor: prefer the reading from our own AP's channel, else the quietest channel
  uint8_t our_bssid[6];
  bool have_bssid = mmwlan_get_bssid(our_bssid) == MMWLAN_SUCCESS;
  bool have_noise = false;
  int8_t noise_dbm = 0;
  for (const auto &e : entries) {
    if (have_bssid && memcmp(e.bssid, our_bssid, 6) == 0) {
      noise_dbm = e.noise_dbm;
      have_noise = true;
      break;
    }
    if (!have_noise || e.noise_dbm < noise_dbm) {
      noise_dbm = e.noise_dbm;
      have_noise = true;
    }
  }

#ifdef USE_TEXT_SENSOR
  if (this->scan_results_sensor_ != nullptr) {
    // Compact JSON, truncated to whole entries that fit Home Assistant's 255-char limit
    std::string json = "[";
    for (const auto &e : entries) {
      char ssid[MMWLAN_SSID_MAXLEN + 1];
      size_t i = 0;
      for (; e.ssid[i] != '\0' && i < MMWLAN_SSID_MAXLEN; i++) {
        char c = e.ssid[i];
        ssid[i] = (c == '"' || c == '\\' || (unsigned char) c < 0x20) ? '_' : c;
      }
      ssid[i] = '\0';
      char item[128];
      int n = snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"noise\":%d,\"mhz\":%.1f,\"bw\":%u}",
                       json.size() > 1 ? "," : "", ssid, (int) e.rssi, (int) e.noise_dbm,
                       e.freq_hz / 1e6f, e.bw_mhz);
      if (n <= 0 || json.size() + n + 1 > MAX_TEXT_SENSOR_LEN)
        break;
      json += item;
    }
    json += "]";
    this->scan_results_sensor_->publish_state(json);
  }
#endif
#ifdef USE_SENSOR
  if (this->noise_floor_sensor_ != nullptr && have_noise) {
    this->noise_floor_sensor_->publish_state((float) noise_dbm);
  }
#endif
  ESP_LOGI(TAG, "Scan results published: %u networks (noise=%d dBm)", (unsigned) entries.size(),
           (int) noise_dbm);
}

// --- ESPHome network integration ---

bool network_is_connected() {
  return global_halow_component != nullptr && global_halow_component->is_connected();
}

network::IPAddresses network_get_ip_addresses() {
  if (global_halow_component == nullptr)
    return {};
  return global_halow_component->get_ip_addresses();
}

}  // namespace halow
}  // namespace esphome
