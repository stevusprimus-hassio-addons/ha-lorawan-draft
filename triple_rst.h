// triple_rst.h — detect three consecutive physical RST-button presses.
//
// Uses RTC RAM so the counter survives RST presses (CPU reset) but is cleared
// on power-on.  Any reset reason other than ESP_RST_EXT (the physical RST
// button) also clears the counter, so deep-sleep wakeups, watchdog resets, and
// panic crashes never accidentally accumulate toward the threshold.
//
// Usage (in radioio.yaml on_boot lambda):
//   if (triple_rst_check()) { id(triple_rst_active) = true; }
//   start_maintenance_ap((App.get_name() + "-ap").c_str(), "radioio12");
//
// To exit AP mode press RST once — that single press resets the counter to 1
// (not 3) so the device boots normally on the next reboot.
#pragma once
#include "esp_system.h"
#include "esp_wifi.h"   // IDF WiFi API — used by ESPHome's IDF WiFi component
#include <cstring>
#include <algorithm>

RTC_DATA_ATTR static int g_rst_count = 0;  // persists across RST presses

// Returns true on the boot where RST has been pressed `threshold` times in a
// row.  Resets the counter automatically when the threshold is reached.
// Any non-ESP_RST_EXT reset (power-on, deep-sleep, watchdog, panic, SW reset)
// resets the counter to 0 and returns false.
inline bool triple_rst_check(int threshold = 3) {
  esp_reset_reason_t r = esp_reset_reason();
  if (r == ESP_RST_EXT) {
    ++g_rst_count;
    ESP_LOGI("triple_rst", "RST press #%d / %d", g_rst_count, threshold);
    if (g_rst_count >= threshold) {
      g_rst_count = 0;
      return true;
    }
    return false;
  }
  // Any other reset type (power-on, deep-sleep, watchdog, panic, SW) clears the sequence.
  g_rst_count = 0;
  return false;
}

// Start a soft-AP on top of the already-running IDF WiFi stack.
// ESPHome has already called esp_wifi_start(); we just add the AP interface
// by switching to APSTA mode and configuring the AP side.
// Connect to `ssid` / `password` and open http://192.168.4.1 for ESPHome OTA.
inline void start_maintenance_ap(const char *ssid, const char *password) {
  wifi_config_t ap_cfg = {};
  size_t slen = std::min(strlen(ssid), (size_t)32);
  memcpy(ap_cfg.ap.ssid, ssid, slen);
  ap_cfg.ap.ssid_len = static_cast<uint8_t>(slen);
  size_t plen = std::min(strlen(password), (size_t)63);
  memcpy(ap_cfg.ap.password, password, plen);
  ap_cfg.ap.max_connection = 4;
  ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap_cfg.ap.channel = 6;
  esp_wifi_set_mode(WIFI_MODE_APSTA);
  esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
  // No esp_wifi_start() — stack is already running; mode + config change is enough.
}
