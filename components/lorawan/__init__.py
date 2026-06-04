import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import sensor, switch, number, mqtt
from esphome.const import CONF_ID

AUTO_LOAD = ["sensor", "switch", "number"]

lorawan_ns = cg.esphome_ns.namespace("lorawan")
LoRaWANComponent  = lorawan_ns.class_("LoRaWANComponent", cg.Component)
LoRaWANSendAction = lorawan_ns.class_("LoRaWANSendAction", automation.Action)
LoRaWANPublishDiscoveryAction = lorawan_ns.class_(
    "LoRaWANPublishDiscoveryAction", automation.Action
)
HaOverrides                  = lorawan_ns.struct("HaOverrides")
DiagnosticSensorEntry        = lorawan_ns.struct("DiagnosticSensorEntry")
CompoundSwitchEntry          = lorawan_ns.struct("CompoundSwitchEntry")
DownlinkButtonEntry          = lorawan_ns.struct("DownlinkButtonEntry")
BinarySensorEntry            = lorawan_ns.struct("BinarySensorEntry")


def _slugify(name: str) -> str:
    """Derive a stable url/topic slug from a free-form display name."""
    return re.sub(r"_+", "_", re.sub(r"[^a-z0-9]+", "_", name.lower())).strip("_")

# Map YAML encoding strings to C++ enum class values emitted via RawExpression
ENCODINGS = {
    "uint8":   "UINT8",
    "int16":   "INT16",
    "uint16":  "UINT16",
    "int32":   "INT32",
    "uint32":  "UINT32",
    "float32": "FLOAT32",
}

# Subset of CayenneLPP types we expose.  Maps YAML name → C++ enum value.
# See https://docs.mydevices.com/docs/lorawan/cayenne-lpp for the full spec.
# Custom types use LPP-style framing (channel + type byte + data) but borrow
# type-byte values from the 0xF0+ range that the official spec leaves unused.
CAYENNE_TYPES = {
    # --- Standard CayenneLPP types (scaling fixed by spec, `scale:` ignored) ---
    "analog_input":     "ANALOG_INPUT",     # 2 B,  signed,   0.01 precision
    "analog_output":    "ANALOG_OUTPUT",    # 2 B,  signed,   0.01 precision
    "digital_input":    "DIGITAL_INPUT",    # 1 B,  0/1
    "digital_output":   "DIGITAL_OUTPUT",   # 1 B,  0/1
    "temperature":      "TEMPERATURE",      # 2 B,  signed,   0.1 °C
    "humidity":         "HUMIDITY",         # 1 B,  unsigned, 0.5 %
    "illuminance":      "ILLUMINANCE",      # 2 B,  unsigned, 1 lux
    "presence":         "PRESENCE",         # 1 B,  0/1
    "barometer":        "BAROMETER",        # 2 B,  unsigned, 0.1 hPa
    "voltage":          "VOLTAGE",          # 2 B,  unsigned, 0.01 V
    "current":          "CURRENT",          # 2 B,  unsigned, 0.001 A
    "percentage":       "PERCENTAGE",       # 1 B,  unsigned, 1 %
    "altitude":         "ALTITUDE",         # 2 B,  signed,   1 m
    "power":            "POWER",            # 2 B,  unsigned, 1 W
    "direction":        "DIRECTION",        # 2 B,  unsigned, 1 °
    # --- Custom types (LPP framing, type byte 0xF0+; `scale:` is applied) ---
    "u16":              "U16",              # type 0xF0, 2 B  unsigned, big-endian
    "u32":              "U32",              # type 0xF1, 4 B  unsigned, big-endian
    "i16":              "I16",              # type 0xF2, 2 B  signed,   big-endian
    "i32":              "I32",              # type 0xF3, 4 B  signed,   big-endian
}

CONF_NSS_PIN    = "nss_pin"
CONF_DIO1_PIN   = "dio1_pin"
CONF_RST_PIN    = "rst_pin"
CONF_BUSY_PIN   = "busy_pin"
CONF_TXEN_PIN   = "txen_pin"
CONF_RXEN_PIN   = "rxen_pin"
CONF_JOIN_EUI   = "join_eui"
CONF_DEV_EUI    = "dev_eui"
CONF_APP_KEY    = "app_key"
CONF_PORT       = "port"
CONF_SENSORS    = "sensors"
CONF_CAYENNE_SENSORS = "cayenne_sensors"
CONF_DOWNLINK_SWITCHES = "downlink_switches"
CONF_DOWNLINK_NUMBERS  = "downlink_numbers"
CONF_SENSOR     = "sensor"
CONF_SWITCH     = "switch"
CONF_NUMBER     = "number"
CONF_ENCODING   = "encoding"
CONF_SCALE      = "scale"
CONF_CHANNEL    = "channel"
CONF_TYPE       = "type"
CONF_PAYLOAD_LAMBDA = "payload_lambda"
CONF_REPORT_STATE   = "report_state"

# Top-level MQTT discovery block.
CONF_MQTT_DISCOVERY    = "mqtt_discovery"
CONF_CHIRPSTACK_APP_ID = "chirpstack_app_id"
CONF_DEVICE_NAME       = "device_name"
CONF_DISCOVERY_PREFIX  = "discovery_prefix"
CONF_MQTT_ID           = "mqtt_id"

# Per-entity HA discovery override sub-block.
CONF_HA                  = "ha"
CONF_NAME                = "name"
CONF_UNIT_OF_MEASUREMENT = "unit_of_measurement"
CONF_DEVICE_CLASS        = "device_class"
CONF_STATE_CLASS         = "state_class"
CONF_ICON                = "icon"
CONF_ENTITY_CATEGORY     = "entity_category"
CONF_MIN_VALUE           = "min_value"
CONF_MAX_VALUE           = "max_value"
CONF_STEP                = "step"
CONF_MODE                = "mode"
CONF_VALUE_TEMPLATE      = "value_template"
CONF_DIAGNOSTIC_SENSORS  = "diagnostic_sensors"
CONF_DOWNLINK_COMPOUND_SWITCHES = "downlink_compound_switches"
CONF_WHEN_ON                    = "when_on"
CONF_WHEN_OFF                   = "when_off"
CONF_VALUE                      = "value"
CONF_FPORT                      = "fport"
CONF_STATE_FROM_CHANNEL         = "state_from_channel"
CONF_INCLUDE_NUMBER_CHANNELS    = "include_number_channels"
CONF_HA_DISCOVERY               = "ha_discovery"
CONF_DOWNLINK_BUTTONS           = "downlink_buttons"
CONF_WHEN_PRESSED               = "when_pressed"
CONF_BINARY_SENSORS             = "binary_sensors"
CONF_DIAGNOSTIC_BINARY_SENSORS  = "diagnostic_binary_sensors"

HA_OVERRIDES_SCHEMA = cv.Schema({
    cv.Optional(CONF_NAME):                cv.string,
    cv.Optional(CONF_UNIT_OF_MEASUREMENT): cv.string,
    cv.Optional(CONF_DEVICE_CLASS):        cv.string,
    cv.Optional(CONF_STATE_CLASS):         cv.string,
    cv.Optional(CONF_ICON):                cv.icon,
    cv.Optional(CONF_ENTITY_CATEGORY):     cv.string,
    cv.Optional(CONF_MIN_VALUE):           cv.string,
    cv.Optional(CONF_MAX_VALUE):           cv.string,
    cv.Optional(CONF_STEP):                cv.string,
    cv.Optional(CONF_MODE):                cv.string,
})


def _ha_struct(ha_conf):
    """Build a HaOverrides initializer (RawExpression) from a YAML `ha:` dict."""
    if ha_conf is None:
        ha_conf = {}
    fields = []
    for key in (CONF_NAME, CONF_UNIT_OF_MEASUREMENT, CONF_DEVICE_CLASS,
                CONF_STATE_CLASS, CONF_ICON, CONF_ENTITY_CATEGORY,
                CONF_MIN_VALUE, CONF_MAX_VALUE, CONF_STEP, CONF_MODE):
        value = str(ha_conf.get(key, "")).replace("\\", "\\\\").replace('"', '\\"')
        fields.append(f'"{value}"')
    return cg.RawExpression("lorawan::HaOverrides{" + ", ".join(fields) + "}")

# Subset of CayenneLPP types decodable on the downlink path for numeric values.
DOWNLINK_NUMBER_TYPES = {
    "analog_output": "ANALOG_OUTPUT",  # std LPP: 2 B signed, ÷100 (max ±327.67)
    "u16":           "U16",            # custom 0xF0, 2 B unsigned (0..65 535)
    "u32":           "U32",            # custom 0xF1, 4 B unsigned
    "i16":           "I16",            # custom 0xF2, 2 B signed
    "i32":           "I32",            # custom 0xF3, 4 B signed
}


def _validate_eui64(value):
    value = cv.string(value).lower().replace("0x", "").replace(":", "")
    if len(value) != 16:
        raise cv.Invalid(f"EUI-64 must be 16 hex chars (got {len(value)})")
    try:
        int(value, 16)
    except ValueError:
        raise cv.Invalid(f"EUI-64 contains non-hex characters: {value!r}")
    return value


def _validate_key16(value):
    value = cv.string(value).lower().replace("0x", "").replace(":", "")
    if len(value) != 32:
        raise cv.Invalid(f"Key must be 32 hex chars (16 bytes, got {len(value)})")
    try:
        int(value, 16)
    except ValueError:
        raise cv.Invalid(f"Key contains non-hex characters: {value!r}")
    return value.upper()


SENSOR_SCHEMA = cv.Schema({
    cv.Required(CONF_SENSOR):   cv.use_id(sensor.Sensor),
    cv.Required(CONF_ENCODING): cv.enum(ENCODINGS, lower=True),
    cv.Optional(CONF_SCALE, default=1.0): cv.float_,
})

CAYENNE_SENSOR_SCHEMA = cv.Schema({
    cv.Required(CONF_SENSOR):  cv.use_id(sensor.Sensor),
    cv.Required(CONF_CHANNEL): cv.int_range(min=0, max=255),
    cv.Required(CONF_TYPE):    cv.enum(CAYENNE_TYPES, lower=True),
    # `scale` applies only to the custom types (u16/u32/i16/i32); for standard
    # CayenneLPP types the scaling is fixed by the spec and this value is ignored.
    cv.Optional(CONF_SCALE, default=1.0): cv.float_,
    # HA discovery overrides — present fields override the inherited values
    # from the referenced ESPHome sensor.
    cv.Optional(CONF_HA): HA_OVERRIDES_SCHEMA,
})

# Bind an existing ESPHome switch to an LPP channel.  When the device receives
# a downlink with `[channel][0x01 digital_output][value]`, the switch is driven
# on/off (turn_on()/turn_off() — so any actions you defined in YAML fire too).
# `report_state: true` echoes the switch state in every uplink as digital_input
# on the same channel, enabling round-tripped MQTT switch UIs in HA.
DOWNLINK_SWITCH_SCHEMA = cv.Schema({
    cv.Required(CONF_SWITCH):  cv.use_id(switch.Switch),
    cv.Required(CONF_CHANNEL): cv.int_range(min=0, max=255),
    cv.Optional(CONF_REPORT_STATE, default=False): cv.boolean,
    cv.Optional(CONF_HA): HA_OVERRIDES_SCHEMA,
    # Set false to keep downlink processing but suppress HA MQTT discovery.
    cv.Optional(CONF_HA_DISCOVERY, default=True): cv.boolean,
})

# Bind an existing ESPHome number to an LPP channel.  Picks the wire type so
# the parser knows how many bytes to consume.  `scale` is applied to the raw
# wire integer to get the final number value (wire_int * scale → number.state).
# `report_state: true` echoes the current value in every uplink using `type`.
DOWNLINK_NUMBER_SCHEMA = cv.Schema({
    cv.Required(CONF_NUMBER):  cv.use_id(number.Number),
    cv.Required(CONF_CHANNEL): cv.int_range(min=0, max=255),
    cv.Required(CONF_TYPE):    cv.enum(DOWNLINK_NUMBER_TYPES, lower=True),
    cv.Optional(CONF_SCALE, default=1.0): cv.float_,
    cv.Optional(CONF_REPORT_STATE, default=False): cv.boolean,
    cv.Optional(CONF_HA): HA_OVERRIDES_SCHEMA,
    # Set false to keep downlink processing but suppress HA MQTT discovery.
    cv.Optional(CONF_HA_DISCOVERY, default=True): cv.boolean,
})

# An (channel, value) tuple in a compound switch's on/off action list.
COMPOUND_ACTION_SCHEMA = cv.Schema({
    cv.Required(CONF_CHANNEL): cv.int_range(min=0, max=255),
    cv.Required(CONF_VALUE):   cv.boolean,
})

# Compound switch — one HA MQTT switch, one downlink, multiple LPP fields.
# `when_on` / `when_off` are lists of (channel, value) tuples; each tuple
# becomes one LPP digital_output field in the resulting downlink payload.
DOWNLINK_COMPOUND_SWITCH_SCHEMA = cv.Schema({
    cv.Required(CONF_NAME):     cv.string,
    cv.Required(CONF_WHEN_ON):  cv.ensure_list(COMPOUND_ACTION_SCHEMA),
    cv.Required(CONF_WHEN_OFF): cv.ensure_list(COMPOUND_ACTION_SCHEMA),
    # Channels whose current downlink_number state is appended to the payload
    # at publish_discovery time.  Useful to bundle a number setting (e.g. sleep
    # duration) into the same downlink as the boolean switch fields.
    cv.Optional(CONF_INCLUDE_NUMBER_CHANNELS, default=[]): cv.ensure_list(
        cv.int_range(min=0, max=255)
    ),
    cv.Optional(CONF_FPORT, default=1): cv.int_range(min=1, max=223),
    # Optional: which channel's digital_input echo represents this switch's state.
    # Omit for an optimistic switch with no state feedback.
    cv.Optional(CONF_STATE_FROM_CHANNEL): cv.int_range(min=0, max=255),
    cv.Optional(CONF_HA): HA_OVERRIDES_SCHEMA,
})


# One-shot downlink button.  when_pressed encodes digital_output LPP fields;
# include_number_channels appends current number states at publish_discovery time.
DOWNLINK_BUTTON_SCHEMA = cv.Schema({
    cv.Required(CONF_NAME):        cv.string,
    cv.Required(CONF_WHEN_PRESSED): cv.ensure_list(COMPOUND_ACTION_SCHEMA),
    cv.Optional(CONF_INCLUDE_NUMBER_CHANNELS, default=[]): cv.ensure_list(
        cv.int_range(min=0, max=255)
    ),
    cv.Optional(CONF_FPORT, default=1): cv.int_range(min=1, max=223),
    cv.Optional(CONF_HA): HA_OVERRIDES_SCHEMA,
})

BINARY_SENSOR_SCHEMA = cv.Schema({
    cv.Required(CONF_NAME):            cv.string,
    cv.Required(CONF_VALUE_TEMPLATE):  cv.string,
    cv.Optional(CONF_DEVICE_CLASS):    cv.string,
    cv.Optional(CONF_ICON):            cv.icon,
    cv.Optional(CONF_ENTITY_CATEGORY): cv.string,
})

# Binary sensor derived from the ChirpStack uplink JSON.
# value_template must evaluate to "ON" or "OFF".
# Defaults to entity_category: diagnostic so HA places them under Diagnostics.
DIAGNOSTIC_BINARY_SENSOR_SCHEMA = cv.Schema({
    cv.Required(CONF_NAME):            cv.string,
    cv.Required(CONF_VALUE_TEMPLATE):  cv.string,
    cv.Optional(CONF_DEVICE_CLASS):    cv.string,
    cv.Optional(CONF_ICON):            cv.icon,
    cv.Optional(CONF_ENTITY_CATEGORY, default="diagnostic"): cv.string,
})


def _compound_lpp_bytes_expr(actions):
    """Encode a list of {channel, value} into a C++ std::vector<uint8_t> RawExpression."""
    raw = bytearray()
    for a in actions:
        raw.append(int(a[CONF_CHANNEL]) & 0xFF)
        raw.append(0x01)                            # LPP digital_output
        raw.append(0x01 if a[CONF_VALUE] else 0x00)
    return cg.RawExpression(
        "std::vector<uint8_t>{" + ", ".join(f"0x{b:02X}" for b in raw) + "}"
    )


# Extra MQTT diagnostic sensors whose value comes out of the uplink event JSON
# itself (rxInfo, gatewayId, fCnt, ...), not the LPP payload.  The `value_template`
# is a Jinja2 expression HA evaluates on each uplink against `value_json`.
DIAGNOSTIC_SENSOR_SCHEMA = cv.Schema({
    cv.Required(CONF_NAME):                cv.string,
    cv.Required(CONF_VALUE_TEMPLATE):      cv.string,
    cv.Optional(CONF_UNIT_OF_MEASUREMENT): cv.string,
    cv.Optional(CONF_DEVICE_CLASS):        cv.string,
    cv.Optional(CONF_STATE_CLASS):         cv.string,
    cv.Optional(CONF_ICON):                cv.icon,
    cv.Optional(CONF_ENTITY_CATEGORY, default="diagnostic"): cv.string,
})

# Top-level MQTT discovery configuration.  When present, the component will
# publish HA MQTT discovery for every configured entity when you call the
# `lorawan.publish_discovery:` action.  Requires the `mqtt:` component.
MQTT_DISCOVERY_SCHEMA = cv.Schema({
    cv.Required(CONF_CHIRPSTACK_APP_ID): cv.string,
    cv.Optional(CONF_DEVICE_NAME):       cv.string,
    cv.Optional(CONF_DISCOVERY_PREFIX, default="homeassistant"): cv.string,
    cv.Optional(CONF_MQTT_ID):           cv.use_id(mqtt.MQTTClientComponent),
    cv.Optional(CONF_DIAGNOSTIC_SENSORS): cv.ensure_list(DIAGNOSTIC_SENSOR_SCHEMA),
    cv.Optional(CONF_DOWNLINK_COMPOUND_SWITCHES): cv.ensure_list(DOWNLINK_COMPOUND_SWITCH_SCHEMA),
    cv.Optional(CONF_DOWNLINK_BUTTONS): cv.ensure_list(DOWNLINK_BUTTON_SCHEMA),
    cv.Optional(CONF_BINARY_SENSORS): cv.ensure_list(BINARY_SENSOR_SCHEMA),
    cv.Optional(CONF_DIAGNOSTIC_BINARY_SENSORS): cv.ensure_list(DIAGNOSTIC_BINARY_SENSOR_SCHEMA),
})

def _validate_payload_config(config):
    sources = [k for k in (CONF_PAYLOAD_LAMBDA, CONF_SENSORS, CONF_CAYENNE_SENSORS) if k in config]
    if len(sources) > 1:
        raise cv.Invalid(
            "Only one payload source allowed: choose one of "
            "'sensors' (raw encoding), 'cayenne_sensors' (CayenneLPP), or 'payload_lambda' — "
            f"got {sources}"
        )
    if not sources:
        raise cv.Invalid("Must specify one of 'sensors', 'cayenne_sensors', or 'payload_lambda'")
    return config

CONFIG_SCHEMA = cv.All(
    cv.Schema({
        cv.GenerateID():              cv.declare_id(LoRaWANComponent),
        cv.Required(CONF_NSS_PIN):   cv.int_range(min=0, max=39),
        cv.Required(CONF_DIO1_PIN):  cv.int_range(min=0, max=39),
        cv.Required(CONF_RST_PIN):   cv.int_range(min=0, max=39),
        cv.Required(CONF_BUSY_PIN):  cv.int_range(min=0, max=39),
        cv.Required(CONF_TXEN_PIN):  cv.int_range(min=0, max=39),
        cv.Required(CONF_RXEN_PIN):  cv.int_range(min=0, max=39),
        cv.Required(CONF_JOIN_EUI):  _validate_eui64,
        cv.Required(CONF_DEV_EUI):   _validate_eui64,
        cv.Required(CONF_APP_KEY):   _validate_key16,
        cv.Required(CONF_PORT):      cv.int_range(min=1, max=223),
        cv.Optional(CONF_SENSORS):           cv.ensure_list(SENSOR_SCHEMA),
        cv.Optional(CONF_CAYENNE_SENSORS):   cv.ensure_list(CAYENNE_SENSOR_SCHEMA),
        cv.Optional(CONF_DOWNLINK_SWITCHES): cv.ensure_list(DOWNLINK_SWITCH_SCHEMA),
        cv.Optional(CONF_DOWNLINK_NUMBERS):  cv.ensure_list(DOWNLINK_NUMBER_SCHEMA),
        cv.Optional(CONF_PAYLOAD_LAMBDA):    cv.lambda_,
        cv.Optional(CONF_MQTT_DISCOVERY):    MQTT_DISCOVERY_SCHEMA,
    }).extend(cv.COMPONENT_SCHEMA),
    _validate_payload_config,
)


# YAML action: `lorawan.send` — triggers one uplink+downlink cycle.
LORAWAN_SEND_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.use_id(LoRaWANComponent),
})

@automation.register_action("lorawan.send", LoRaWANSendAction, LORAWAN_SEND_SCHEMA)
async def lorawan_send_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


# YAML action: `lorawan.publish_discovery` — publish HA MQTT discovery for
# every configured cayenne_sensor / downlink_switch / downlink_number.
LORAWAN_PUBLISH_DISCOVERY_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.use_id(LoRaWANComponent),
})

@automation.register_action("lorawan.publish_discovery", LoRaWANPublishDiscoveryAction,
                            LORAWAN_PUBLISH_DISCOVERY_SCHEMA)
async def lorawan_publish_discovery_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # RadioLib 7.6.0+ is required when building under ESPHome 2026.4+ (arduino-esp32
    # v3.x / ESP-IDF 5.x).  Earlier RadioLib releases (incl. 7.1.2) fail with
    # RADIOLIB_ERR_SPI_CMD_TIMEOUT (-707) on that framework even when wiring is
    # correct and SPI itself works.
    cg.add_library("jgromes/RadioLib", "7.6.0")
    cg.add_library("electroniccats/CayenneLPP", "1.6.1")  # self-describing payload codec
    cg.add_library("SPI", None)         # Arduino built-in, needed by RadioLib on ESP32
    cg.add_library("Preferences", None)  # Arduino ESP32 built-in, needed for NVS

    cg.add(var.set_nss_pin(config[CONF_NSS_PIN]))
    cg.add(var.set_dio1_pin(config[CONF_DIO1_PIN]))
    cg.add(var.set_rst_pin(config[CONF_RST_PIN]))
    cg.add(var.set_busy_pin(config[CONF_BUSY_PIN]))
    cg.add(var.set_txen_pin(config[CONF_TXEN_PIN]))
    cg.add(var.set_rxen_pin(config[CONF_RXEN_PIN]))

    cg.add(var.set_join_eui(cg.RawExpression(f"0x{config[CONF_JOIN_EUI]}ULL")))
    cg.add(var.set_dev_eui(cg.RawExpression(f"0x{config[CONF_DEV_EUI]}ULL")))

    key_bytes = bytes.fromhex(config[CONF_APP_KEY])
    key_expr = cg.RawExpression(
        "std::vector<uint8_t>{" + ", ".join(f"0x{b:02X}" for b in key_bytes) + "}"
    )
    cg.add(var.set_app_key(key_expr))

    cg.add(var.set_port(config[CONF_PORT]))

    if CONF_PAYLOAD_LAMBDA in config:
        lambda_ = await cg.process_lambda(
            config[CONF_PAYLOAD_LAMBDA],
            [],
            return_type=cg.std_vector.template(cg.uint8),
        )
        cg.add(var.set_payload_lambda(lambda_))
    elif CONF_SENSORS in config:
        for s_conf in config[CONF_SENSORS]:
            sens = await cg.get_variable(s_conf[CONF_SENSOR])
            enc = cg.RawExpression(f"lorawan::Encoding::{s_conf[CONF_ENCODING].upper()}")
            cg.add(var.add_sensor(sens, enc, s_conf[CONF_SCALE]))
    elif CONF_CAYENNE_SENSORS in config:
        for s_conf in config[CONF_CAYENNE_SENSORS]:
            sens = await cg.get_variable(s_conf[CONF_SENSOR])
            cayenne_type = cg.RawExpression(
                f"lorawan::CayenneType::{s_conf[CONF_TYPE].upper()}"
            )
            cg.add(var.add_cayenne_sensor(
                sens, s_conf[CONF_CHANNEL], cayenne_type, s_conf[CONF_SCALE],
                _ha_struct(s_conf.get(CONF_HA)),
            ))

    # Downlink-controlled switches are independent of the uplink payload format.
    for sw_conf in config.get(CONF_DOWNLINK_SWITCHES, []):
        sw = await cg.get_variable(sw_conf[CONF_SWITCH])
        cg.add(var.add_downlink_switch(
            sw, sw_conf[CONF_CHANNEL], sw_conf[CONF_REPORT_STATE],
            _ha_struct(sw_conf.get(CONF_HA)),
            sw_conf.get(CONF_HA_DISCOVERY, True),
        ))

    # Downlink-controlled numbers (analog_output / u16 / u32 / i16 / i32).
    for n_conf in config.get(CONF_DOWNLINK_NUMBERS, []):
        num = await cg.get_variable(n_conf[CONF_NUMBER])
        ctype = cg.RawExpression(f"lorawan::CayenneType::{n_conf[CONF_TYPE].upper()}")
        cg.add(var.add_downlink_number(
            num, n_conf[CONF_CHANNEL], ctype, n_conf[CONF_SCALE],
            n_conf[CONF_REPORT_STATE],
            _ha_struct(n_conf.get(CONF_HA)),
            n_conf.get(CONF_HA_DISCOVERY, True),
        ))

    # MQTT discovery configuration — wires up the publish_discovery() action.
    if CONF_MQTT_DISCOVERY in config:
        d_conf = config[CONF_MQTT_DISCOVERY]
        cg.add(var.set_chirpstack_app_id(d_conf[CONF_CHIRPSTACK_APP_ID]))
        cg.add(var.set_discovery_prefix(d_conf[CONF_DISCOVERY_PREFIX]))
        cg.add(var.set_dev_eui_hex(config[CONF_DEV_EUI]))
        if CONF_DEVICE_NAME in d_conf:
            cg.add(var.set_device_name(d_conf[CONF_DEVICE_NAME]))
        # Look up the MQTT client to publish through.  If not explicitly given,
        # use the first (typically only) MQTT client in the YAML.
        if CONF_MQTT_ID in d_conf:
            mqtt_var = await cg.get_variable(d_conf[CONF_MQTT_ID])
        else:
            mqtt_var = cg.RawExpression("mqtt::global_mqtt_client")
        cg.add(var.set_mqtt_client(mqtt_var))

        # Compound switches — one downlink with multiple LPP fields per toggle.
        for c_conf in d_conf.get(CONF_DOWNLINK_COMPOUND_SWITCHES, []):
            slug = _slugify(c_conf[CONF_NAME])
            include_channels = c_conf.get(CONF_INCLUDE_NUMBER_CHANNELS, [])
            include_expr = cg.RawExpression(
                "std::vector<uint8_t>{"
                + ", ".join(str(ch) for ch in include_channels)
                + "}"
            )
            entry = cg.StructInitializer(
                CompoundSwitchEntry,
                ("slug",                    slug),
                ("name",                    c_conf[CONF_NAME]),
                ("static_bytes_on",         _compound_lpp_bytes_expr(c_conf[CONF_WHEN_ON])),
                ("static_bytes_off",        _compound_lpp_bytes_expr(c_conf[CONF_WHEN_OFF])),
                ("include_number_channels", include_expr),
                ("fport",                   c_conf[CONF_FPORT]),
                ("state_from_channel",      c_conf.get(CONF_STATE_FROM_CHANNEL, -1)),
                ("ha",                      _ha_struct(c_conf.get(CONF_HA))),
            )
            cg.add(var.add_compound_switch(entry))

        # Buttons — one-shot downlinks, no state sync.
        for b_conf in d_conf.get(CONF_DOWNLINK_BUTTONS, []):
            slug = _slugify(b_conf[CONF_NAME])
            include_channels = b_conf.get(CONF_INCLUDE_NUMBER_CHANNELS, [])
            include_expr = cg.RawExpression(
                "std::vector<uint8_t>{"
                + ", ".join(str(ch) for ch in include_channels)
                + "}"
            )
            entry = cg.StructInitializer(
                DownlinkButtonEntry,
                ("slug",                    slug),
                ("name",                    b_conf[CONF_NAME]),
                ("static_bytes",            _compound_lpp_bytes_expr(b_conf[CONF_WHEN_PRESSED])),
                ("include_number_channels", include_expr),
                ("fport",                   b_conf[CONF_FPORT]),
                ("ha",                      _ha_struct(b_conf.get(CONF_HA))),
            )
            cg.add(var.add_downlink_button(entry))

        # Normal binary sensors — state from uplink JSON, not LPP payload.
        for bs_conf in d_conf.get(CONF_BINARY_SENSORS, []):
            slug = _slugify(bs_conf[CONF_NAME])
            entry = cg.StructInitializer(
                BinarySensorEntry,
                ("slug",            slug),
                ("name",            bs_conf[CONF_NAME]),
                ("value_template",  bs_conf[CONF_VALUE_TEMPLATE]),
                ("device_class",    bs_conf.get(CONF_DEVICE_CLASS, "")),
                ("icon",            bs_conf.get(CONF_ICON, "")),
                ("entity_category", bs_conf.get(CONF_ENTITY_CATEGORY, "")),
            )
            cg.add(var.add_binary_sensor(entry))

        # Diagnostic binary sensors — state from uplink JSON, not LPP payload.
        for bs_conf in d_conf.get(CONF_DIAGNOSTIC_BINARY_SENSORS, []):
            slug = _slugify(bs_conf[CONF_NAME])
            entry = cg.StructInitializer(
                BinarySensorEntry,
                ("slug",            slug),
                ("name",            bs_conf[CONF_NAME]),
                ("value_template",  bs_conf[CONF_VALUE_TEMPLATE]),
                ("device_class",    bs_conf.get(CONF_DEVICE_CLASS, "")),
                ("icon",            bs_conf.get(CONF_ICON, "")),
                ("entity_category", bs_conf.get(CONF_ENTITY_CATEGORY, "")),
            )
            cg.add(var.add_diagnostic_binary_sensor(entry))

        # Diagnostic sensors — pulled from the uplink event JSON itself.
        for diag in d_conf.get(CONF_DIAGNOSTIC_SENSORS, []):
            slug = _slugify(diag[CONF_NAME])
            entry = cg.StructInitializer(
                DiagnosticSensorEntry,
                ("slug",                slug),
                ("name",                diag[CONF_NAME]),
                ("value_template",      diag[CONF_VALUE_TEMPLATE]),
                ("unit_of_measurement", diag.get(CONF_UNIT_OF_MEASUREMENT, "")),
                ("device_class",        diag.get(CONF_DEVICE_CLASS, "")),
                ("state_class",         diag.get(CONF_STATE_CLASS, "")),
                ("icon",                diag.get(CONF_ICON, "")),
                ("entity_category",     diag[CONF_ENTITY_CATEGORY]),
            )
            cg.add(var.add_diagnostic_sensor(entry))
