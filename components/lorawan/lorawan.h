#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/number/number.h"
#include "esphome/components/mqtt/mqtt_client.h"

#include <RadioLib.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <atomic>
#include <functional>
#include <vector>

namespace esphome {
namespace lorawan {

enum class Encoding : uint8_t {
  UINT8,
  INT16,
  UINT16,
  INT32,
  UINT32,
  FLOAT32,
};

// Subset of CayenneLPP data types we expose.  Each maps to a fixed
// CayenneLPP::add*() method with its own byte layout & precision.
// Custom types (U16..I32) follow LPP framing but use unallocated type
// bytes from the 0xF0+ range, with a YAML-supplied `scale`.
enum class CayenneType : uint8_t {
  // Standard CayenneLPP types
  ANALOG_INPUT,
  ANALOG_OUTPUT,
  DIGITAL_INPUT,
  DIGITAL_OUTPUT,
  TEMPERATURE,
  HUMIDITY,
  ILLUMINANCE,
  PRESENCE,
  BAROMETER,
  VOLTAGE,
  CURRENT,
  PERCENTAGE,
  ALTITUDE,
  POWER,
  DIRECTION,
  // Custom types — see CAYENNE_CUSTOM_TYPE_* constants in lorawan.cpp
  U16,
  U32,
  I16,
  I32,
};

struct SensorEntry {
  sensor::Sensor *sensor;
  Encoding encoding;
  float scale;
};

// HA discovery metadata overrides — empty string means "inherit from the
// referenced ESPHome entity's get_*() at runtime".  Used by publish_discovery()
// to build HA MQTT discovery JSON for each entity.
struct HaOverrides {
  std::string name;
  std::string unit_of_measurement;
  std::string device_class;
  std::string state_class;
  std::string icon;
  std::string entity_category;
  // Number-only overrides; ignored on sensor/switch entries.
  std::string min_value;
  std::string max_value;
  std::string step;
  std::string mode;
};

struct CayenneSensorEntry {
  sensor::Sensor *sensor;
  uint8_t channel;
  CayenneType type;
  float scale;     // applied only for custom types; ignored for standard CayenneLPP types
  HaOverrides ha;  // per-entity HA MQTT discovery overrides (empty fields → inherit)
};

// Downlink mapping: when a CayenneLPP digital_output field arrives on `channel`,
// the matching switch is driven on/off from the main loop.
// If report_state is true, the switch's current state is echoed in every
// uplink as a CayenneLPP digital_input on the same channel.
// ha_discovery=false suppresses HA MQTT discovery while keeping downlink processing.
struct DownlinkSwitchEntry {
  switch_::Switch *sw;
  uint8_t channel;
  bool report_state;
  HaOverrides ha;
  bool ha_discovery{true};
};

// Downlink mapping for numeric fields (analog_output, u16/u32/i16/i32).
// `scale` multiplies the wire integer to produce the value applied to the
// number entity, so e.g. wire 5 with scale 60 → number state 300.
// If report_state is true, the number's current state is echoed in every
// uplink using the configured `type` on the same channel.
// ha_discovery=false suppresses HA MQTT discovery while keeping downlink processing.
struct DownlinkNumberEntry {
  number::Number *num;
  uint8_t channel;
  CayenneType type;
  float scale;
  bool report_state;
  HaOverrides ha;
  bool ha_discovery{true};
};

// Extra MQTT sensor whose state is pulled out of the ChirpStack uplink JSON
// itself (rxInfo, gatewayId, fCnt, etc.) — not from the LPP payload.  These
// are purely discovery-side: the device just emits the discovery message,
// HA's MQTT integration handles the JSON parsing via value_template.
struct DiagnosticSensorEntry {
  std::string slug;            // url-safe id (auto-derived from name in codegen)
  std::string name;
  std::string value_template;  // Jinja2 evaluated by HA on each uplink
  std::string unit_of_measurement;
  std::string device_class;
  std::string state_class;
  std::string icon;
  std::string entity_category;
};

// Compound switch: one HA MQTT switch that publishes a SINGLE downlink
// containing multiple LPP digital_output fields — toggling several device
// states (WiFi + deep-sleep, say) in a single ChirpStack queue item.
// static_bytes_on/off are baked at codegen time; include_number_channels lets
// publish_discovery() append live number values so all settings go in one frame.
struct CompoundSwitchEntry {
  std::string slug;
  std::string name;
  // Static prefix bytes — built at codegen time from `when_on` / `when_off`.
  // At publish_discovery time we append current-state bytes for each channel
  // in `include_number_channels` (looked up against downlink_numbers_) and
  // base64-encode the whole thing into the discovery payload_on/off.
  std::vector<uint8_t> static_bytes_on;
  std::vector<uint8_t> static_bytes_off;
  std::vector<uint8_t> include_number_channels;
  int fport;
  int state_from_channel;
  HaOverrides ha;
};

// Downlink button: one HA MQTT button that fires a SINGLE downlink.
// static_bytes hold the LPP fields baked at codegen time from `when_pressed`.
// include_number_channels lets publish_discovery() append current number values
// so all settings travel in one ChirpStack queue item.
struct DownlinkButtonEntry {
  std::string slug;
  std::string name;
  std::vector<uint8_t> static_bytes;
  std::vector<uint8_t> include_number_channels;
  int fport;
  HaOverrides ha;
};

// Binary sensor whose state is derived from the ChirpStack uplink JSON itself
// via a Jinja2 value_template that evaluates to "ON" or "OFF".
// Published under homeassistant/binary_sensor/ by publish_discovery().
struct DiagnosticBinarySensorEntry {
  std::string slug;
  std::string name;
  std::string value_template;
  std::string device_class;
  std::string icon;
  std::string entity_category;
};

// FreeRTOS queue payloads — pushed by the background task, consumed by loop().
struct PendingSwitchAction {
  switch_::Switch *sw;
  bool value;
};

struct PendingNumberAction {
  number::Number *num;
  float value;
};

class LoRaWANComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  // Trigger a single uplink+downlink cycle.  Non-blocking: the actual radio
  // work happens on the background task.  Call from YAML via `lorawan.send`,
  // from `interval:`, from `on_boot:` + `delay:`, etc.
  void send_now();
  // Publish retained HA MQTT discovery messages for every configured entity
  // (cayenne_sensors, downlink_switches, downlink_numbers).  Call once after
  // MQTT is connected; subsequent calls are idempotent (retained payloads).
  // Triggered from YAML via `lorawan.publish_discovery:`.
  void publish_discovery();
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_nss_pin(uint8_t pin)  { nss_pin_  = pin; }
  void set_dio1_pin(uint8_t pin) { dio1_pin_ = pin; }
  void set_rst_pin(uint8_t pin)  { rst_pin_  = pin; }
  void set_busy_pin(uint8_t pin) { busy_pin_ = pin; }
  void set_txen_pin(uint8_t pin) { txen_pin_ = pin; }
  void set_rxen_pin(uint8_t pin) { rxen_pin_ = pin; }

  void set_join_eui(uint64_t eui)                { join_eui_ = eui; }
  void set_dev_eui(uint64_t eui)                 { dev_eui_  = eui; }
  void set_app_key(const std::vector<uint8_t> &k){ app_key_  = k; }
  void set_port(uint8_t port)                    { port_     = port; }

  // MQTT discovery configuration (optional — only needed if you call publish_discovery).
  void set_mqtt_client(mqtt::MQTTClientComponent *client) { mqtt_client_ = client; }
  void set_chirpstack_app_id(const std::string &id)       { chirpstack_app_id_ = id; }
  void set_device_name(const std::string &name)           { device_name_ = name; }
  void set_discovery_prefix(const std::string &p)         { discovery_prefix_ = p; }
  void set_dev_eui_hex(const std::string &hex)            { dev_eui_hex_ = hex; }

  void add_sensor(sensor::Sensor *sensor, Encoding encoding, float scale);
  void add_cayenne_sensor(sensor::Sensor *sensor, uint8_t channel, CayenneType type, float scale,
                          const HaOverrides &ha = {});
  void add_downlink_switch(switch_::Switch *sw, uint8_t channel,
                           bool report_state = false, const HaOverrides &ha = {},
                           bool ha_discovery = true);
  void add_downlink_number(number::Number *num, uint8_t channel, CayenneType type, float scale,
                           bool report_state = false, const HaOverrides &ha = {},
                           bool ha_discovery = true);
  void add_diagnostic_sensor(const DiagnosticSensorEntry &entry);
  void add_compound_switch(const CompoundSwitchEntry &entry);
  void add_downlink_button(const DownlinkButtonEntry &entry);
  void add_diagnostic_binary_sensor(const DiagnosticBinarySensorEntry &entry);
  void set_payload_lambda(const std::function<std::vector<uint8_t>()> &lambda);

 protected:
  std::vector<uint8_t> build_payload_();
  void save_nvs_();
  // Parse a downlink payload in CayenneLPP framing and enqueue matching
  // switch actions to be applied from loop() on the main thread.
  void handle_downlink_(const uint8_t *data, size_t len);

  // FreeRTOS task entry — trampolines to lora_task_loop_() on the instance.
  static void task_trampoline_(void *arg);
  // Background task that owns all blocking RadioLib calls (join + uplinks).
  void lora_task_loop_();

  uint8_t nss_pin_{0}, dio1_pin_{0}, rst_pin_{0}, busy_pin_{0};
  uint8_t txen_pin_{0}, rxen_pin_{0};
  uint64_t join_eui_{0}, dev_eui_{0};
  std::vector<uint8_t> app_key_;
  uint8_t port_{1};

  std::vector<SensorEntry> sensors_;
  std::vector<CayenneSensorEntry> cayenne_sensors_;
  std::vector<DownlinkSwitchEntry> downlink_switches_;
  std::vector<DownlinkNumberEntry> downlink_numbers_;
  std::vector<DiagnosticSensorEntry> diagnostic_sensors_;
  std::vector<CompoundSwitchEntry> compound_switches_;
  std::vector<DownlinkButtonEntry> downlink_buttons_;
  std::vector<DiagnosticBinarySensorEntry> diagnostic_binary_sensors_;
  optional<std::function<std::vector<uint8_t>()>> payload_lambda_;

  // Dedicated VSPI instance — keeps LoRaWAN SPI isolated from the global
  // SPI object that ESPHome/WiFi may touch and corrupt before our setup().
  SPIClass lora_spi_{VSPI};

  SX1262 *radio_{nullptr};
  LoRaWANNode *node_{nullptr};

  // Concurrency: the queue carries pending uplinks from update() (main loop)
  // to lora_task_loop_() (background).  `ready_` is set by the task after a
  // successful join and read by update() — atomic for cross-task visibility.
  // pending_switch_queue_ carries downlink-derived switch actions back from
  // the task to loop().
  TaskHandle_t  task_handle_{nullptr};
  QueueHandle_t uplink_queue_{nullptr};
  QueueHandle_t pending_switch_queue_{nullptr};
  QueueHandle_t pending_number_queue_{nullptr};
  std::atomic<bool> ready_{false};
  // Set by lora_task_loop_() after every downlink; cleared by loop() which then
  // calls send_now() to keep the exchange going until no further downlink arrives.
  std::atomic<bool> follow_up_uplink_{false};

  // MQTT discovery state — set only when mqtt_discovery is configured in YAML.
  // publish_discovery() is a no-op unless mqtt_client_ and chirpstack_app_id_
  // are both populated.
  mqtt::MQTTClientComponent *mqtt_client_{nullptr};
  std::string chirpstack_app_id_;
  std::string device_name_;
  std::string discovery_prefix_{"homeassistant"};
  std::string dev_eui_hex_;
};

// YAML action: triggers a single uplink+downlink cycle via `lorawan.send:`.
template<typename... Ts>
class LoRaWANSendAction : public Action<Ts...> {
 public:
  explicit LoRaWANSendAction(LoRaWANComponent *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->send_now(); }
 protected:
  LoRaWANComponent *parent_;
};

// YAML action: publishes HA MQTT discovery for every configured entity.
//   - lorawan.publish_discovery:
template<typename... Ts>
class LoRaWANPublishDiscoveryAction : public Action<Ts...> {
 public:
  explicit LoRaWANPublishDiscoveryAction(LoRaWANComponent *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->publish_discovery(); }
 protected:
  LoRaWANComponent *parent_;
};

}  // namespace lorawan
}  // namespace esphome
