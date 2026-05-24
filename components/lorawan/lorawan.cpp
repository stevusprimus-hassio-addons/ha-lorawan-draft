#include "lorawan.h"
#include "esphome/core/log.h"
#include <SPI.h>
#include <Preferences.h>
#include <CayenneLPP.h>

namespace esphome {
namespace lorawan {

static const char *const TAG = "lorawan";

// Translate common RadioLib error codes into human-readable hints.
// Returns nullptr for unknown codes so the caller can fall back to "%d".
static const char *radiolib_err_str(int code) {
  switch (code) {
    // Generic radio errors
    case RADIOLIB_ERR_NONE:                  return "OK";
    case RADIOLIB_ERR_UNKNOWN:               return "UNKNOWN";
    case RADIOLIB_ERR_CHIP_NOT_FOUND:        return "CHIP_NOT_FOUND — module unresponsive (check power/wiring)";
    case RADIOLIB_ERR_PACKET_TOO_LONG:       return "PACKET_TOO_LONG";
    case RADIOLIB_ERR_TX_TIMEOUT:            return "TX_TIMEOUT";
    case RADIOLIB_ERR_RX_TIMEOUT:            return "RX_TIMEOUT — no downlink in either RX window";
    case RADIOLIB_ERR_CRC_MISMATCH:          return "CRC_MISMATCH";
    case RADIOLIB_ERR_INVALID_BANDWIDTH:     return "INVALID_BANDWIDTH";
    case RADIOLIB_ERR_INVALID_SPREADING_FACTOR: return "INVALID_SPREADING_FACTOR";
    case RADIOLIB_ERR_INVALID_CODING_RATE:   return "INVALID_CODING_RATE";
    case RADIOLIB_ERR_INVALID_FREQUENCY:     return "INVALID_FREQUENCY";
    case RADIOLIB_ERR_INVALID_OUTPUT_POWER:  return "INVALID_OUTPUT_POWER";
    case RADIOLIB_ERR_SPI_CMD_TIMEOUT:       return "SPI_CMD_TIMEOUT — BUSY stuck high (wiring or framework issue)";

    // LoRaWAN-specific
    case RADIOLIB_ERR_NETWORK_NOT_JOINED:    return "NETWORK_NOT_JOINED — call activateOTAA() first";
    case RADIOLIB_ERR_DOWNLINK_MALFORMED:    return "DOWNLINK_MALFORMED";
    case RADIOLIB_ERR_INVALID_REVISION:      return "INVALID_REVISION — LoRaWAN 1.0 vs 1.1 mismatch";
    case RADIOLIB_ERR_INVALID_PORT:          return "INVALID_PORT — port must be 1-223";
    case RADIOLIB_ERR_NO_RX_WINDOW:          return "NO_RX_WINDOW";
    case RADIOLIB_ERR_NO_JOIN_ACCEPT:        return "NO_JOIN_ACCEPT — gateway out of range, wrong keys, or DevNonce rejected by NS";
    case RADIOLIB_ERR_JOIN_NONCE_INVALID:    return "JOIN_NONCE_INVALID — replay protection; flush device in ChirpStack";
    case RADIOLIB_ERR_DWELL_TIME_EXCEEDED:   return "DWELL_TIME_EXCEEDED";
    case RADIOLIB_ERR_CHECKSUM_MISMATCH:     return "CHECKSUM_MISMATCH";
    case RADIOLIB_ERR_NO_CHANNEL_AVAILABLE:  return "NO_CHANNEL_AVAILABLE — duty cycle limit reached";
    default:                                  return nullptr;
  }
}

// ESP_LOGE wrapper that adds the symbolic error name when known.
#define LOG_RL_ERR(what, code) do { \
  const char *_s = radiolib_err_str(code); \
  if (_s) ESP_LOGE(TAG, "%s failed: %d (%s)", what, code, _s); \
  else    ESP_LOGE(TAG, "%s failed: %d", what, code); \
} while (0)

// Items passed through uplink_queue_ from update() → lora_task_loop_().
// Heap-allocated by the producer, deleted by the consumer.
struct UplinkRequest {
  std::vector<uint8_t> payload;
  uint8_t port;
};

// Custom LPP type-byte assignments.  The official CayenneLPP spec leaves
// the 0xF0+ range unallocated, so we use it for our extensions.  Keep
// these in sync with the decoder JS in README.md.
static constexpr uint8_t LPP_TYPE_U16 = 0xF0;
static constexpr uint8_t LPP_TYPE_U32 = 0xF1;
static constexpr uint8_t LPP_TYPE_I16 = 0xF2;
static constexpr uint8_t LPP_TYPE_I32 = 0xF3;

// Standard CayenneLPP downlink type for boolean control.
static constexpr uint8_t LPP_TYPE_DIGITAL_OUTPUT = 0x01;
// Standard CayenneLPP analog_output: signed 2 bytes, ÷100 scaling.
static constexpr uint8_t LPP_TYPE_ANALOG_OUTPUT  = 0x03;

void LoRaWANComponent::setup() {

  ESP_LOGD(TAG, "Setup begin");
  if (radio_ != nullptr) return;  // guard against re-entry

  if (app_key_.size() != 16) {
    ESP_LOGE(TAG, "app_key must be 16 bytes, got %u", app_key_.size());
    return;
  }

  // Use a dedicated VSPI instance rather than the global SPI object.
  // ESPHome may initialise other peripherals before our component and leave
  // the shared SPI object in an unexpected state; a fresh SPIClass(VSPI)
  // is unaffected by that.
  lora_spi_.begin(18 /*SCK*/, 19 /*MISO*/, 23 /*MOSI*/, -1 /*no HW-SS*/);

  radio_ = new SX1262(new Module(nss_pin_, dio1_pin_, rst_pin_, busy_pin_, lora_spi_));
  node_  = new LoRaWANNode(radio_, &EU868);

  int state = radio_->begin();
  if (state != RADIOLIB_ERR_NONE) {
    LOG_RL_ERR("Radio begin", state);
    delete node_; node_ = nullptr;
    delete radio_; radio_ = nullptr;
    return;
  }

  // setRfSwitchPins must be called AFTER begin() in RadioLib 7.x
  radio_->setRfSwitchPins(rxen_pin_, txen_pin_);

  state = node_->beginOTAA(join_eui_, dev_eui_, app_key_.data(), app_key_.data());
  if (state != RADIOLIB_ERR_NONE) {
    LOG_RL_ERR("beginOTAA", state);
    delete node_; node_ = nullptr;
    delete radio_; radio_ = nullptr;
    return;
  }

  ::Preferences prefs;
  if (prefs.begin("lorawan", true)) {
    if (prefs.isKey("nonces")) {
      uint8_t buf[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
      prefs.getBytes("nonces", buf, sizeof(buf));
      node_->setBufferNonces(buf);
      ESP_LOGD(TAG, "NVS nonces restored");
    }
    if (prefs.isKey("session")) {
      uint8_t buf[RADIOLIB_LORAWAN_SESSION_BUF_SIZE];
      prefs.getBytes("session", buf, sizeof(buf));
      node_->setBufferSession(buf);
      ESP_LOGD(TAG, "NVS session restored");
    }
    prefs.end();
  } else {
    ESP_LOGW(TAG, "NVS open failed — will do fresh join");
  }

  // Queue carries at most one pending uplink; extras are dropped with a warning.
  uplink_queue_ = xQueueCreate(1, sizeof(UplinkRequest *));
  // Queues of pending state changes derived from received downlinks.
  pending_switch_queue_ = xQueueCreate(8, sizeof(PendingSwitchAction));
  pending_number_queue_ = xQueueCreate(8, sizeof(PendingNumberAction));

  // Spawn the background task that owns activateOTAA() and all subsequent
  // sendReceive() calls.  Setup returns immediately; the join happens
  // asynchronously without holding up ESPHome's main loop.
  xTaskCreate(&LoRaWANComponent::task_trampoline_, "lorawan",
              /*stack*/ 4096, this, /*prio*/ 1, &task_handle_);

  ESP_LOGD(TAG, "Setup end (radio task spawned, join in progress)");
}

void LoRaWANComponent::task_trampoline_(void *arg) {
  static_cast<LoRaWANComponent *>(arg)->lora_task_loop_();
}

void LoRaWANComponent::lora_task_loop_() {
  // --- Phase 1: OTAA join.  Blocks ~6s but we are NOT subscribed to the
  // task watchdog, so this is fine. Retry on failure with a 30s backoff so
  // the device recovers from transient gateway outages without rebooting.
  for (;;) {
    int state = node_->activateOTAA();
    if (state == RADIOLIB_LORAWAN_NEW_SESSION) {
      ESP_LOGI(TAG, "OTAA join OK — new session");
      save_nvs_();
      break;
    }
    if (state == RADIOLIB_LORAWAN_SESSION_RESTORED) {
      ESP_LOGI(TAG, "OTAA join OK — session restored");
      break;
    }
    LOG_RL_ERR("activateOTAA", state);
    ESP_LOGW(TAG, "Retrying join in 30s...");
    vTaskDelay(pdMS_TO_TICKS(30000));
  }
  ready_.store(true, std::memory_order_release);

  // --- Phase 2: process uplinks ---
  UplinkRequest *req = nullptr;
  for (;;) {
    if (xQueueReceive(uplink_queue_, &req, portMAX_DELAY) != pdTRUE) continue;
    if (req == nullptr) continue;

    ESP_LOGD(TAG, "Sending %u-byte uplink on port %u",
             (unsigned) req->payload.size(), (unsigned) req->port);

    // Use the overload that captures downlink bytes — RX1/RX2 windows are
    // listened to anyway, this just makes the data available to us.
    uint8_t dl_buf[256];
    size_t  dl_len = sizeof(dl_buf);
    int state = node_->sendReceive(req->payload.data(), req->payload.size(), req->port,
                                   dl_buf, &dl_len);

    if (state > 0) {
      ESP_LOGI(TAG, "Uplink OK, downlink received on port %d (%u bytes)",
               state, (unsigned) dl_len);
      handle_downlink_(dl_buf, dl_len);
    } else if (state == RADIOLIB_ERR_NONE) {
      ESP_LOGI(TAG, "Uplink OK");
    } else {
      LOG_RL_ERR("Uplink", state);
    }
    delete req;
    save_nvs_();
  }
}

void LoRaWANComponent::handle_downlink_(const uint8_t *data, size_t len) {
  // Parse CayenneLPP framing: repeating [channel][type][data...] tuples.
  size_t i = 0;
  while (i + 1 < len) {
    const uint8_t ch  = data[i++];
    const uint8_t typ = data[i++];

    // Helper: check we have `n` more bytes; log + abort otherwise.
    auto need_bytes = [&](size_t n) -> bool {
      if (i + n > len) {
        ESP_LOGW(TAG, "Truncated downlink (type 0x%02X, ch %u) — needed %u, have %u",
                 typ, ch, (unsigned) n, (unsigned)(len - i));
        return false;
      }
      return true;
    };

    // Dispatch a numeric value to the first matching downlink_numbers entry.
    auto dispatch_number = [&](float wire_value) {
      for (const auto &entry : downlink_numbers_) {
        if (entry.channel == ch) {
          PendingNumberAction action{entry.num, wire_value * entry.scale};
          if (xQueueSend(pending_number_queue_, &action, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Pending number queue full, dropping action for channel %u", ch);
          }
          return;
        }
      }
      ESP_LOGW(TAG, "Downlink value on channel %u (type 0x%02X) → no matching number",
               ch, typ);
    };

    switch (typ) {
      case LPP_TYPE_DIGITAL_OUTPUT: {
        if (!need_bytes(1)) return;
        const bool value = data[i++] != 0;
        bool matched = false;
        for (const auto &entry : downlink_switches_) {
          if (entry.channel == ch) {
            PendingSwitchAction action{entry.sw, value};
            if (xQueueSend(pending_switch_queue_, &action, 0) != pdTRUE) {
              ESP_LOGW(TAG, "Pending switch queue full, dropping action for channel %u", ch);
            }
            matched = true;
            break;
          }
        }
        if (!matched) {
          ESP_LOGW(TAG, "Downlink digital_output on channel %u → no matching switch", ch);
        }
        break;
      }

      case LPP_TYPE_ANALOG_OUTPUT: {
        if (!need_bytes(2)) return;
        int16_t raw = static_cast<int16_t>((data[i] << 8) | data[i + 1]);
        i += 2;
        dispatch_number(raw / 100.0f);
        break;
      }

      case LPP_TYPE_U16: {
        if (!need_bytes(2)) return;
        uint16_t raw = (data[i] << 8) | data[i + 1];
        i += 2;
        dispatch_number(static_cast<float>(raw));
        break;
      }

      case LPP_TYPE_U32: {
        if (!need_bytes(4)) return;
        uint32_t raw = (static_cast<uint32_t>(data[i]) << 24) |
                       (static_cast<uint32_t>(data[i + 1]) << 16) |
                       (static_cast<uint32_t>(data[i + 2]) << 8) |
                        static_cast<uint32_t>(data[i + 3]);
        i += 4;
        dispatch_number(static_cast<float>(raw));
        break;
      }

      case LPP_TYPE_I16: {
        if (!need_bytes(2)) return;
        int16_t raw = static_cast<int16_t>((data[i] << 8) | data[i + 1]);
        i += 2;
        dispatch_number(static_cast<float>(raw));
        break;
      }

      case LPP_TYPE_I32: {
        if (!need_bytes(4)) return;
        int32_t raw = static_cast<int32_t>(
            (static_cast<uint32_t>(data[i]) << 24) |
            (static_cast<uint32_t>(data[i + 1]) << 16) |
            (static_cast<uint32_t>(data[i + 2]) << 8) |
             static_cast<uint32_t>(data[i + 3]));
        i += 4;
        dispatch_number(static_cast<float>(raw));
        break;
      }

      default:
        // Can't safely advance past an unknown type without knowing its size.
        ESP_LOGW(TAG, "Unhandled downlink LPP type 0x%02X on channel %u, aborting parse", typ, ch);
        return;
    }
  }
}

void LoRaWANComponent::loop() {
  // Drain switch actions deposited by handle_downlink_() and apply them.
  // turn_on()/turn_off() also fire any turn_on_action/turn_off_action the
  // user defined in YAML and call publish_state() under the hood.
  if (pending_switch_queue_ != nullptr) {
    PendingSwitchAction action;
    while (xQueueReceive(pending_switch_queue_, &action, 0) == pdTRUE) {
      ESP_LOGI(TAG, "Downlink → switch '%s' = %s",
               action.sw->get_name().c_str(), action.value ? "ON" : "OFF");
      if (action.value) action.sw->turn_on();
      else              action.sw->turn_off();
    }
  }

  // Drain pending number updates — make_call().perform() invokes the number's
  // set_action and publishes the new state back to Home Assistant.
  if (pending_number_queue_ != nullptr) {
    PendingNumberAction action;
    while (xQueueReceive(pending_number_queue_, &action, 0) == pdTRUE) {
      ESP_LOGI(TAG, "Downlink → number '%s' = %.3f",
               action.num->get_name().c_str(), action.value);
      auto call = action.num->make_call();
      call.set_value(action.value);
      call.perform();
    }
  }
}

void LoRaWANComponent::save_nvs_() {
  if (node_ == nullptr) return;
  ::Preferences prefs;
  prefs.begin("lorawan", false);
  prefs.putBytes("nonces",  node_->getBufferNonces(),  RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
  prefs.putBytes("session", node_->getBufferSession(), RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
  prefs.end();
  ESP_LOGV(TAG, "NVS saved");
}

void LoRaWANComponent::add_sensor(sensor::Sensor *sensor, Encoding encoding, float scale) {
  sensors_.push_back({sensor, encoding, scale});
}

void LoRaWANComponent::add_cayenne_sensor(sensor::Sensor *sensor, uint8_t channel,
                                          CayenneType type, float scale,
                                          const HaOverrides &ha) {
  cayenne_sensors_.push_back({sensor, channel, type, scale, ha});
}

void LoRaWANComponent::add_downlink_switch(switch_::Switch *sw, uint8_t channel,
                                            bool report_state, const HaOverrides &ha) {
  downlink_switches_.push_back({sw, channel, report_state, ha});
}

void LoRaWANComponent::add_downlink_number(number::Number *num, uint8_t channel,
                                            CayenneType type, float scale,
                                            bool report_state, const HaOverrides &ha) {
  downlink_numbers_.push_back({num, channel, type, scale, report_state, ha});
}

void LoRaWANComponent::add_diagnostic_sensor(const DiagnosticSensorEntry &entry) {
  diagnostic_sensors_.push_back(entry);
}

void LoRaWANComponent::add_compound_switch(const CompoundSwitchEntry &entry) {
  compound_switches_.push_back(entry);
}

void LoRaWANComponent::set_payload_lambda(const std::function<std::vector<uint8_t>()> &lambda) {
  payload_lambda_ = lambda;
}

std::vector<uint8_t> LoRaWANComponent::build_payload_() {
  if (payload_lambda_.has_value()) {
    return (*payload_lambda_)();
  }

  // --- CayenneLPP path -----------------------------------------------------
  if (!cayenne_sensors_.empty()) {
    CayenneLPP lpp(51);  // 51 bytes is the safe max at SF12 in EU868
    lpp.reset();
    std::vector<uint8_t> custom;  // custom-type fields appended after the LPP buffer

    int skipped = 0;
    for (auto &entry : cayenne_sensors_) {
      if (!entry.sensor->has_state()) {
        // Skip this field, keep going.  Aborting the whole uplink would also
        // kill the RX windows that follow it, blocking all downlink commands.
        ESP_LOGD(TAG, "Sensor on channel %u has no state yet, skipping field", entry.channel);
        ++skipped;
        continue;
      }
      const float v = entry.sensor->state;
      switch (entry.type) {
        // --- Standard CayenneLPP types (scaling fixed by spec) ---
        case CayenneType::ANALOG_INPUT:   lpp.addAnalogInput(entry.channel, v); break;
        case CayenneType::ANALOG_OUTPUT:  lpp.addAnalogOutput(entry.channel, v); break;
        case CayenneType::DIGITAL_INPUT:  lpp.addDigitalInput(entry.channel, v != 0.0f); break;
        case CayenneType::DIGITAL_OUTPUT: lpp.addDigitalOutput(entry.channel, v != 0.0f); break;
        case CayenneType::TEMPERATURE:    lpp.addTemperature(entry.channel, v); break;
        case CayenneType::HUMIDITY:       lpp.addRelativeHumidity(entry.channel, v); break;
        case CayenneType::ILLUMINANCE:    lpp.addLuminosity(entry.channel, static_cast<uint16_t>(v)); break;
        case CayenneType::PRESENCE:       lpp.addPresence(entry.channel, v != 0.0f); break;
        case CayenneType::BAROMETER:      lpp.addBarometricPressure(entry.channel, v); break;
        case CayenneType::VOLTAGE:        lpp.addVoltage(entry.channel, v); break;
        case CayenneType::CURRENT:        lpp.addCurrent(entry.channel, v); break;
        case CayenneType::PERCENTAGE:     lpp.addPercentage(entry.channel, static_cast<uint8_t>(v)); break;
        case CayenneType::ALTITUDE:       lpp.addAltitude(entry.channel, static_cast<int16_t>(v)); break;
        case CayenneType::POWER:          lpp.addPower(entry.channel, static_cast<uint16_t>(v)); break;
        case CayenneType::DIRECTION:      lpp.addDirection(entry.channel, v); break;

        // --- Custom types (channel + type byte + raw big-endian integer) ---
        case CayenneType::U16: {
          uint16_t n = static_cast<uint16_t>(v * entry.scale);
          custom.insert(custom.end(), {entry.channel, LPP_TYPE_U16,
                                       static_cast<uint8_t>((n >> 8) & 0xFF),
                                       static_cast<uint8_t>(n & 0xFF)});
          break;
        }
        case CayenneType::U32: {
          uint32_t n = static_cast<uint32_t>(v * entry.scale);
          custom.insert(custom.end(), {entry.channel, LPP_TYPE_U32,
                                       static_cast<uint8_t>((n >> 24) & 0xFF),
                                       static_cast<uint8_t>((n >> 16) & 0xFF),
                                       static_cast<uint8_t>((n >> 8) & 0xFF),
                                       static_cast<uint8_t>(n & 0xFF)});
          break;
        }
        case CayenneType::I16: {
          int16_t n = static_cast<int16_t>(v * entry.scale);
          uint16_t u = static_cast<uint16_t>(n);  // two's-complement bit pattern
          custom.insert(custom.end(), {entry.channel, LPP_TYPE_I16,
                                       static_cast<uint8_t>((u >> 8) & 0xFF),
                                       static_cast<uint8_t>(u & 0xFF)});
          break;
        }
        case CayenneType::I32: {
          int32_t n = static_cast<int32_t>(v * entry.scale);
          uint32_t u = static_cast<uint32_t>(n);
          custom.insert(custom.end(), {entry.channel, LPP_TYPE_I32,
                                       static_cast<uint8_t>((u >> 24) & 0xFF),
                                       static_cast<uint8_t>((u >> 16) & 0xFF),
                                       static_cast<uint8_t>((u >> 8) & 0xFF),
                                       static_cast<uint8_t>(u & 0xFF)});
          break;
        }
      }
    }

    // --- Echo downlink_switches with report_state=true as digital_input -----
    // Same channel as the downlink (digital_output 0x01), but uplinked as
    // digital_input (0x00).  Receiver value_template extracts ch<N>_digital_input.
    for (const auto &sw_entry : downlink_switches_) {
      if (!sw_entry.report_state) continue;
      lpp.addDigitalInput(sw_entry.channel, sw_entry.sw->state ? 1 : 0);
    }

    // --- Echo downlink_numbers with report_state=true using their LPP type --
    // wire integer = state / scale  (inverse of the downlink scaling).
    for (const auto &n_entry : downlink_numbers_) {
      if (!n_entry.report_state) continue;
      if (!n_entry.num->has_state()) continue;
      const float wire = n_entry.num->state / (n_entry.scale == 0.0f ? 1.0f : n_entry.scale);
      switch (n_entry.type) {
        case CayenneType::ANALOG_OUTPUT:
          lpp.addAnalogOutput(n_entry.channel, wire);
          break;
        case CayenneType::U16: {
          uint16_t v = static_cast<uint16_t>(wire);
          custom.insert(custom.end(), {n_entry.channel, LPP_TYPE_U16,
                                       static_cast<uint8_t>((v >> 8) & 0xFF),
                                       static_cast<uint8_t>(v & 0xFF)});
          break;
        }
        case CayenneType::U32: {
          uint32_t v = static_cast<uint32_t>(wire);
          custom.insert(custom.end(), {n_entry.channel, LPP_TYPE_U32,
                                       static_cast<uint8_t>((v >> 24) & 0xFF),
                                       static_cast<uint8_t>((v >> 16) & 0xFF),
                                       static_cast<uint8_t>((v >> 8) & 0xFF),
                                       static_cast<uint8_t>(v & 0xFF)});
          break;
        }
        case CayenneType::I16: {
          int16_t v = static_cast<int16_t>(wire);
          uint16_t u = static_cast<uint16_t>(v);
          custom.insert(custom.end(), {n_entry.channel, LPP_TYPE_I16,
                                       static_cast<uint8_t>((u >> 8) & 0xFF),
                                       static_cast<uint8_t>(u & 0xFF)});
          break;
        }
        case CayenneType::I32: {
          int32_t v = static_cast<int32_t>(wire);
          uint32_t u = static_cast<uint32_t>(v);
          custom.insert(custom.end(), {n_entry.channel, LPP_TYPE_I32,
                                       static_cast<uint8_t>((u >> 24) & 0xFF),
                                       static_cast<uint8_t>((u >> 16) & 0xFF),
                                       static_cast<uint8_t>((u >> 8) & 0xFF),
                                       static_cast<uint8_t>(u & 0xFF)});
          break;
        }
        default:
          // Other types not currently supported as echo sources.
          break;
      }
    }

    std::vector<uint8_t> out(lpp.getBuffer(), lpp.getBuffer() + lpp.getSize());
    out.insert(out.end(), custom.begin(), custom.end());
    if (skipped > 0) {
      ESP_LOGW(TAG, "Cayenne payload: %u sensor(s) skipped (no state); sending %u-byte uplink anyway",
               (unsigned) skipped, (unsigned) out.size());
    }
    return out;
  }

  // --- Raw encoding path ---------------------------------------------------
  std::vector<uint8_t> payload;
  int raw_skipped = 0;
  for (auto &entry : sensors_) {
    if (!entry.sensor->has_state()) {
      // Same rationale as the Cayenne path: skip missing fields, don't abort.
      ESP_LOGD(TAG, "Sensor has no state yet, skipping field");
      ++raw_skipped;
      continue;
    }
    float raw = entry.sensor->state * entry.scale;
    switch (entry.encoding) {
      case Encoding::UINT8: {
        if (raw < 0.0f || raw > 255.0f)
          ESP_LOGW(TAG, "UINT8 value %.1f out of range [0, 255], truncating", raw);
        payload.push_back(static_cast<uint8_t>(raw));
        break;
      }
      case Encoding::INT16: {
        int16_t v = static_cast<int16_t>(raw);
        payload.push_back((v >> 8) & 0xFF);
        payload.push_back(v & 0xFF);
        break;
      }
      case Encoding::UINT16: {
        uint16_t v = static_cast<uint16_t>(raw);
        payload.push_back((v >> 8) & 0xFF);
        payload.push_back(v & 0xFF);
        break;
      }
      case Encoding::INT32: {
        int32_t v = static_cast<int32_t>(raw);
        payload.push_back((v >> 24) & 0xFF);
        payload.push_back((v >> 16) & 0xFF);
        payload.push_back((v >> 8) & 0xFF);
        payload.push_back(v & 0xFF);
        break;
      }
      case Encoding::UINT32: {
        uint32_t v = static_cast<uint32_t>(raw);
        payload.push_back((v >> 24) & 0xFF);
        payload.push_back((v >> 16) & 0xFF);
        payload.push_back((v >> 8) & 0xFF);
        payload.push_back(v & 0xFF);
        break;
      }
      case Encoding::FLOAT32: {
        uint32_t tmp;
        memcpy(&tmp, &raw, 4);  // reinterpret bits without UB
        payload.push_back((tmp >> 24) & 0xFF);
        payload.push_back((tmp >> 16) & 0xFF);
        payload.push_back((tmp >> 8)  & 0xFF);
        payload.push_back( tmp        & 0xFF);
        break;
      }
    }
  }
  if (raw_skipped > 0) {
    ESP_LOGW(TAG, "Raw payload: %u sensor(s) skipped (no state); sending %u-byte uplink anyway",
             (unsigned) raw_skipped, (unsigned) payload.size());
  }
  return payload;
}

void LoRaWANComponent::send_now() {
  if (!ready_.load(std::memory_order_acquire)) {
    ESP_LOGW(TAG, "Not ready (join in progress or failed), skipping uplink");
    return;
  }

  auto payload = build_payload_();
  if (payload.empty()) {
    // We still send.  An empty-payload Class A uplink opens the RX1/RX2
    // windows, which is the only way for queued downlinks (control commands,
    // sleep duration changes, etc.) to ever reach the device.  Suppressing
    // the uplink here would lock the device out of remote control.
    ESP_LOGI(TAG, "No sensor data ready — sending empty uplink to keep RX windows open");
  }

  // Hand the payload to the background task and return immediately.
  // The radio work (~2-6s) happens off the main loop — no "long time" warning
  // and other components (WiFi, API, sensors) stay responsive.
  auto *req = new UplinkRequest{std::move(payload), port_};
  if (xQueueSend(uplink_queue_, &req, 0) != pdTRUE) {
    ESP_LOGW(TAG, "Previous uplink still in flight, dropping this one");
    delete req;
  }
}

// =========================================================================== //
// HA MQTT discovery — publishes retained config messages for every entity.    //
// Layout: homeassistant/<comp>/lorawan_<dev_eui>/ch<N>/config                  //
// =========================================================================== //

namespace {

// Map our LPP type enum to the string the ChirpStack codec uses in `object`.
// Must match the `name` field in the JS decoder template (see README).
const char *cayenne_type_to_lpp_name(CayenneType t) {
  switch (t) {
    case CayenneType::ANALOG_INPUT:   return "analog_input";
    case CayenneType::ANALOG_OUTPUT:  return "analog_output";
    case CayenneType::DIGITAL_INPUT:  return "digital_input";
    case CayenneType::DIGITAL_OUTPUT: return "digital_output";
    case CayenneType::TEMPERATURE:    return "temperature";
    case CayenneType::HUMIDITY:       return "humidity";
    case CayenneType::ILLUMINANCE:    return "illuminance";
    case CayenneType::PRESENCE:       return "presence";
    case CayenneType::BAROMETER:      return "barometer";
    case CayenneType::VOLTAGE:        return "voltage";
    case CayenneType::CURRENT:        return "current";
    case CayenneType::PERCENTAGE:     return "percentage";
    case CayenneType::ALTITUDE:       return "altitude";
    case CayenneType::POWER:          return "power";
    case CayenneType::DIRECTION:      return "direction";
    case CayenneType::U16:            return "u16";
    case CayenneType::U32:            return "u32";
    case CayenneType::I16:            return "i16";
    case CayenneType::I32:            return "i32";
  }
  return "unknown";
}

// ESPHome state class enum → HA discovery string.  Empty string = none.
const char *sc_to_ha_string(sensor::StateClass sc) {
  switch (sc) {
    case sensor::STATE_CLASS_MEASUREMENT:       return "measurement";
    case sensor::STATE_CLASS_TOTAL_INCREASING:  return "total_increasing";
    case sensor::STATE_CLASS_TOTAL:             return "total";
    default:                                     return "";
  }
}

// ESPHome entity category enum → HA discovery string.  Empty = none.
const char *entity_category_to_string(EntityCategory ec) {
  switch (ec) {
    case ENTITY_CATEGORY_CONFIG:      return "config";
    case ENTITY_CATEGORY_DIAGNOSTIC:  return "diagnostic";
    default:                           return "";
  }
}

// Escape a string for JSON-inside-string embedding.
std::string json_escape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

// Pick `override` if non-empty, else `inherited`.
inline std::string pick(const std::string &override_v, const std::string &inherited) {
  return override_v.empty() ? inherited : override_v;
}

// Append `"key":"escaped_value"` to JSON if value non-empty; with comma prefix.
void json_append_str(std::string &json, const char *key, const std::string &value) {
  if (value.empty()) return;
  json += ",\"";
  json += key;
  json += "\":\"";
  json += json_escape(value);
  json += "\"";
}

}  // anonymous namespace

void LoRaWANComponent::publish_discovery() {
  if (mqtt_client_ == nullptr) {
    ESP_LOGW(TAG, "publish_discovery: mqtt_client not configured, skipping");
    return;
  }
  if (chirpstack_app_id_.empty() || dev_eui_hex_.empty()) {
    ESP_LOGW(TAG, "publish_discovery: chirpstack_app_id or dev_eui not set, skipping");
    return;
  }

  const std::string app_path = "chirpstack/application/" + chirpstack_app_id_ +
                                "/device/" + dev_eui_hex_;
  const std::string uplink_topic   = app_path + "/event/up";
  const std::string downlink_topic = app_path + "/command/down";
  const std::string device_id      = "lorawan_" + dev_eui_hex_;

  // Shared "device" block embedded in every discovery payload.
  const std::string device_block =
      ",\"device\":{\"identifiers\":[\"" + device_id + "\"]"
      ",\"name\":\"" + json_escape(device_name_.empty() ? device_id : device_name_) + "\""
      ",\"manufacturer\":\"ChirpStack\""
      ",\"model\":\"LoRaWAN node\"}";

  // ---------- Sensors --------------------------------------------------------
  for (const auto &e : cayenne_sensors_) {
    const std::string ch_str  = std::to_string(e.channel);
    const std::string uid     = device_id + "_ch" + ch_str;
    const std::string topic   = discovery_prefix_ + "/sensor/" + device_id + "/ch" + ch_str + "/config";
    const std::string field   = std::string("ch") + ch_str + "_" + cayenne_type_to_lpp_name(e.type);

    const std::string name        = pick(e.ha.name,                 e.sensor->get_name());
    const std::string unit        = pick(e.ha.unit_of_measurement,  e.sensor->get_unit_of_measurement());
    const std::string device_class = pick(e.ha.device_class,        e.sensor->get_device_class());
    const std::string state_class = pick(e.ha.state_class,          sc_to_ha_string(e.sensor->get_state_class()));
    const std::string icon        = pick(e.ha.icon,                 e.sensor->get_icon());
    const std::string ent_cat     = pick(e.ha.entity_category,      entity_category_to_string(e.sensor->get_entity_category()));

    std::string payload = "{\"name\":\"" + json_escape(name) + "\"";
    payload += ",\"unique_id\":\"" + uid + "\"";
    payload += ",\"state_topic\":\"" + uplink_topic + "\"";
    payload += ",\"value_template\":\"{{ value_json.object." + field + " }}\"";
    json_append_str(payload, "unit_of_measurement", unit);
    json_append_str(payload, "device_class",        device_class);
    json_append_str(payload, "state_class",         state_class);
    json_append_str(payload, "icon",                icon);
    json_append_str(payload, "entity_category",     ent_cat);
    payload += device_block;
    payload += "}";

    mqtt_client_->publish(topic, payload, 0, true);
    ESP_LOGI(TAG, "Discovery: sensor ch%u → '%s'", e.channel, name.c_str());
  }

  // ---------- Switches -------------------------------------------------------
  for (const auto &e : downlink_switches_) {
    const std::string ch_str = std::to_string(e.channel);
    const std::string uid    = device_id + "_ch" + ch_str;
    const std::string topic  = discovery_prefix_ + "/switch/" + device_id + "/ch" + ch_str + "/config";

    const std::string name    = pick(e.ha.name, e.sw->get_name());
    const std::string icon    = pick(e.ha.icon, e.sw->get_icon());
    const std::string ent_cat = pick(e.ha.entity_category, entity_category_to_string(e.sw->get_entity_category()));

    // Round-tripped via uplink echo if report_state=true.  Otherwise the
    // switch acts as a pure command (no state reflected back from device).
    //
    // state_on / state_off MUST be explicit: HA's MQTT switch defaults them to
    // payload_on / payload_off, which we set to the full LPP downlink JSON.
    // Without these the value_template output ("ON"/"OFF") would never match
    // and HA would show the switch as state-unknown.
    std::string state_topic_block;
    if (e.report_state) {
      state_topic_block =
          ",\"state_topic\":\"" + uplink_topic + "\""
          ",\"value_template\":\"{{ 'ON' if value_json.object.ch" + ch_str +
              "_digital_input == 1 else 'OFF' }}\""
          ",\"state_on\":\"ON\""
          ",\"state_off\":\"OFF\""
          ",\"optimistic\":false";
    } else {
      // No state coming back; HA's UI is optimistic by default in this case.
      state_topic_block = ",\"optimistic\":true";
    }

    // payload_on / payload_off — pre-baked LPP digital_output JSON payloads.
    // Bytes: [channel][0x01][value].  3 bytes → 4 base64 chars (no padding).
    auto encode = [&](bool on) -> std::string {
      static const char ALPHA[] =
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      uint8_t b0 = e.channel, b1 = 0x01, b2 = on ? 0x01 : 0x00;
      char b64[5];
      b64[0] = ALPHA[(b0 >> 2) & 0x3F];
      b64[1] = ALPHA[((b0 << 4) | (b1 >> 4)) & 0x3F];
      b64[2] = ALPHA[((b1 << 2) | (b2 >> 6)) & 0x3F];
      b64[3] = ALPHA[b2 & 0x3F];
      b64[4] = '\0';
      return std::string("{\"devEui\":\"") + dev_eui_hex_ + "\""
           + ",\"confirmed\":false"
           + ",\"fPort\":" + std::to_string(e.channel)
           + ",\"data\":\"" + b64 + "\"}";
    };

    std::string payload = "{\"name\":\"" + json_escape(name) + "\"";
    payload += ",\"unique_id\":\"" + uid + "\"";
    payload += ",\"command_topic\":\"" + downlink_topic + "\"";
    payload += ",\"payload_on\":\"" + json_escape(encode(true))  + "\"";
    payload += ",\"payload_off\":\"" + json_escape(encode(false)) + "\"";
    payload += state_topic_block;
    json_append_str(payload, "icon",            icon);
    json_append_str(payload, "entity_category", ent_cat);
    payload += device_block;
    payload += "}";

    mqtt_client_->publish(topic, payload, 0, true);
    ESP_LOGI(TAG, "Discovery: switch ch%u → '%s' (round-trip=%s)",
             e.channel, name.c_str(), e.report_state ? "yes" : "no");
  }

  // ---------- Numbers --------------------------------------------------------
  // command_template is pure-Jinja2 base64 encoder for the LPP type's bytes.
  for (const auto &e : downlink_numbers_) {
    const std::string ch_str = std::to_string(e.channel);
    const std::string uid    = device_id + "_ch" + ch_str;
    const std::string topic  = discovery_prefix_ + "/number/" + device_id + "/ch" + ch_str + "/config";
    const std::string field  = std::string("ch") + ch_str + "_" + cayenne_type_to_lpp_name(e.type);

    const std::string name        = pick(e.ha.name,                e.num->get_name());
    const std::string unit        = pick(e.ha.unit_of_measurement, e.num->get_unit_of_measurement());
    const std::string device_class = pick(e.ha.device_class,       e.num->get_device_class());
    const std::string icon        = pick(e.ha.icon,                e.num->get_icon());
    const std::string ent_cat     = pick(e.ha.entity_category, entity_category_to_string(e.num->get_entity_category()));
    const std::string min_v       = pick(e.ha.min_value, std::to_string(static_cast<int>(e.num->traits.get_min_value())));
    const std::string max_v       = pick(e.ha.max_value, std::to_string(static_cast<int>(e.num->traits.get_max_value())));
    const std::string step_v      = pick(e.ha.step,      std::to_string(static_cast<int>(e.num->traits.get_step())));
    const std::string mode_v      = e.ha.mode.empty() ? std::string("box") : e.ha.mode;

    // Build the Jinja2 command_template that encodes [ch][type_byte][bytes...]
    // into base64 in pure HA template syntax.  Per LPP type, byte layout differs.
    // value_pre = (value / scale) cast to int — inverse of downlink scaling.
    std::string scale_div = "{% set v = (value | float) / " +
                            std::to_string(static_cast<double>(e.scale == 0.0f ? 1.0f : e.scale)) +
                            " %}{% set v = v | int %}";

    std::string type_byte_str;
    std::string byte_extract;  // sets b2,b3,(b4,b5) from v
    int total_bytes = 2;       // channel + type, plus N data bytes
    int data_bytes = 0;

    switch (e.type) {
      case CayenneType::ANALOG_OUTPUT: {
        type_byte_str = "3";  // 0x03
        // analog_output: value * 100 as int16 signed, big-endian
        byte_extract  = "{% set vw = (value | float * 100) | int %}"
                        "{% if vw < 0 %}{% set vw = vw + 65536 %}{% endif %}"
                        "{% set b2 = vw // 256 %}{% set b3 = vw % 256 %}";
        data_bytes = 2;
        break;
      }
      case CayenneType::U16:
        type_byte_str = "240";  // 0xF0
        byte_extract  = scale_div + "{% set b2 = v // 256 %}{% set b3 = v % 256 %}";
        data_bytes = 2;
        break;
      case CayenneType::U32:
        type_byte_str = "241";  // 0xF1
        byte_extract  = scale_div +
                        "{% set b2 = v // 16777216 %}"
                        "{% set b3 = (v // 65536) % 256 %}"
                        "{% set b4 = (v // 256) % 256 %}"
                        "{% set b5 = v % 256 %}";
        data_bytes = 4;
        break;
      case CayenneType::I16:
        type_byte_str = "242";  // 0xF2
        byte_extract  = scale_div +
                        "{% if v < 0 %}{% set v = v + 65536 %}{% endif %}"
                        "{% set b2 = v // 256 %}{% set b3 = v % 256 %}";
        data_bytes = 2;
        break;
      case CayenneType::I32:
        type_byte_str = "243";  // 0xF3
        byte_extract  = scale_div +
                        "{% if v < 0 %}{% set v = v + 4294967296 %}{% endif %}"
                        "{% set b2 = v // 16777216 %}"
                        "{% set b3 = (v // 65536) % 256 %}"
                        "{% set b4 = (v // 256) % 256 %}"
                        "{% set b5 = v % 256 %}";
        data_bytes = 4;
        break;
      default:
        ESP_LOGW(TAG, "Discovery: number ch%u type unsupported, skipping", e.channel);
        continue;
    }
    total_bytes = 2 + data_bytes;
    (void) total_bytes;

    std::string alpha_set =
        "{% set alpha = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/' %}";
    std::string ch_set =
        "{% set b0 = " + ch_str + " %}{% set b1 = " + type_byte_str + " %}";

    // Build base64 string depending on data byte count (4 vs 6 vs 8 chars).
    std::string b64_build;
    if (data_bytes == 2) {
      // 4 input bytes total → 6 base64 + 2 padding
      b64_build =
          "{% set c0 = alpha[b0 // 4] %}"
          "{% set c1 = alpha[(b0 % 4) * 16 + b1 // 16] %}"
          "{% set c2 = alpha[(b1 % 16) * 4 + b2 // 64] %}"
          "{% set c3 = alpha[b2 % 64] %}"
          "{% set c4 = alpha[b3 // 4] %}"
          "{% set c5 = alpha[(b3 % 4) * 16] %}"
          "{% set b64 = c0 ~ c1 ~ c2 ~ c3 ~ c4 ~ c5 ~ '==' %}";
    } else /* data_bytes == 4 */ {
      // 6 input bytes total → 8 base64 + 0 padding
      b64_build =
          "{% set c0 = alpha[b0 // 4] %}"
          "{% set c1 = alpha[(b0 % 4) * 16 + b1 // 16] %}"
          "{% set c2 = alpha[(b1 % 16) * 4 + b2 // 64] %}"
          "{% set c3 = alpha[b2 % 64] %}"
          "{% set c4 = alpha[b3 // 4] %}"
          "{% set c5 = alpha[(b3 % 4) * 16 + b4 // 16] %}"
          "{% set c6 = alpha[(b4 % 16) * 4 + b5 // 64] %}"
          "{% set c7 = alpha[b5 % 64] %}"
          "{% set b64 = c0 ~ c1 ~ c2 ~ c3 ~ c4 ~ c5 ~ c6 ~ c7 %}";
    }

    std::string command_template =
        alpha_set + ch_set + byte_extract + b64_build +
        "{\"devEui\":\"" + dev_eui_hex_ + "\""
        ",\"confirmed\":false"
        ",\"fPort\":" + ch_str +
        ",\"data\":\"{{ b64 }}\"}";

    std::string payload = "{\"name\":\"" + json_escape(name) + "\"";
    payload += ",\"unique_id\":\"" + uid + "\"";
    payload += ",\"command_topic\":\"" + downlink_topic + "\"";
    payload += ",\"command_template\":\"" + json_escape(command_template) + "\"";
    if (e.report_state) {
      payload += ",\"state_topic\":\"" + uplink_topic + "\"";
      payload += ",\"value_template\":\"{{ value_json.object." + field + " }}\"";
    }
    payload += ",\"min\":"  + min_v;
    payload += ",\"max\":"  + max_v;
    payload += ",\"step\":" + step_v;
    payload += ",\"mode\":\"" + mode_v + "\"";
    json_append_str(payload, "unit_of_measurement", unit);
    json_append_str(payload, "device_class",        device_class);
    json_append_str(payload, "icon",                icon);
    json_append_str(payload, "entity_category",     ent_cat);
    payload += device_block;
    payload += "}";

    mqtt_client_->publish(topic, payload, 0, true);
    ESP_LOGI(TAG, "Discovery: number ch%u → '%s' (round-trip=%s)",
             e.channel, name.c_str(), e.report_state ? "yes" : "no");
  }

  // ---------- Compound switches --------------------------------------------
  // One HA MQTT switch that triggers a single downlink containing multiple
  // LPP digital_output fields — bundles several device-side toggles into one
  // ChirpStack queue item and one wake-cycle apply.
  for (const auto &e : compound_switches_) {
    const std::string uid    = device_id + "_compound_" + e.slug;
    const std::string topic  = discovery_prefix_ + "/switch/" + device_id + "/compound_" + e.slug + "/config";

    const std::string name    = pick(e.ha.name, e.name);
    const std::string icon    = e.ha.icon;
    const std::string ent_cat = e.ha.entity_category;

    // payload_on / payload_off — pre-baked downlink JSON strings.
    auto wrap = [&](const std::string &b64) -> std::string {
      return std::string("{\"devEui\":\"") + dev_eui_hex_ + "\""
           + ",\"confirmed\":false"
           + ",\"fPort\":" + std::to_string(e.fport)
           + ",\"data\":\"" + b64 + "\"}";
    };
    const std::string payload_on  = wrap(e.payload_on_b64);
    const std::string payload_off = wrap(e.payload_off_b64);

    // If state_from_channel >= 0, mirror that channel's digital_input echo.
    // Otherwise the switch is optimistic (HA flips immediately on press).
    std::string state_topic_block;
    if (e.state_from_channel >= 0) {
      state_topic_block =
          ",\"state_topic\":\"" + uplink_topic + "\""
          ",\"value_template\":\"{{ 'ON' if value_json.object.ch" +
              std::to_string(e.state_from_channel) +
              "_digital_input == 1 else 'OFF' }}\""
          ",\"state_on\":\"ON\""
          ",\"state_off\":\"OFF\""
          ",\"optimistic\":false";
    } else {
      state_topic_block = ",\"optimistic\":true";
    }

    std::string payload = "{\"name\":\"" + json_escape(name) + "\"";
    payload += ",\"unique_id\":\"" + uid + "\"";
    payload += ",\"command_topic\":\"" + downlink_topic + "\"";
    payload += ",\"payload_on\":\""  + json_escape(payload_on)  + "\"";
    payload += ",\"payload_off\":\"" + json_escape(payload_off) + "\"";
    payload += state_topic_block;
    json_append_str(payload, "icon",            icon);
    json_append_str(payload, "entity_category", ent_cat);
    payload += device_block;
    payload += "}";

    mqtt_client_->publish(topic, payload, 0, true);
    ESP_LOGI(TAG, "Discovery: compound switch '%s' (fport=%d, state_ch=%d)",
             name.c_str(), e.fport, e.state_from_channel);
  }

  // ---------- Diagnostic sensors --------------------------------------------
  // Pulled out of the ChirpStack uplink JSON itself, not the LPP payload.
  // HA evaluates the user-supplied Jinja2 value_template on each uplink.
  for (const auto &e : diagnostic_sensors_) {
    const std::string uid   = device_id + "_diag_" + e.slug;
    const std::string topic = discovery_prefix_ + "/sensor/" + device_id + "/diag_" + e.slug + "/config";

    std::string payload = "{\"name\":\"" + json_escape(e.name) + "\"";
    payload += ",\"unique_id\":\"" + uid + "\"";
    payload += ",\"state_topic\":\"" + uplink_topic + "\"";
    payload += ",\"value_template\":\"" + json_escape(e.value_template) + "\"";
    json_append_str(payload, "unit_of_measurement", e.unit_of_measurement);
    json_append_str(payload, "device_class",        e.device_class);
    json_append_str(payload, "state_class",         e.state_class);
    json_append_str(payload, "icon",                e.icon);
    json_append_str(payload, "entity_category",     e.entity_category);
    payload += device_block;
    payload += "}";

    mqtt_client_->publish(topic, payload, 0, true);
    ESP_LOGI(TAG, "Discovery: diagnostic '%s' → %s", e.name.c_str(), e.slug.c_str());
  }

  ESP_LOGI(TAG, "Discovery: published %u sensor / %u switch / %u compound / %u number / %u diagnostic entities",
           (unsigned) cayenne_sensors_.size(),
           (unsigned) downlink_switches_.size(),
           (unsigned) compound_switches_.size(),
           (unsigned) downlink_numbers_.size(),
           (unsigned) diagnostic_sensors_.size());
}

}  // namespace lorawan
}  // namespace esphome
