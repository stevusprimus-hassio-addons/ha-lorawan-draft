# `lorawan` external component

ESPHome external component that drives an **SX1262** LoRa radio as a **LoRaWAN 1.1 OTAA** node on **EU868**, with NVS-persisted sessions and a non-blocking background task. Built on [RadioLib](https://github.com/jgromes/RadioLib) and [CayenneLPP](https://github.com/ElectronicCats/CayenneLPP).

> ⚠️ **Pre-production code.** This component was built as an integration of an existing Arduino sketch and has not been through a formal embedded-engineering review. Before deploying to real-world hardware, have a qualified embedded/LoRaWAN engineer review the SPI bus handling, NVS write strategy, and FreeRTOS task isolation. The defaults reflect a specific board + ChirpStack setup; verify they're appropriate for yours.

---

## Features

- **OTAA 1.1 join** with persistent session storage (NVS via `Preferences`)
- **Non-blocking uplinks**: radio operations run on a dedicated FreeRTOS task; ESPHome's main loop stays responsive
- **Manual-trigger uplink/downlink cycle** — `lorawan.send:` YAML action, no implicit polling (you decide the cadence via `interval:`, `on_boot:`, deep-sleep wake, etc.)
- **Three payload formats**:
  - **CayenneLPP** (recommended) — self-describing, decodes natively in ChirpStack
  - **Raw fixed-layout** — pack sensors as `uint8`/`int16`/`uint32`/`float32` etc.
  - **Lambda** — full control, build the payload yourself in C++
- **Downlink-controlled switches and numbers**: drive ESPHome `switch` / `number` entities from CayenneLPP downlinks (Class A timing)
- **Optional state echo**: switches and numbers can mirror their state in every uplink (`report_state: true`) so HA can show the actual device state, not just commands sent
- **HA MQTT auto-discovery** — the component publishes retained discovery for every configured sensor/switch/number to your MQTT broker. The resulting HA virtual device stays available even when the ESP itself is offline. See [MQTT discovery](#mqtt-discovery).
- **Automatic join retry** with a 30-second backoff on failure
- **Human-readable error codes** in the log (e.g. `JOIN_NONCE_INVALID — replay protection; flush device in ChirpStack`)

---

## Requirements

| | |
|---|---|
| **ESPHome** | 2026.4+ (uses arduino-esp32 v3.x / ESP-IDF 5.x) |
| **RadioLib** | **≥ 7.6.0** — pinned automatically. Older releases (incl. 7.1.2) return `RADIOLIB_ERR_SPI_CMD_TIMEOUT` (−707) under arduino-esp32 v3.x even with correct wiring. |
| **CayenneLPP** | 1.6.1 (pinned) — only needed if `cayenne_sensors:` is used, but always pulled in |
| **MQTT** | only required if you use `mqtt_discovery:`. ESPHome's `mqtt:` component connecting to the same broker HA is on. |
| **Board** | Any ESP32 with a Semtech SX1262 LoRa module on VSPI |
| **Region** | EU868 (hardcoded — see Limitations) |

---

## Hardware wiring

The component uses **VSPI** (`SCK=GPIO18`, `MISO=GPIO19`, `MOSI=GPIO23`) — these are hardcoded. NSS/IRQ/RST/BUSY/TXEN/RXEN are configurable in YAML.

Example for an SX1262 breakout (v1.2) wired to a WEMOS Lolin32 Lite:

| SX1262 pin | Signal | ESP32 GPIO |
|---|---|---|
| `VCC`   | Power  | 3.3V    |
| `GND`   | Ground | GND     |
| `NSS`   | CS     | GPIO5   |
| `MOSI`  | SPI    | GPIO23  |
| `MISO`  | SPI    | GPIO19  |
| `SCK`   | SPI    | GPIO18  |
| `NRST`  | Reset  | GPIO14  |
| `BUSY`  | Busy   | GPIO27  |
| `DIO1`  | IRQ    | GPIO26  |
| `TXEN`  | TX sel | GPIO32  |
| `RXEN`  | RX sel | GPIO33  |

> Power the SX1262 from 3.3 V only. The Lolin32 Lite has it labeled on the right header next to the antenna.

---

## Minimal YAML

```yaml
external_components:
  - source:
      type: local
      path: components

# Some sensor that will be uplinked
sensor:
  - platform: wifi_signal
    id: wifi_rssi
    name: "WiFi RSSI"
    update_interval: 60s

lorawan:
  # Pin assignments (GPIO numbers)
  nss_pin:  5
  dio1_pin: 26
  rst_pin:  14
  busy_pin: 27
  txen_pin: 32
  rxen_pin: 33

  # OTAA credentials (must match the ChirpStack device profile)
  join_eui: "from chirpstack"        # 8 bytes hex
  dev_eui:  "from chirpstack"        # 8 bytes hex
  app_key:  "from chirpstack"  # 16 bytes hex

  # LoRaWAN port (1–223) — application-defined
  port: 1

  # Payload format — pick exactly ONE of `cayenne_sensors`, `sensors`, `payload_lambda`
  cayenne_sensors:
    - sensor: wifi_rssi
      channel: 1
      type: analog_input

# Trigger uplinks explicitly — no implicit polling.  Use any combo of
# on_boot, interval, button presses, automations, etc.
interval:
  - interval: 60s
    then:
      - lorawan.send:
```

For the always-available HA virtual device (recommended for anything with deep sleep):

```yaml
mqtt:
  id: mqtt_client
  broker: 192.168.x.x        # same broker HA's MQTT integration uses
  discovery: false              # don't duplicate the api:-backed entities

lorawan:
  # … other fields above …
  mqtt_discovery:
    chirpstack_app_id: "b3c186ef-41ac-4bf0-bcea-xxxxxxxxx"
    device_name: "LoRaWAN sensor"
  cayenne_sensors:
    - sensor: wifi_rssi
      channel: 1
      type: analog_input        # name/unit/device_class inherited from wifi_signal sensor

esphome:
  on_boot:
    then:
      - wait_until: { condition: mqtt.connected:, timeout: 15s }
      - lorawan.publish_discovery:    # once per boot, retained on broker
```

See the [MQTT discovery](#mqtt-discovery) section for the full story.

---

## Configuration reference

### Top-level

| Key | Type | Required | Notes |
|---|---|---|---|
| `nss_pin`, `dio1_pin`, `rst_pin`, `busy_pin`, `txen_pin`, `rxen_pin` | `int 0-39` | yes | ESP32 GPIO numbers |
| `join_eui` | `hex string` | yes | 16 hex chars (8 bytes), big-endian |
| `dev_eui`  | `hex string` | yes | 16 hex chars (8 bytes), big-endian |
| `app_key`  | `hex string` | yes | 32 hex chars (16 bytes) |
| `port`     | `int 1-223`  | yes | LoRaWAN FPort for uplinks |
| `cayenne_sensors` | list | one of | CayenneLPP payload — recommended |
| `sensors` | list | one of | Raw fixed-layout payload |
| `payload_lambda` | lambda | one of | Build the `std::vector<uint8_t>` yourself |
| `downlink_switches` | list | no | Bind incoming downlinks to ESPHome switches (see [Downlinks](#downlinks-driving-switches-from-the-network-server)) |
| `downlink_numbers` | list | no | Bind incoming downlinks to ESPHome numbers |
| `mqtt_discovery` | block | no | Publish HA MQTT discovery for every entity (see [MQTT discovery](#mqtt-discovery)) |

### `cayenne_sensors:` entries

| Key | Type | Required | Notes |
|---|---|---|---|
| `sensor`  | sensor ID | yes | Any `sensor::Sensor` ESPHome entity |
| `channel` | `int 0-255` | yes | CayenneLPP channel — unique per sensor |
| `type`    | enum | yes | LPP type (standard or custom — table below) |
| `scale`   | float | no | Default `1.0`. **Applied only to custom types** (`u16`/`u32`/`i16`/`i32`); ignored for standard CayenneLPP types whose scaling is fixed by the spec. |
| `ha`      | block | no | HA MQTT discovery overrides — see [HA metadata overrides](#ha-metadata-overrides). Only meaningful if `mqtt_discovery:` is also configured. |

#### Standard CayenneLPP types

Scaling is fixed by the CayenneLPP spec; do **not** set `scale:` on these (it has no effect).

| YAML `type` | Bytes | Scaling | Use for |
|---|---|---|---|
| `analog_input`     | 2 | signed, ÷100, ±327.67 | RSSI dBm, generic signed values |
| `analog_output`    | 2 | signed, ÷100 | Setpoints |
| `digital_input`    | 1 | 0 / 1   | Booleans |
| `digital_output`   | 1 | 0 / 1   | Booleans |
| `temperature`      | 2 | signed, ÷10 °C, ±3276.7 | Temperature in °C |
| `humidity`         | 1 | unsigned, ÷2 % | Relative humidity |
| `illuminance`      | 2 | unsigned, 1 lux | Light sensors |
| `presence`         | 1 | 0 / 1 | Motion / occupancy |
| `barometer`        | 2 | unsigned, ÷10 hPa | Pressure |
| `voltage`          | 2 | unsigned, ÷100 V | Battery, supply |
| `current`          | 2 | unsigned, ÷1000 A | Current draw |
| `percentage`       | 1 | unsigned, 1 % | Battery %, fuel level |
| `altitude`         | 2 | signed, 1 m | GPS altitude |
| `power`            | 2 | unsigned, 1 W | Active power |
| `direction`        | 2 | unsigned, 1 ° | Wind direction, heading |

#### Custom types (LPP-style framing, type bytes 0xF0+)

For values that don't fit the standard scaling/range. Wire format is the same `[channel][type][data...]` — only the **type byte** changes. The `scale:` multiplier is applied to `sensor.state` before truncating to the integer width.

| YAML `type` | Type byte | Bytes | Range (after scaling) | Notes |
|---|---|---|---|---|
| `u16` | `0xF0` | 2 | 0 – 65 535        | Big-endian unsigned |
| `u32` | `0xF1` | 4 | 0 – 4 294 967 295 | Big-endian unsigned |
| `i16` | `0xF2` | 2 | ±32 767           | Big-endian signed (two's complement) |
| `i32` | `0xF3` | 4 | ±2 147 483 647    | Big-endian signed |

Example — uptime in minutes from an ESPHome `uptime` sensor (which reports seconds):

```yaml
sensor:
  - platform: uptime
    id: device_uptime
    update_interval: 60s

lorawan:
  cayenne_sensors:
    - sensor: device_uptime
      channel: 2
      type: u16
      scale: 0.0166667    # seconds → minutes, u16 max ≈ 45 days
```

> The custom type bytes (0xF0–0xF3) are **not part of the CayenneLPP spec** — they're a project-local convention. The bundled "Cayenne LPP" codec in ChirpStack will ignore unknown types, so use the custom decoder below if you depend on these.

> Each entry uses 2 framing bytes (channel + type) + the data bytes. Plan for `≤ 12 bytes total` to stay comfortable at SF12.

### `sensors:` entries (raw mode)

| Key | Type | Required | Notes |
|---|---|---|---|
| `sensor`   | sensor ID | yes | |
| `encoding` | `uint8` \| `int16` \| `uint16` \| `int32` \| `uint32` \| `float32` | yes | All multi-byte values are **big-endian** |
| `scale`    | float | no | Default `1.0`. The sensor value is multiplied before encoding. |

The recipient must know the field layout — write a custom decoder in ChirpStack.

### `payload_lambda:`

```yaml
payload_lambda: !lambda |-
  std::vector<uint8_t> p;
  p.push_back(0xCA);
  // append id(my_sensor).state to p…
  return p;
```

Return an empty vector to skip the uplink for this cycle.

---

## Downlinks: driving switches from the network server

The component can receive downlink payloads and drive ESPHome `switch` entities from them. The downlink wire format reuses CayenneLPP framing — same `[channel][type][data…]` tuples as uplinks — so the only thing the network server has to do is queue the right bytes for the device's next downlink slot.

### Class A timing

This component runs as a **Class A** LoRaWAN node: the radio only opens RX windows during the ~1–2 s immediately after each uplink. Practical consequences:

- A downlink queued at the network server is delivered on the **next uplink** the device makes.
- Worst-case latency = `update_interval` (so 60 s by default).
- For more responsive control during testing, drop `update_interval` to e.g. `10s`. For production, design around the cadence — Class A is not a real-time channel.

### Wire format

Each downlink field is the standard CayenneLPP `digital_output` tuple — 3 bytes per switch:

| Byte | Value | Meaning |
|---|---|---|
| 0 | `0x00`–`0xFF` | LPP **channel** — matches the `channel:` in YAML |
| 1 | `0x01` | LPP type = `digital_output` |
| 2 | `0x00` / `0x01` | New switch state (any non-zero value = ON) |

Multiple switches in one downlink are allowed — just concatenate tuples. The `fPort` of the downlink doesn't matter: the parser dispatches by channel byte, not port.

> Custom LPP downlink types (`u16`/`u32`/`i16`/`i32`) are **not** decoded — `digital_output` is currently the only downlink type the component knows. Anything else gets logged as `Unhandled downlink LPP type 0xNN` and parsing aborts at that point.

### YAML

```yaml
# Any ESPHome switch — template, gpio, restart, etc. — works as the target.
switch:
  - platform: template
    name: "LoRa Test Switch"
    id: lora_test_switch
    optimistic: true     # required for template switches that don't read back hardware
    turn_on_action:
      - logger.log: "Switch turned ON via downlink"
    turn_off_action:
      - logger.log: "Switch turned OFF via downlink"

lorawan:
  # … other settings …
  downlink_switches:
    - switch: lora_test_switch
      channel: 10
```

#### `downlink_switches:` entries

| Key | Type | Required | Notes |
|---|---|---|---|
| `switch`  | switch ID | yes | Any ESPHome `switch::Switch` |
| `channel` | `int 0-255` | yes | LPP channel the device listens for |

### How it's dispatched

The blocking part of receive (RX1/RX2) already runs on the background FreeRTOS task. After each uplink:

1. Downlink bytes (if any) land in a buffer on the radio task.
2. The task parses LPP tuples and pushes matching `{switch*, on/off}` into a small queue.
3. The main loop drains that queue every iteration and calls `turn_on()` / `turn_off()` on the matched switch — so any `turn_on_action`/`turn_off_action` you defined in YAML fires normally, and the new state is published back to Home Assistant via the API.

If the network server queues a downlink for a channel that **doesn't** map to any `downlink_switches` entry, you'll see:

```
[W][lorawan]: Downlink digital_output on channel 7 → no matching switch
```

### What you'll see in the log on a successful command

```
[D][lorawan]: Sending 8-byte uplink on port 1
[I][lorawan]: Uplink OK, downlink received on port 10 (3 bytes)
[I][lorawan]: Downlink → switch 'LoRa Test Switch' = ON
[D][switch.template]: 'LoRa Test Switch' Turning ON.
[I][automation]: LoRa downlink: switch ON
```

### Encoding examples

For a switch on `channel: 10`:

| Action | Bytes | base64 | hex |
|---|---|---|---|
| Turn ON  | `0A 01 01` | `CgEB` | `0a0101` |
| Turn OFF | `0A 01 00` | `CgEA` | `0a0100` |

Two switches in one downlink (channel 10 ON, channel 11 OFF):

| Bytes | base64 |
|---|---|
| `0A 01 01 0B 01 00` | `CgEBCwEA` |

How you actually queue those bytes at the network server (REST, MQTT, CLI, web UI) is outside the scope of this README — refer to your LNS's documentation. Anything that can deliver an arbitrary-byte downlink to the device will work.

### Safety notes

- **No acknowledgement back to ESPHome.** The component doesn't track which downlinks have been applied beyond the in-process log line. If you need closed-loop control (e.g. for an actuator that mustn't be stuck on/off), design the application-level handshake yourself — for example, uplink the switch state in the next payload and let the controller compare.
- **`confirmed: false` is assumed by default.** RadioLib supports confirmed downlinks but the component doesn't currently expose the flag — there's no way for the device to ack a downlink at the MAC layer from YAML. Plumb it through if you need it.
- **Stale queued downlinks.** If the device is offline for a while and the network server has multiple queued commands, the device will receive them one per uplink cycle, in order. Don't queue rapid-fire toggles and assume they apply instantaneously.

Have a qualified embedded/safety engineer sign off on the failure modes before wiring downlink-controlled switches to anything that matters.

---

## MQTT discovery

When you add an `mqtt_discovery:` block to `lorawan:`, the component emits a `lorawan.publish_discovery:` YAML action that publishes **retained HA MQTT discovery messages** for every configured `cayenne_sensors` / `downlink_switches` / `downlink_numbers` entry. The result is a dedicated "LoRaWAN" device in Home Assistant with virtual entities that:

- **survive the ESP being offline** — they're owned by HA's MQTT integration, not by the ESPHome integration
- **get state from ChirpStack uplinks** — `state_topic` points at `chirpstack/.../event/up`, decoded by the codec (see [ChirpStack codec](#chirpstack-codec))
- **send commands directly to ChirpStack on toggle** — the device isn't involved in routing the press, only in receiving and acting on the eventual downlink

### When to use it

The use case is twofold:

1. **Battery-powered nodes that go offline.** Without MQTT discovery, all ESPHome entities become unavailable when the device sleeps with WiFi disabled — including the very switches you'd need to re-enable WiFi. With MQTT discovery, the controls stay reachable from HA, and the next time the device wakes its LoRaWAN uplink picks up whatever was queued at ChirpStack.

2. **Decoupling HA state from the ESP's transport.** The HA virtual entities update from LoRaWAN uplinks regardless of WiFi; conversely, if you toggle them in HA, the command flows over LoRaWAN (and only the device side knows whether WiFi is also up at that moment).

If you're using the ESP only as a normal always-on ESPHome device and don't care about offline reachability, you can skip the `mqtt_discovery:` block entirely — `api:` is enough.

### Configuration

```yaml
lorawan:
  # … existing pin / OTAA config …

  mqtt_discovery:
    chirpstack_app_id: "${app_id}"           # ChirpStack application UUID
    device_name: "LoRaWAN ${dev_eui}"        # HA device card title (optional)
    # discovery_prefix: homeassistant         # optional, default "homeassistant"
    # mqtt_id: mqtt_client                    # optional, only if multiple mqtt: defined
```

Requires the `mqtt:` component to be configured on the ESP, pointing at the same broker HA's MQTT integration uses. Set `discovery: false` on the `mqtt:` component to avoid HA seeing duplicate entities (the ESP's own sensors/switches are already exposed via `api:`).

### Trigger

Call the action from your `on_boot` after MQTT comes up. The publishes are retained, so subsequent boots are harmless re-publishes.

```yaml
esphome:
  on_boot:
    priority: -100
    then:
      - wait_until:
          condition:
            mqtt.connected:
          timeout: 15s
      - lorawan.publish_discovery:
      # … rest of your on_boot chain …
```

You can also call it from any other context (a button press, a restart automation, after wiping retained messages, etc.).

### HA metadata overrides

Each entity entry (sensor, switch, number) accepts an optional `ha:` sub-block whose fields override what the codegen would otherwise inherit from the referenced ESPHome entity. Empty / absent fields fall back to the entity's own values (`get_name()`, `get_unit_of_measurement()`, `get_device_class()`, etc.).

```yaml
cayenne_sensors:
  # Inherits everything — MQTT entity name/unit/device_class come from the
  # underlying uptime sensor's config.
  - sensor: device_uptime
    channel: 1
    type: u32

  # Overrides — useful when LPP scaling changes the unit.
  - sensor: device_uptime
    channel: 1
    type: u32
    scale: 0.0166667
    ha:
      name: "Uptime"
      unit_of_measurement: min
      device_class: duration
      state_class: total_increasing
```

Available override fields under `ha:`:

| Field | Applies to | Notes |
|---|---|---|
| `name` | sensor / switch / number | Display name in HA |
| `unit_of_measurement` | sensor / number | |
| `device_class` | sensor / number | |
| `state_class` | sensor | `measurement`, `total`, `total_increasing` |
| `icon` | sensor / switch / number | `mdi:…` |
| `entity_category` | all | `config`, `diagnostic` |
| `min_value`, `max_value`, `step` | number | Override the underlying number's traits (strings, so quoted) |
| `mode` | number | `box`, `slider`, `auto` |

### Round-tripped switches and numbers

Setting `report_state: true` on a `downlink_switches` / `downlink_numbers` entry makes the component **echo that entity's current state in every uplink**:

- For switches: echoed as `digital_input` (LPP type `0x00`) on the same channel as the downlink. The HA MQTT switch's `value_template` extracts `value_json.object.ch<N>_digital_input` and flips accordingly.
- For numbers: echoed as the configured `type` (u16/u32/i16/i32 etc.) on the same channel. The HA MQTT number's `state_topic` shows the actual device value, not just the last-commanded one.

Without `report_state`, the MQTT switches/numbers are command-only (HA shows them as optimistic; commands fly out, state may not catch up).

State is **strict** (not optimistic): when you toggle an MQTT switch in HA, it stays at its old state until the device's next uplink confirms the change. Worst-case latency = `sleep_duration_s` + RX windows. Plan accordingly for UX expectations.

### What gets published

For each configured entity, exactly one retained MQTT message:

| Entity | Topic |
|---|---|
| `cayenne_sensors:` entry on channel N | `<discovery_prefix>/sensor/lorawan_<dev_eui>/chN/config` |
| `downlink_switches:` entry on channel N | `<discovery_prefix>/switch/lorawan_<dev_eui>/chN/config` |
| `downlink_numbers:` entry on channel N | `<discovery_prefix>/number/lorawan_<dev_eui>/chN/config` |
| `diagnostic_sensors:` entry named "Foo Bar" | `<discovery_prefix>/sensor/lorawan_<dev_eui>/diag_foo_bar/config` |

All entities belong to one HA device with `identifiers: ["lorawan_<dev_eui>"]`, so they group cleanly in the HA UI.

### Compound switches (mode toggles)

ChirpStack queues each MQTT-delivered downlink as its own queue item, delivered FIFO one per uplink cycle. If you toggle two HA switches in quick succession, the two state changes apply across two uplink RX windows (= up to 2× `sleep_duration` worst case).

For "mode change" UX — toggling several device states atomically — a **compound switch** publishes a single downlink containing multiple LPP digital_output fields. One queue item, one wake-cycle to apply, one toggle in HA.

```yaml
mqtt_discovery:
  chirpstack_app_id: "${app_id}"
  downlink_compound_switches:
    - name: "Online mode"
      when_on:
        - { channel: 11, value: true }   # WiFi on
        - { channel: 12, value: false }  # Deep sleep off
      when_off:
        - { channel: 11, value: false }  # WiFi off
        - { channel: 12, value: true }   # Deep sleep on
      fport: 11
      state_from_channel: 11             # use WiFi state echo as toggle state
      ha:
        icon: "mdi:rocket-launch"
```

Each entry produces one HA MQTT switch under `homeassistant/switch/lorawan_<dev_eui>/compound_<slug>/config`. The `payload_on` / `payload_off` are pre-computed at codegen time — single base64 strings encoding all the channel/value pairs as one multi-field LPP payload.

| Field | Type | Required | Notes |
|---|---|---|---|
| `name` | string | yes | HA display name; slug auto-derived for the topic |
| `when_on` | list of `{channel, value}` | yes | Channels and values applied when the HA switch is toggled ON |
| `when_off` | list of `{channel, value}` | yes | Channels and values applied when toggled OFF |
| `fport` | `int 1-223` | no | LoRaWAN application port for the downlink frame. Default `1`. Device parses by LPP channel byte anyway. |
| `state_from_channel` | `int 0-255` | no | Channel whose `digital_input` echo represents the compound switch's state. Omit for an optimistic switch with no echo. |
| `ha` | block | no | HA metadata overrides (name, icon, entity_category, etc.) |

The on-device individual `downlink_switches:` are unaffected — they remain available for fine-grained control or for round-trip state mirroring. A compound switch is purely an HA-side convenience layer that bundles a downlink.

### Diagnostic sensors

Some useful information lives in the uplink event JSON itself (gateway RSSI/SNR, frame counter, gateway timestamp) — not in the LPP payload your device sends. Add a `diagnostic_sensors:` list under `mqtt_discovery:` to expose them. The `value_template` is a Jinja2 expression HA evaluates against `value_json` on each uplink.

```yaml
mqtt_discovery:
  chirpstack_app_id: "${app_id}"
  diagnostic_sensors:
    - name: "Last update"
      value_template: "{{ value_json.rxInfo[0].nsTime }}"
      device_class: timestamp
    - name: "LoRaWAN RSSI"
      value_template: "{{ value_json.rxInfo[0].rssi }}"
      unit_of_measurement: dBm
      device_class: signal_strength
      state_class: measurement
    - name: "LoRaWAN SNR"
      value_template: "{{ value_json.rxInfo[0].snr }}"
      unit_of_measurement: dB
      device_class: signal_strength
      state_class: measurement
    - name: "FCnt"
      value_template: "{{ value_json.fCnt }}"
      state_class: total_increasing
```

`entity_category` defaults to `"diagnostic"` for these entries (since that's almost always what you want). Override to `"config"` or leave it on whatever value you set if you need different behaviour. Each entry's topic slug is auto-derived from the name (lowercased, non-alphanumerics → underscore).

For number entities, the `command_template` is a pure-Jinja2 base64 encoder generated at codegen time — it produces the LPP-framed downlink JSON HA needs to send for the configured `type`/`channel`/`scale`. Look at the retained message on your broker if you want to see the exact template.

### Decommissioning

Retained discovery messages live on your broker indefinitely. To remove an entity:

1. Drop the entity from your YAML, reflash.
2. Publish an empty payload to that entity's config topic, retained:

```bash
mosquitto_pub -h <broker> -u <user> -P <pass> \
  -t 'homeassistant/switch/lorawan_61caf3eb9d8acb82/ch10/config' \
  -r -n
```

HA's MQTT integration will then drop the entity.

---

## Behaviour

### Startup

1. `setup()` initialises SPI, RadioLib, calls `beginOTAA()`, restores nonces/session from NVS if present.
2. Spawns a FreeRTOS task (`lorawan`, 4 KB stack, priority 1) and returns immediately.
3. The task calls `activateOTAA()` — blocks ~6 s but is **not subscribed to the watchdog**.
4. On success → sets `ready_=true`, persists the new session to NVS.
5. On failure → logs the symbolic error, waits 30 s, retries forever.

### Uplinks

- `update()` is called by the polling component every `update_interval`.
- It builds the payload, enqueues it (queue depth = 1), and returns in microseconds.
- The background task drains the queue, calls `sendReceive()`, logs the result, persists FCntUp to NVS.
- If a previous uplink is still in flight when a new one is enqueued, the new one is **dropped** with a warning. (Better than queuing them up and violating duty cycle.)

### NVS layout

Namespace `lorawan` in the `nvs` partition. Two keys:

| Key       | Size    | Contents |
|-----------|---------|----------|
| `nonces`  | 12 B    | DevNonce, JoinNonce, etc. — survives reboots, prevents replay rejections |
| `session` | ~250 B  | Session keys, FCntUp/Down, channel mask |

To force a fresh join, wipe the device flash or just the `nvs` partition.

---

## Common errors

The log now prints both the numeric code and a hint:

```
[E][lorawan]: activateOTAA failed: -1110 (JOIN_NONCE_INVALID — replay protection; flush device in ChirpStack)
```

Quick reference:

| Code | Symbol | Likely cause |
|------|--------|--------------|
| −2   | `CHIP_NOT_FOUND` | Wiring / power |
| −707 | `SPI_CMD_TIMEOUT` | SPI bus broken, BUSY stuck. On arduino-esp32 v3.x: make sure RadioLib ≥ 7.6.0. |
| −1105 | `NO_JOIN_ACCEPT` | Gateway out of range, wrong keys, or DevNonce rejected |
| −1110 | `JOIN_NONCE_INVALID` | Replay protection — flush device session in ChirpStack |
| −1115 | `NO_CHANNEL_AVAILABLE` | Duty cycle hit (EU868 = 1%/h per sub-band) — slow your `update_interval` down |

---

## Limitations

- **EU868 only** — `&EU868` is hardcoded. Add a region parameter if you need US915/AS923/etc.
- **VSPI pins hardcoded** (`SCK=18`, `MISO=19`, `MOSI=23`). Easy to expose if needed.
- **Single uplink in flight** — the queue is depth-1 by design; rapid `lorawan.send:` calls drop the newer one with a warning.
- **Downlinks decode: `digital_output` + numerics only** — the parser handles `0x01` (digital_output → switch) plus `analog_output` (`0x03`) and the custom `u16/u32/i16/i32` (`0xF0–0xF3`) types as numbers. Other LPP types in a downlink log `Unhandled downlink LPP type` and abort parsing at that point.
- **No downlink confirmation from YAML** — the device acts on downlinks but can't ack at the LoRaWAN MAC layer from YAML. ChirpStack's `event/txack` topic tells you the gateway transmitted; for end-to-end confirmation, use `report_state: true` and watch the next uplink's echo.
- **MQTT discovery requires a shared broker** — `lorawan.publish_discovery:` publishes to whatever broker your ESP's `mqtt:` component is connected to. If HA's MQTT integration is on a *different* broker, you need a topic bridge for `homeassistant/#` (and the ChirpStack `event/up` topic) between the two.
- **No ADR exposure** — RadioLib's default ADR behaviour is used; not configurable from YAML.
- **Deep sleep is YAML-driven** — the lorawan component itself doesn't manage deep sleep; pair it with ESPHome's `deep_sleep:` component. The join + session survive across sleeps via NVS, but cycle timing (run_duration, when to call `lorawan.send:`) is your YAML's responsibility.
- **Watchdog interaction** — the background task is not subscribed to the task watchdog. That's fine because RadioLib's own internal waits have bounded timeouts, but if you extend the task with more blocking work, audit it.

---

## ChirpStack codec

ChirpStack's built-in **Cayenne LPP** template handles the standard types but **silently ignores the custom `0xF0–0xF3` ones**. If you use any custom type, paste this codec into the Application → Codec field instead:

```javascript
function decodeUplink(input) {
  const b = input.bytes;
  const readU16 = i => (b[i] << 8) | b[i + 1];
  const readI16 = i => { const v = readU16(i); return v > 0x7FFF ? v - 0x10000 : v; };
  const readU32 = i => (b[i] * 0x1000000) + (b[i + 1] << 16) + (b[i + 2] << 8) + b[i + 3];
  const readI32 = i => { const v = readU32(i); return v > 0x7FFFFFFF ? v - 0x100000000 : v; };

  const TYPES = {
    // --- Standard CayenneLPP ---
    0x00: {name: 'digital_input',  size: 1, fn: i => b[i]},
    0x01: {name: 'digital_output', size: 1, fn: i => b[i]},
    0x02: {name: 'analog_input',   size: 2, fn: i => readI16(i) / 100},
    0x03: {name: 'analog_output',  size: 2, fn: i => readI16(i) / 100},
    0x65: {name: 'illuminance',    size: 2, fn: i => (b[i] << 8) | b[i + 1]},
    0x66: {name: 'presence',       size: 1, fn: i => b[i]},
    0x67: {name: 'temperature',    size: 2, fn: i => readI16(i) / 10},
    0x68: {name: 'humidity',       size: 1, fn: i => b[i] / 2},
    0x73: {name: 'barometer',      size: 2, fn: i => ((b[i] << 8) | b[i + 1]) / 10},
    0x74: {name: 'voltage',        size: 2, fn: i => ((b[i] << 8) | b[i + 1]) / 100},
    0x75: {name: 'current',        size: 2, fn: i => ((b[i] << 8) | b[i + 1]) / 1000},
    0x78: {name: 'percentage',     size: 1, fn: i => b[i]},
    0x79: {name: 'altitude',       size: 2, fn: i => readI16(i)},
    0x7A: {name: 'power',          size: 2, fn: i => (b[i] << 8) | b[i + 1]},
    0x7D: {name: 'direction',      size: 2, fn: i => (b[i] << 8) | b[i + 1]},
    // --- Custom (project-local, 0xF0+) ---
    0xF0: {name: 'u16',            size: 2, fn: i => readU16(i)},
    0xF1: {name: 'u32',            size: 4, fn: i => readU32(i)},
    0xF2: {name: 'i16',            size: 2, fn: i => readI16(i)},
    0xF3: {name: 'i32',            size: 4, fn: i => readI32(i)},
  };

  const out = {};
  let i = 0;
  while (i < b.length - 1) {
    const ch = b[i++];
    const typ = b[i++];
    const def = TYPES[typ];
    if (!def) {
      return { errors: [`Unknown LPP type 0x${typ.toString(16)} at offset ${i - 1}`] };
    }
    out[`ch${ch}_${def.name}`] = def.fn(i);
    i += def.size;
  }
  return { data: out };
}
```

For the example YAML in this README you'd see:

```json
{
  "ch1_analog_input": -47,
  "ch2_u16": 205
}
```

(Channel 1 = RSSI in dBm, Channel 2 = uptime in minutes.) Apply whatever post-processing you need in ChirpStack integrations or downstream (e.g. dividing `ch2_u16` by 60 if you'd rather report hours).

---

## Why a separate `SPIClass(VSPI)` instance?

ESPHome may use the global Arduino `SPI` object internally (e.g. for other SPI components) before our `setup()` runs. Using a dedicated `SPIClass lora_spi_{VSPI}` member isolates LoRaWAN from any peripheral re-init done by other components, and matches how the original Arduino sketch behaves. It costs ~80 bytes of RAM and is cheap insurance.

---

## Adding more sensors

Just add entries — they're packed into a single payload up to 51 bytes (SF12 budget):

```yaml
sensor:
  - platform: adc
    id: vbat
    pin: GPIO34
    update_interval: 60s
  - platform: dht
    pin: GPIO15
    temperature: { id: t_in }
    humidity:    { id: h_in }
    update_interval: 60s

lorawan:
  # … other settings …
  cayenne_sensors:
    - { sensor: vbat,  channel: 1, type: voltage }
    - { sensor: t_in,  channel: 2, type: temperature }
    - { sensor: h_in,  channel: 3, type: humidity }
```

Total payload size = (2 + 2) + (2 + 2) + (2 + 1) = **11 bytes**. Comfortable for SF12.
