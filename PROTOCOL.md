# App protocol

How apps talk to the board: the Flamekeeper phone apps (iOS, Android), and the board's own web
page in a browser on the home network (the same app, built for the browser and served by the
board). The same records and commands travel over two transports:

| Transport | Used by | Carries |
|---|---|---|
| Bluetooth LE, one GATT service | the phone apps near the heater, and the first setup | each record in its own characteristic |
| Wi-Fi, one WebSocket on the board | the phone apps on the same network, the web page, the internet relay | each record in an encrypted binary frame, behind a one-byte record type |

So there is one set of record layouts, opcodes and result codes, one command handler
(`esp32/components/flamekeeper/ble_protocol.cpp`) and one encoder; only the transport code
differs. A phone paired once over Bluetooth also gets its Wi-Fi key over that link, so it switches
between the two without the user entering anything again. A browser signs in with the web
password the user chose during setup. The internet relay forwards the Wi-Fi frames unchanged:
they are encrypted end to end between the app and the board.

Home Assistant is the third way in, off until the user turns it on from an app. It uses ESPHome's
native API and its entities, which are thin wrappers over the same controller calls, so it can do
the same things with the same checks.

The app cannot tell which heater backend is behind the board, only what that heater can do
(capability bits) and which values it reports (unknown values have a fixed marker).

A new board has Wi-Fi and Home Assistant switched off. The user turns them on from the app over
Bluetooth; there is no access point, captive portal or Improv.

The layouts are in `esp32/components/flamekeeper/ble_protocol.h` and `.cpp`, which stay the code's
own truth, and `esp32/components/flamekeeper/tests/dump_vectors.cpp` prints their exact bytes as
JSON (records, results, a Wi-Fi sign-in and encrypted frames, the relay's keys, pushes and live
records) for a client's decoders to be tested against.

## Principles

1. **An app is one more input, not a second controller.** Every command becomes the same call a
   Home Assistant entity makes on the shared controller (`Controller::request(command, source)`),
   so it meets the same checks, is refused for the same reasons, and Home Assistant sees the
   resulting state at once. Neither transport talks to a heater backend.
2. **Nobody in radio range or on the network may run a fuel-burning heater without having been let
   in.** Everything except the Info read needs a bonded phone over an encrypted, authenticated BLE
   link (LE Secure Connections with MITM protection), or an app or browser signed in over Wi-Fi
   with a key it got from the board. A new phone needs the board's PIN and an open pairing window;
   a new browser needs the web password.
3. **No transport weakens a safety behaviour.** Stop-on-restart, starts only from off and the
   priming limits all stay in the controller and the safety MCU and apply whatever the source. The
   board is always in control of its heater: there is no other controller on the line. A dropped
   BLE link or WebSocket changes nothing on the heater. The Home Assistant failsafe covers runs
   started from Home Assistant ([below](#home-assistant-failsafe)).
4. **Small, fixed binary records, little-endian.** One read or notification carries a whole
   record.

## Versioning

Info byte 0 is the protocol major (1). Until the apps' first public release there is no backward
compatibility: layouts, opcodes and codes change in place when a cleaner design needs it, and the
apps change with the firmware. From that release, new fields go only on the end of a record and
anything older apps cannot ignore is a new major; an app that does not know the major shows
"update the app" and sends nothing (the service UUID stays the same so it can say so). The
WebSocket path `/api/v1` and subprotocol `flamekeeper.v1` are fixed names.

## Encoding

Little-endian. Fixed-point values are integers with the scale given. "Unknown or not reported by
this heater" is `0x8000` for `i16`, `0xFFFF` for `u16`, `0xFF` for `u8`, `0x80000000` for `i32`,
`0xFFFFFFFF` for `u32`. Temperatures are always °C (the app converts for display).

## Bluetooth LE

### Advertising

| | Content |
|---|---|
| Advertisement | Flags; the Flamekeeper service UUID |
| Scan response | The device name (the ESPHome `name`) |
| Connections | Up to 3 at once; advertising continues while a slot is free |

Advertising is ESPHome's shared one, so other services may add their own UUIDs to it. Apps find
boards by the Flamekeeper UUID; whether pairing is open is in Info, readable without pairing.

A connection that is not encrypted with a bond after 30 s is dropped (unless it is pairing inside
the window), so strangers cannot hold the slots.

### GATT layout

Base UUID `81ceXXXX-024c-414c-9ca7-e0972fed52ee` (random, owned by this project).

| UUID (`81ceXXXX`) | Name | Properties | Security |
|---|---|---|---|
| `1000` | **Flamekeeper service** (primary) | | |
| `1001` | Info | read | none |
| `1002` | State | read, notify | encrypted + authenticated, bonded |
| `1003` | Telemetry | read, notify | encrypted + authenticated, bonded |
| `1004` | Command | write (with response) | encrypted + authenticated, bonded |
| `1005` | Result | indicate | encrypted + authenticated, bonded |
| `1006` | Update data | write without response | encrypted + authenticated, bonded ([Board firmware updates](#board-firmware-updates)) |

Also present: GAP (0x1800; Device Name = ESPHome `friendly_name`), GATT (0x1801, with Service
Changed), Device Information (0x180A, unencrypted: Manufacturer "Flamekeeper", Model Number
"Flamekeeper rev A2", Firmware Revision = the ESPHome project version and build date).

Access without the required security fails with the standard ATT error (Insufficient
Authentication `0x05` or Insufficient Encryption `0x0F`); a phone's OS then starts pairing by
itself, which succeeds only inside the pairing window.

**MTU.** Records are up to 132 bytes (a result or event; State and Telemetry are shorter) and
notifications are not split; a command is up to 132 bytes (`WEB_PASSWORD_SET` with two 64-byte
passwords) and must go in one write (long writes are refused). So the app must negotiate an ATT
MTU of at least 135 before subscribing (iOS does this itself; Android calls `requestMtu(247)`).
The board offers 247. With a smaller MTU, notifications are cut by the stack; reads still work
through Read Blob.

**A record that grows grows its GATT maximum.** Each stack-answered characteristic (Info, State,
Telemetry) is declared with its record's size as its maximum (`flamekeeper_ble.cpp`,
`build_table_`), never a literal: Bluedroid refuses a characteristic whose value is longer than its
declared maximum, the whole table then fails to build, and no phone sees the service. The build
also checks every record against the stack's attribute limit (517) and every notified record
against one notification at the offered MTU (244 bytes).

### Info (`1001`)

Readable by anyone in range. 14 bytes.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | protocol major (1) |
| 1 | u8 | board: 2 Flamekeeper rev A2; other codes other boards |
| 2 | u8 | flags: bit 0 pairing window open; bit 1 the PIN was changed by the user; bit 2 a web password is set; bits 3-7 0 |
| 3 | u16 | capabilities, below |
| 5 | u8 | Wi-Fi: 0 off (never set up, or switched off), 1 connecting, 2 connected, 3 cannot connect |
| 6 | u8 | Home Assistant: 0 off, 1 on and waiting, 2 connected |
| 7 | u8 | firmware version: major |
| 8 | u8 | firmware version: minor |
| 9 | u8 | firmware version: patch |
| 10 | u32 | firmware build time, UTC Unix seconds (tells builds of one version apart) |

Capability bits (what the heater behind this board can do; a missing bit means the app hides the
control and the matching command is refused with `13`):

| Bit | |
|---|---|
| 0 | fan-only mode |
| 1 | temperature mode (thermostat) |
| 2 | fuel priming |
| 3 | stop priming early (without setting Off) |
| 4 | unused (0) |
| 5 | room temperature source can be chosen |
| 6 | Home Assistant can be switched on and off from the app |
| 7 | Bluetooth room thermometers: `SENSOR_*` and room source 2 |
| 8 | board firmware updates from the app over Bluetooth: `UPDATE_*` (this build checks signatures) |
| 9 | the internet relay and push notifications: `RELAY_*`, `PUSH_*` ([Internet relay](#internet-relay)) |
| 10 | a Home Assistant room temperature sensor is set up in this build (room source 1 can have a value) |
| 11 | the heater keeps a fault log: `HEATER_LOG`, State byte 32 |
| 12-15 | unused (0) |

Over Bluetooth Info is a plain read, never notified; over Wi-Fi the board sends it again whenever
it changes.

### State (`1002`)

What is asked of the heater and what the controller is doing. 39 bytes. Notified on every change
(at most 4 per second, coalesced).

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | flags: bit 0 heater answering; bit 1 the last stop came from the failsafe; bit 2 room temperature from a fallback source; bit 3 unused (0); bit 4 board firmware update running (from `UPDATE_BEGIN` until it ends or is dropped); bit 5 a phone is connected through the internet relay; bit 6 thermostat pause: a heat run the thermostat holds off because the room is warm enough (from its stop, through that cooldown, until it starts the heater again; also a run that begins warm enough), never set otherwise |
| 1 | u8 | requested mode: 0 off, 1 heat, 2 fan only, 3 priming |
| 2 | u8 | heat mode: 0 power level, 1 temperature |
| 3 | u8 | power, 0-100 %: the fixed power demand (0 % the low end of the heater's range, 100 % its maximum), and the fan speed in fan-only mode |
| 4 | i16 | target temperature, 0.1 °C |
| 6 | i16 | room temperature in use, 0.1 °C |
| 8 | u8 | room temperature source: 0 board probe (the default), 1 Home Assistant (capability bit 10), 2 a Bluetooth thermometer (capability bit 7). With no reading from it, the first of these that has one is used (flag bit 2) |
| 9 | u8 | heater state, below |
| 10 | u8 | controller status, below |
| 11 | u8 | fault, below (0 none) |
| 12 | u8 | the run was started from, kept while it stops and cools down: 0 no run, 1 Home Assistant, 2 a phone over Bluetooth, 3 the board itself (its own controls, a low-fuel or low-battery action), 4 a phone app over Wi-Fi, 5 frost protection, 6 a timer, 7 a phone through the internet relay, 8 a web browser (the board's web page) |
| 13 | u8 | heater parameter changes staged and not applied yet; board settings apply at once and never count |
| 14 | u16 | priming seconds left (0 when not priming) |
| 16 | u16 | failsafe seconds left before it stops the heater; `0xFFFF` while Home Assistant is connected, the heater is off, or the run was not started from Home Assistant |
| 18 | u16 | alerts active, [below](#alerts) |
| 20 | u8 | automatic starts: bit 0 frost protection on; bit 1 disarmed after a problem (`AUTO_REARM` arms them again); bit 2 the board knows the time; bit 3 at least one timer enabled |
| 21 | u8 | fuel left, % of the tank; `0xFF` with no tank size set |
| 22 | u8 | carbon burn-off run: minutes left, 0 when none is running |
| 23 | u8 | the timer slot whose run this is, `0xFF` when the run is not a timer's |
| 24 | u32 | when the current run stops by itself, UTC Unix seconds: its timer's end, or the end `RUN_UNTIL` set (0: no end) |
| 28 | u32 | this run's identity, 0 while off: a random number per board boot in the high 16 bits, the runs started since that boot (from 1) in the low 16. Opaque to the app: a different value is a different run. A run lasts from its start until the heater is really off: through stopping and cooling down, whatever changes on the way. A start during the cooldown is a new run. How long it has run is in [Telemetry](#telemetry-1003) (it changes every second, State does not) |
| 32 | u8 | the heater's fault log: its newest entry's number (0 empty); read it again when this changes |
| 33 | u32 | `START_AT`'s start to come, UTC Unix seconds (0: none) |
| 37 | u16 | its run's minutes (0 with none) |

**Heater state** (the backend maps its own states onto these): 0 unknown (no reply), 1 off,
2 starting, 3 heating, 4 fan only, 5 stopping, 6 cooling down, 7 priming, 8 updating the heater's
firmware.

**Controller status:** 0 idle, 1 starting / sending a change (also a start accepted that the heater
has not reported yet: it still says off), 2 running, 3 priming, 4 stopping, 5 no reply from the
heater (normally also while the heater's firmware is being updated: heater state 8 says so). The Home Assistant "Controller status"
text is the English name of this code.

**Fault** (the backend maps the heater's own code; the code as the heater's panel shows it is in
Telemetry):

| Code | Fault | Code | Fault |
|---|---|---|---|
| 1 | supply voltage (direction not reported) | 8 | flame-out / fuel shortage |
| 2 | supply voltage low | 9 | temperature sensor |
| 3 | supply voltage high | 10 | start failure |
| 4 | glow plug | 11 | CO above limit |
| 5 | fuel pump | 12 | intake air sensor |
| 6 | overheat | 13 | heater ↔ board communication |
| 7 | fan | 14 | CO sensor fault |
| | | 255 | other (see the panel code) |

### Telemetry (`1003`)

Measurements. 45 bytes. Notified at most once a second while the heater is not off, and at most
every 10 s otherwise.

| Offset | Type | Field |
|---|---|---|
| 0 | i16 | heat exchanger temperature, 0.1 °C |
| 2 | i16 | intake air temperature, 0.1 °C |
| 4 | u16 | supply voltage, 0.01 V |
| 6 | u16 | fan speed, rpm |
| 8 | u16 | fan voltage (the fan's drive), 0.01 V |
| 10 | u16 | fuel pump frequency, 0.01 Hz |
| 12 | u16 | glow plug power, W |
| 14 | u16 | glow plug voltage, 0.01 V |
| 16 | u16 | glow plug current, 0.01 A |
| 18 | u8 | power the heater is running at, % of its range |
| 19 | u8 | fault code as the heater's panel shows it (E-number), 0 none |
| 20 | u16 | CO, ppm |
| 22 | i16 | altitude, m: the heater's own, else the board's pressure sensor plus the Altitude offset setting (0x121) |
| 24 | u16 | barometric pressure, 0.1 hPa, from the board's own sensor |
| 26 | u32 | heater running time, lifetime, s |
| 30 | u8 | CO sensor: 0 not fitted, 1 warming up, 2 learning clean air, 3 ok, 4 alarm, 5 sensor fault, 6 too cold to trust |
| 31 | u32 | how long this run (State bytes 28-31) has run, s: it stops counting when the stop is asked, and holds through stopping and cooling down; 0 with no run |
| 35 | i16 | room temperature from the board's probe, 0.1 °C, its offset added (`0x8000`: none, or more than 5 minutes old) |
| 37 | i16 | room temperature from Home Assistant, 0.1 °C, its offset added (`0x8000`: none, or Home Assistant not connected) |
| 39 | i16 | room temperature from the chosen Bluetooth thermometer, 0.1 °C, its offset added (`0x8000`: none, or more than 10 minutes old) |
| 41 | i8 | the altitude's fuel trim, % (negative = less fuel; `0x80`: Altitude fuel trim off, or no altitude yet) |
| 42 | i8 | the fuel trim in effect, %: the Fuel trim setting plus the altitude's, held to ±30 (what the heater is sent) |
| 43 | i16 | the smoothed altitude the trim follows, m (`0x8000`: none yet) |

Every field has its "not reported" marker; the app shows a placeholder for it. A heater that does
not measure a value (an intake sensor, CO) always gives the marker; the fan and glow plug values
come once the heater answers.

## Commands and results

### Command (`1004`) and Result (`1005`)

The app writes one command per write:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | request id, 1-255, chosen by the app and echoed in the result |
| 1 | u8 | opcode |
| 2 | ... | arguments |

The write itself is answered at once (ATT write response). The outcome follows as an indication
on Result, which the app must have enabled (if it hasn't, commands are refused with ATT error
`0xFD`, CCCD improperly configured). Indications go out one at a time per phone; results and
events wait in a short queue (8) for the previous confirmation, so the app sends its next command
after the previous result:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | request id (0 = unsolicited event, below) |
| 1 | u8 | opcode (for events: event code) |
| 2 | u8 | result |
| 3 | ... | data: for refusals the reason as UTF-8 text, the same text Home Assistant's log shows |

### Opcodes

| Op | Name | Arguments | Same as in Home Assistant |
|---|---|---|---|
| `01` | SET_MODE | u8 0 off, 1 heat, 2 fan only | Heater climate mode |
| `02` | SET_HEAT_MODE | u8 0 power level, 1 temperature | Heat mode select |
| `03` | SET_POWER | u8 0-100 % | Power number |
| `04` | SET_TARGET | i16 0.1 °C | Heater climate target temperature |
| `06` | SET_ROOM_SOURCE | u8 0 board probe, 1 Home Assistant, 2 a Bluetooth thermometer (capability bit 7) | Room temperature source select |
| `07` | PRIME_START | u8 `A5` = "the user confirmed they are at the heater" | Prime fuel pump button |
| `08` | PRIME_STOP | | Stop priming button (or Off) |
| `09` | FAULT_RESET | | Reset heater fault button: clears a fault latched until a user reset, if its cause is gone. `2F` at once when the heater has no latched fault; otherwise `00` when asked, and event `06` brings the heater's own answer |
| `0A` | HEATER_LOG | u8 first entry wanted, counted back from the newest (0 = the newest) | (none), [The heater's fault log](#the-heaters-fault-log) |
| `0B` | HEATER_LOG_CLEAR | | (none) |
| `20` | PARAM_DESCRIBE | u8 index | |
| `21` | PARAM_GET | u16 id | |
| `22` | PARAM_SET | u16 id, i32 value | the parameter's number entity |
| `23` | PARAM_APPLY | | Save settings to heater button: applies the staged changes and saves them in the heater |
| `24` | PARAM_DISCARD | | Discard setting changes button |
| `25` | PARAM_GROUP | u8 index | (none): one settings menu, see [Parameters](#parameters) |
| `26` | PARAM_HELP | u16 id | (none): what a parameter does |
| `27` | PARAM_DESCRIBE_ID | u16 id | (none): `PARAM_DESCRIBE` for one parameter by id |
| `30` | FORGET_ME | | (deletes this app's Wi-Fi key and its Bluetooth bond, over either transport; the result goes out first, then the board disconnects) |
| `32` | WIFI_KEY | | (Bluetooth only: issues this phone's Wi-Fi key, see [Wi-Fi keys](#wi-fi-keys)) |
| `40` | PIN_SET | u32 new PIN, 0-999999 | (none) |
| `41` | WEB_PASSWORD_SET | u8 length, current password (empty if none) ‖ u8 length, new password (8-64 bytes UTF-8; empty turns the web sign-in off) | (none) |
| `50` | WIFI_STATUS | | (Wi-Fi signal sensor) |
| `51` | WIFI_SET | u8 SSID length, SSID, u8 password length, password | (none) |
| `52` | WIFI_OFF | | (none) |
| `60` | HA_STATUS | | (none) |
| `61` | HA_SET | u8 0 off, 1 on | (none) |
| `62` | HA_KEY | | (none) |
| `63` | HA_KEY_NEW | | (none) |
| `70` | CLOCK_SET | u32 UTC Unix seconds, u8 length, POSIX time zone (at most 63 bytes) | (none), [Clock](#clock) |
| `71` | CLOCK_GET | | (none) |
| `72` | METERS | | (Fuel used / Burner hours / Glow plug hours sensors), [Fuel](#fuel-meters-and-tank) |
| `73` | TANK_SET | u8 fuel in the tank, 0-100 % (100: filled up) | Fuel tank filled button (100) |
| `74` | ALERTS_CLEAR | u16 alert bits | Clear alerts button |
| `75` | AUTO_REARM | | Re-arm frost protection and timers button |
| `76` | TIMER_GET | u8 slot | (none), [Timers](#timers) |
| `77` | TIMER_SET | u8 slot, timer record | (none) |
| `78` | TIMER_DELETE | u8 slot | (none) |
| `79` | LOG_READ | u32 first sequence number wanted | (none), [Event log](#event-log) |
| `7A` | GLOW_HEALTH | | (none), [Glow plug health](#glow-plug-health) |
| `7B` | GLOW_NEW_PLUG | | New glow plug fitted button |
| `7C` | BURNOFF_START | u8 minutes, 10-30 | Carbon burn-off button (the length setting), [Carbon burn-off](#carbon-burn-off) |
| `7D` | RUN_UNTIL | u32 end, UTC Unix seconds (0: no end) | (none), [Run end](#run-end) |
| `7E` | START_AT | u32 start, UTC Unix seconds (0: cancel), u16 run minutes 1-1440 | (none), [Start at](#start-at) |
| `7F` | LOG_DETAIL | u32 event log sequence number, u8 part (0 the readings, 1 the burn window) | (none), [Snapshots](#snapshots) |
| `80` | SENSOR_SCAN | u8 seconds, 5-60 | (none), [Bluetooth thermometers](#bluetooth-thermometers) |
| `81` | SENSOR_FOUND | u8 index | (none) |
| `82` | SENSOR_USE | u8[6] address, u8 kind | (none) |
| `83` | SENSOR_STATUS | | (none) |
| `90` | UPDATE_BEGIN | u32 image size, u8[32] SHA-256 of the image (Bluetooth only) | (none), [Board firmware updates](#board-firmware-updates) |
| `91` | UPDATE_STATUS | | (none) |
| `92` | UPDATE_FINISH | | (none) |
| `93` | UPDATE_ABORT | | (none) |
| `A0` | RELAY_STATUS | | (Relay text sensor), [Internet relay](#internet-relay) |
| `A1` | RELAY_SET | u8 0 off, 1 on, u8 n, host (n bytes, at most 63; n = 0 the default) | (none) |
| `A2` | PUSH_PREFS | u16 alert mask, u8 event mask, u8 flags | (none), [Push notifications](#push-notifications) |
| `A3` | PUSH_PREFS_GET | | (none) |

`PRIME_START` without the `A5` byte is refused: priming runs fuel into a cold heater and the app
must ask the user first. The controller's own checks are the same as for the Home Assistant button.

### Results

| Code | Meaning |
|---|---|
| `00` | OK: done, or accepted and under way |
| `01` | stored, not acting yet, as the same entity change from Home Assistant |
| `10` | unknown opcode |
| `11` | wrong argument length |
| `12` | argument out of range |
| `13` | not supported by this heater or build (capability missing) |
| `20` | refused: the heater is not off / not idle |
| `21` | refused: the heater does not answer |
| `22` | refused: busy (a heater or board firmware update, or a parameter apply in progress) |
| `23` | refused: confirmation missing |
| `24` | sign-in refused: key unknown or revoked (Wi-Fi) |
| `25` | refused: wrong password (Wi-Fi sign-in, or the current password in `WEB_PASSWORD_SET`) |
| `26` | refused: locked out after wrong passwords; the data holds u16 seconds left |
| `27` | refused: key table full |
| `28` | refused: only over Bluetooth (`WIFI_KEY`, `UPDATE_BEGIN`) |
| `29` | refused: no web password set |
| `2A` | refused: the image does not fit the board's update slot |
| `2B` | refused: the image received does not match its SHA-256 (or bytes are missing) |
| `2C` | refused: the image is not signed by a key this board trusts, or is not a valid image for it |
| `2F` | refused, other reason (see the text) |

Apps treat any result code they don't know as a refusal and show the text.

### Events

Unsolicited events (request id 0) go to every subscribed phone and signed-in app:

| Event | Result byte | Data |
|---|---|---|
| `01` parameters applied and saved in the heater | `00` ("saved in the heater"), `2F` ("the heater refused the changes") | text |
| `03` parameter value changed (by the heater, another phone or Home Assistant) | `00` | u16 id |
| `04` failsafe stopped the heater | `00` | reason text |
| `05` Wi-Fi change finished | `00` connected, `2F` failed and the old settings are back | `00`: u8[4] IPv4 address, SSID; `2F`: reason text |
| `06` the heater answered a fault reset | `00` cleared, or the heater's refusal | text |
| `08` an alert came on | `00` | u8 alert bit, then the text the app shows (UTF-8, to the end) |

Codes `02` and `07` are not sent and not reused.

### The heater's fault log

Capability bit 11: the safety MCU's own record of its last 32 faults, dead-man stops and resets,
kept over its resets. `HEATER_LOG` takes the first entry wanted, counted back from the newest (u8,
0 = the newest), and returns the number of entries in the log (u8), then up to 8 entries, newest
first, of 14 bytes each:

| Bytes | Field |
|---|---|
| 0 | kind: 1 a fault, or a stop for a reason without one (code 0); 2 a reset of the safety MCU |
| 1 | code: the fault (as State's), or the reset's reason (0 unknown, 1 power-up, 2 watchdog, 3 reset pin, 4 brown-out, 5 bootloader, 6 software) |
| 2 | why: the heater's own reason code (as Telemetry byte 19, `stm32/core/burn.h` `BURN_WHY_*`); 0 for a reset |
| 3 | the heater's step it happened in (`BURN_STEP_*`: 0 off, 1 fan check, 2 preheat, 3 ignition, 4 purge, 5 warm-up, 6 running, 7 burn-off, 8 cooldown, 9 fan only, 10 priming, 11 pre-start self-test, 12 fan retry); for a reset, the step it started in (8: hot, it cools down) |
| 4 | its number, 1-255, one more for each entry, wrapping past 0 |
| 5 | the board's temperature, °C (i8, `0x80` unknown) |
| 6-7 | the heat exchanger, 0.1 °C (i16) |
| 8-9 | the supply, 0.01 V (u16) |
| 10-13 | seconds since the safety MCU's last reset (u32) |

State byte 32 is the newest entry's number (0: empty): read the log again when it changes.
`HEATER_LOG_CLEAR` empties it, in the safety MCU too.

### Parameters

The heater's parameters and the board's own settings (thermostat tuning, altitude trim, and the
rest below) come as one table that the board describes, so the app needs no list of its own and
never learns the backend. The heater's own parameters come first, then the board's settings, which
every board has. A heater with no parameters of its own (or one not described yet) has only the
board's part of the table.

`PARAM_DESCRIBE(index)` for index 0 .. count-1 returns, in the result data (`PARAM_DESCRIBE_ID(id)`
the same for one parameter found by its id, `12` for an unknown id):

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | index |
| 1 | u8 | count (parameters in the table) |
| 2 | u16 | id (stable for this board; used by GET and SET) |
| 4 | u8 | flags: bit 0 the heater must be off to apply it; bit 1 board setting, applied at once (no APPLY needed); bit 2 read-only |
| 5 | u8 | unit: 0 none, 1 s, 2 %, 3 Hz, 4 rpm, 5 °C (a temperature), 6 W, 7 m, 8 min, 9 on/off, 10 °C difference (a band or offset: x1.8 in °F, no +32), 11 L (a volume: an app may show US gallons, x0.264172), 12 mL (shown as is), 13 V, 14 h. An app shows an unknown unit as a plain number |
| 6 | u8 | decimals (display value = raw / 10^decimals) |
| 7 | u8 | group, below (the settings menu it is in) |
| 8 | u8 | level: 0 basic (always shown), 1 advanced (in the Advanced menus), 2 expert (only with Expert mode on); an app shows an unknown level as expert |
| 9 | i32 | minimum |
| 13 | i32 | maximum |
| 17 | i32 | step |
| 21 | | name, UTF-8, the Home Assistant entity name (at most 40 bytes) |

**Settings menus.** `PARAM_GROUP(index)` for index 0 .. count-1 returns the groups that have
parameters on this board, in the order the app shows them: u8 index, u8 count (groups on this
board), u8 group code, then the group's name (UTF-8, to the end).

| Code | Group | | Code | Group |
|---|---|---|---|---|
| 1 | Thermostat | | 7 | Altitude |
| 2 | Fuel pump | | 8 | Fuel use |
| 3 | Fan | | 9 | Frost protection |
| 4 | Start-up | | 10 | Room temperature |
| 5 | Glow plug | | 11 | Battery |
| 6 | Shut-down | | 12 | Maintenance |
| | | | 0 | Other (last) |

Menu order: Thermostat, Frost protection, Room temperature, Fuel use, Battery, Fuel pump, Fan,
Start-up, Glow plug, Shut-down, Altitude, Maintenance, Other.

The name comes from the board; the code is stable, so an app may pick an icon by it (and a plain
one for a code it does not know).

`PARAM_HELP(id)` returns `u16 id` and the parameter's description as UTF-8 to the end: one or two
plain sentences on what it does and what raising or lowering it changes, at most 120 bytes. Empty
when the board has none for it (yet: on a board with a safety MCU the texts come over its link in
the first seconds after start-up). Descriptions don't change while the board runs, so an app may
fetch one when the user first opens it.

`PARAM_GET(id)` returns `u16 id, u8 flags (bit 0 value known, bit 1 change staged), i32 value in
the heater (or board), i32 staged value`. `PARAM_SET` stages a heater parameter (applies a board
setting at once) exactly as the number entity does. `PARAM_APPLY` (no arguments) applies every
staged change and saves it in the heater, with the outcome as event `01`; `PARAM_DISCARD` drops
the staged changes. The heater checks the staged set as a whole and may refuse it.

## Wi-Fi transport (WebSocket)

Once the user has put the board on their network ([Wi-Fi setup](#wi-fi-setup-from-the-app)), it
runs an HTTP server on port 80:

| Route | |
|---|---|
| `GET /` and the page's files | The web page: the app built for the browser, stored gzipped in the firmware image (one update brings the board and its page together). Open to anyone on the network; it holds no data until it signs in |
| `GET /api/v1` | WebSocket upgrade, subprotocol `flamekeeper.v1` |

Everything else gets `404`. There is no REST API and no second set of endpoints: the web page and
the phone apps send the same command bytes and get the same records as over Bluetooth. The board
answers only requests whose `Host` header is its IP address or its mDNS name (`<name>.local`),
otherwise `421`, so another web page open in a browser on the network cannot reach it through DNS
rebinding. A WebSocket handshake is refused (no `101`) unless its `Origin` header is absent (a
native client), the phone apps' web view (`capacitor://localhost` on iOS, `https://localhost` or
`http://localhost` on Android), or the board's own page (`http://` and one of those two names). A
web page from anywhere else therefore cannot open connections to the board. It advertises
`_flamekeeper._tcp` over mDNS (TXT: protocol major, board), so apps find it on the network as they
find it over Bluetooth by the service UUID.

The page's files, as the board sends them:

- Always gzipped (`Content-Encoding: gzip`), with an `ETag`; a matching `If-None-Match` gets `304`.
- `index.html` with `Cache-Control: no-cache`, so a board update shows at once. Files whose names
  carry the build's content hash (`assets/index-CkksxKgm.js`) with
  `public, max-age=31536000, immutable`.
- `Content-Security-Policy: default-src 'self'; connect-src 'self' ws://<host>; style-src 'self'
  'unsafe-inline'; img-src 'self' data:; frame-ancestors 'none'`, where `<host>` is the request's
  own `Host`. No inline scripts and nothing from other sites: the page is the board's alone.
- Built in at compile time from the app's browser build, through the `web_app` option of
  `flamekeeper_net`. A build without it serves a placeholder page.

The connection is plain HTTP; the protocol encrypts and authenticates every frame itself
([below](#what-this-protects)). That keeps the same frames usable through the internet relay,
which only forwards bytes it cannot read.

### Frames

Every WebSocket message is one binary frame: a one-byte record type, then the record. Types
`01`-`05` are the low byte of the matching characteristic UUID, and the records are byte for byte
the Bluetooth ones.

| Type | Direction | Content | When |
|---|---|---|---|
| `01` | board → app | Info, in the clear | on connect, and whenever it changes |
| `10` | board → app | Hello: 16 random bytes (the board's nonce), in the clear | on connect, after Info |
| `11`-`15` | both | sign-in messages, below, in the clear | |
| `02` | board → app | State, encrypted | once signed in, then on change (at most 4 per second) |
| `03` | board → app | Telemetry, encrypted | once signed in, then as the Telemetry notification |
| `04` | app → board | Command, encrypted | once signed in |
| `05` | board → app | Result and events, encrypted | the answer to each command, and the unsolicited events |

**Encrypted frames:** type byte (in the clear, authenticated) ‖ AES-128-GCM ciphertext of the
record ‖ 16-byte tag. The nonce is the direction's 4-byte salt ‖ a 64-bit little-endian frame
counter that starts at 0 in each direction and is not sent (TCP keeps frames in order). A frame
that does not verify, a text frame, an unknown type, a command before signing in or a second
sign-in closes the connection (close code 1008).

Results are not queued as on Bluetooth, but apps still wait for a command's result before sending
the next one, so they behave the same on both transports. The board takes a set number of
WebSocket connections at once (its `max_sessions` setting, 4 by default); one that has not signed
in after 30 s is closed; a WebSocket ping every 20 s finds dead ones.

### Wi-Fi keys

Over Wi-Fi an app signs in with a **Wi-Fi key**: a 16-byte id and a 32-byte secret from the board's
hardware RNG, kept in NVS. There are two tables of 8:

| | Phone app keys | Browser keys |
|---|---|---|
| Obtained | over Bluetooth, from a bonded phone (`WIFI_KEY`) | over Wi-Fi, with the web password (EC J-PAKE, below) |
| What the user enters | nothing more: the PIN was entered once, for Bluetooth pairing | the web password, once per browser |
| Kept by the app in | the iOS Keychain or Android Keystore | the page's local storage ("remember this browser"), or only for the tab |
| When the table is full | refused (`27`) | the least recently used browser key is dropped |
| Revoked by | `FORGET_ME`, a factory reset | `FORGET_ME`, a new web password, a factory reset, dropped as least recently used |

A connection signed in with a key that is then revoked is closed at once. A key becomes "most
recently used" only after a sign-in with it succeeds; a failed one changes nothing on the board.

A bonded phone asks for its key right after pairing, so from then on it uses Bluetooth or Wi-Fi,
whichever reaches the board. One key per bond: asking again replaces it. The key crosses no
network: the Bluetooth link is encrypted with the bond. A phone app that never pairs over
Bluetooth can also sign in with the web password, as a browser does.

A browser key not remembered is deleted when its connection closes.

### Sign-in with a key

| Step | Frame | Content |
|---|---|---|
| 1 | `10` board → app | the board's nonce Nb (16 bytes) |
| 2 | `11` app → board | u8 method 0 ‖ key id (16) ‖ the app's nonce Na (16) ‖ HMAC-SHA256(secret, `"fk1 app"` ‖ Nb ‖ Na) |
| 3 | `12` board → app | u8 result (`00`, or `24` key unknown) ‖ on `00`: HMAC-SHA256(secret, `"fk1 board"` ‖ Nb ‖ Na) |

The app checks the board's answer, so it knows it reached the board that issued its key and not
something else on the network. Both sides then derive the session keys: HKDF-SHA256(key = secret,
salt = Nb ‖ Na, info = `"fk1 session"`) gives 40 bytes: the app → board key (16), the board → app
key (16), and the two 4-byte nonce salts (app → board first). The board then sends State and
Telemetry.

### Sign-in with the web password (EC J-PAKE)

EC J-PAKE on P-256 with SHA-256, as implemented by mbedTLS (`mbedtls_ecjpake_*`, the variant
Thread commissioning uses), with the web password (UTF-8) as the shared secret. The app is the
client, the board the server. Message layouts are mbedTLS's round one and round two.

| Step | Frame | Content |
|---|---|---|
| 1 | `11` app → board | u8 method 1 ‖ u8 remember (0 this tab only, 1 remember) ‖ the app's round one (the rest of the frame). No browser name: the board's log and "Run started by" say "web browser" |
| 2 | `13` board → app | u16 length of the board's round one ‖ the board's round one ‖ the board's round two |
| 3 | `14` app → board | the app's round two |
| 4 | | both sides derive K = HKDF-SHA256(the 32 bytes of `mbedtls_ecjpake_derive_secret`, salt = Nb, info = `"fk1 password"`) |
| 5 | `15` app → board | HMAC-SHA256(K, `"fk1 app confirm"` ‖ SHA-256 of the frames `10`, `11`, `13`, `14`, whole) |
| 6 | `12` board → app | u8 result ‖ on `00`: HMAC-SHA256(K, `"fk1 board confirm"` ‖ the same hash) ‖ the new browser key, AES-128-GCM under the first 16 bytes of K |

After step 6 the app signs in with the new key on the same connection (method 0, from step 2 of
the previous table).

- Results: `00`; `25` wrong password (the confirmation in step 5 did not match); `26` locked out;
  `29` no web password set (the page then tells the user to set one in the phone app).
- **Wrong passwords:** after 5 in a row the board refuses password sign-ins for 1 minute, then
  doubles the wait after each further wrong one, up to 1 hour. A success resets the count.
- **Pacing:** password sign-ins start at most once every 5 s on the whole board, right or wrong
  (the J-PAKE maths runs in the board's main loop). One that comes too soon gets `26` with the
  seconds to wait, before any J-PAKE work, and does not count as a wrong password. The count is for
  the whole board, so a stranger on the network can delay browser sign-ins but not guess fast;
  phones over Bluetooth and keys already issued are not affected.
- No pairing window is needed: the web password is the user's own and is meant for use from a
  computer far from the heater.

### What this protects

- **Someone who listens** to the network learns nothing: not the web password (J-PAKE gives no
  offline guess, unlike a password hashed with a challenge), not a key, not the commands or the
  state.
- **Someone who can change traffic** (a compromised device on the network, a hostile access point)
  cannot sign in, cannot forge, replay or reorder frames, and cannot pose as the board to a phone
  app. They can cut connections, and make online password guesses at the locked-out rate.
- **The web page is the exception.** It is served over plain HTTP, so someone who can change
  traffic can also replace the page itself with one that sends them the password as the user
  types it. Encryption cannot fix that; only HTTPS with a certificate the browser trusts could, and
  a board on a home network cannot get one. The phone apps do not have this weakness: their code
  is on the phone.
- The board is for the home network. Reaching it from outside goes through the
  [internet relay](#internet-relay), never through a forwarded port.

## Wi-Fi setup from the app

A new board has Wi-Fi switched off until the user sets it up from the app over Bluetooth, with the
network name, its password and a web password; the app can change them later. There is no
fallback access point, no captive portal and no Improv. A board with no Wi-Fi settings, or after a
factory reset, does not use its Wi-Fi radio at all.

| Op | Over | Arguments | Result data |
|---|---|---|---|
| `50` WIFI_STATUS | both | | u8 state (as Info byte 5) ‖ i8 signal, dBm ‖ u8[4] IPv4 address ‖ SSID |
| `51` WIFI_SET | both | u8 SSID length (1-32) ‖ SSID ‖ u8 password length (0 open network, 8-63) ‖ password | |
| `52` WIFI_OFF | both | | |
| `41` WEB_PASSWORD_SET | both | current web password ‖ new web password (see the opcode table) | |

- The app's setup screen asks for the three fields together and sends `WIFI_SET`, then
  `WEB_PASSWORD_SET`.
- `WIFI_SET` answers `00` at once, then, half a second later, the board drops its current
  connection and tries the new network. Event `05` follows: connected (with the address) once the
  board has connected again, on the new network, or failed. **A change never strands the board:**
  the old settings stay until the new network connects, and if it has not within 60 s the board
  goes back to them. They are also kept in flash during the trial, so a restart in the middle of it
  goes back to them too.
- Over Wi-Fi, the app's own connection drops when the board switches networks. The app finds the
  board again by mDNS on the new network, or over Bluetooth, and reads `WIFI_STATUS`.
- Neither password is ever readable back, over any transport or in Home Assistant. The web
  password is stored as the J-PAKE secret needs it, in NVS.
- Changing the web password needs the current one, and signs every browser out. Phone app keys
  stay.
- `WIFI_OFF` forgets the network and switches Wi-Fi off; Home Assistant, the web page and Wi-Fi
  apps lose the board until it is set up again. The app asks the user first.

## Home Assistant on demand

Home Assistant is off on a new board and is switched on from the app (capability bit 6).

| Op | Over | Arguments | Result data |
|---|---|---|---|
| `60` HA_STATUS | both | | u8 state (as Info byte 6) |
| `61` HA_SET | both | u8 0 off, 1 on | |
| `62` HA_KEY | both | | the 32-byte API encryption key (the app shows it as base64 and a QR code) |
| `63` HA_KEY_NEW | both | | the new key; Home Assistant must be given it again |

- **Off** means ESPHome's API does not listen and mDNS does not announce it, so Home Assistant
  neither finds nor reaches the board. The Home Assistant failsafe then never applies (no run can
  start from Home Assistant), and the "Home Assistant" room temperature source falls back to the
  board's probe.
- **Switching on** creates the API key on the board if there is none and shows it in the app. The
  user adds the board in Home Assistant (it is announced over mDNS) and pastes the key there.
  Because the board already has a key, nobody else on the network can add it first.
- Switching on or off restarts the board (ESPHome's API cannot start or stop while running), so
  `HA_SET` is refused while the heater is not off (`20`).
- Over Bluetooth Info is not notified, so an app follows the state (waiting → connected) with
  `HA_STATUS`.
- Refused with `13` on a board without capability bit 6.

## Pairing and security

### Mechanism

- **LE Secure Connections only** (P-256 ECDH, AES-CCM link encryption), MITM protection
  required, bonding. Legacy pairing is refused.
- **Passkey entry, the board as "display only".** The phone shows a 6-digit entry box; the
  passkey is the board's [PIN](#the-pin).
- **Pairing window.** Pairing requests outside an open window are refused at the security request
  (`esp_ble_gap_security_rsp(..., false)`), so a bonded phone keeps working all the time but a new
  phone can only pair while someone opened the window. The window lasts 2 minutes and closes early
  after one new phone, or after three failed pairings (a wrong PIN). It opens on power-up (reset
  reason "power on" only, not an update or watchdog restart), a short press of BOOT, or the Home
  Assistant button "Allow Bluetooth pairing".
- **Failed pairings add up.** A phone that fails pairing on purpose learns the PIN about a bit at
  a time, so the board keeps a count of failed pairings since the last good one, over restarts.
  From 5 on, a power-up no longer opens the window; BOOT and the Home Assistant button still do. A
  good pairing clears the count.
- **Bond table:** 8 phones. When full, new phones are refused until a factory reset or a
  `FORGET_ME`.
- Bonds, keys, the user's PIN, the passwords and the Home Assistant key live in NVS and survive
  firmware updates. A factory reset clears them.

Why both a window and a PIN: the window alone lets anyone nearby in while the user switches the
heater on; a fixed PIN alone could be learned by someone who watches one Bluetooth pairing over the
air (a known weakness of reused passkeys) and then used at any time. Together an attacker needs
the PIN, radio range and the moment the window is open.

### The PIN

The board ships with a 6-digit PIN printed on its label; a factory reset brings the same PIN back;
the user can change it from the app; Home Assistant does not show it. The PIN is the Bluetooth
passkey. It only lets new phones in: phones already paired keep working when it changes.

**Derivation.** The serial is not secret (it is on the label, in the Device Information service
and in Home Assistant), and anything in the firmware is in every update image, so the PIN cannot be
a function of the serial alone. Instead:

1. Production holds a **master key** on the provisioning machine. It is never on a board, in the
   firmware or in this repository.
2. For each board, production writes the serial into an eFuse user-data block and burns
   **board key = HMAC-SHA256(master key, serial)** into an ESP32-C6 eFuse key block with purpose
   `HMAC_UP`. Software cannot read that block: only the C6's HMAC peripheral can use it, so not
   even a malicious firmware update can copy it out.
3. **Label PIN** = HMAC(board key, `"fk1 pin"` ‖ counter), reduced to 000000-999999 without bias
   (rejection sampling). The board computes it with the HMAC peripheral (`esp_hmac_calculate()`);
   production computes the same value from the master key and the serial, and prints it on the
   label with a QR code (`flamekeeper://pair?s=<serial>&k=<PIN>`).
4. A lost label can be recomputed by whoever holds the master key, from the serial.

**Changing it:** `PIN_SET` from any app that is in. The new PIN is kept in NVS and used for new
pairings from then on; Info byte 2 bit 1 says it was changed. A factory reset deletes it, so the
label's PIN applies again.

### Factory reset

Forgetting every app is the job of the board's button, not the app's. With the board running, hold
BOOT 10 to 30 s and let go (the reset starts on release; held through a reset or power-up, BOOT is
the ESP32's download mode instead). The board stops the heater, then restarts:

- **Access:** every Bluetooth bond and Wi-Fi key deleted, the PIN back to the label's, the web
  password deleted, Wi-Fi settings deleted and Wi-Fi off, Home Assistant off and its key deleted.
- **Settings:** every board setting back to its default (heat mode, power, target, room
  temperature source, thermostat tuning, altitude trim, fuel and fan trims), and the safety MCU's
  heater parameters back to its defaults, since they are stored on the board. The board waits up
  to 8 s for the safety MCU to confirm; without that it asks again after the restart, every 10 s,
  until the MCU answers. A heater whose parameters are not kept on the board owes nothing.
- **Everything else it stored, cleared too:** timers and frost protection, `START_AT`, the chosen
  Bluetooth thermometer, the time zone, the fuel meters and tank level, the lifetime and
  maintenance counters (glow plug baseline, burn-off), the event log, the relay's key pair and
  settings, the push settings. Only the firmware is kept, and the clock until the next power cut.

### What an app that is in can do

Everything Home Assistant can do through the entities listed above, plus the Wi-Fi settings, the
PIN, the web password, the Home Assistant switch and key, and board firmware updates over
Bluetooth. Phone apps and browsers are equal; there are no roles.

### Home Assistant failsafe

The failsafe applies only to a run that was **started from Home Assistant**. Such a run stops when
Home Assistant has been unreachable for 15 minutes. A run started from an app or from the board's
own controls runs until someone stops it, like a stock panel, whether or not Home Assistant is
there.

- A run's source is the source of the command that started it (off → heat, fan or priming). Later
  changes (power, target, heat ↔ fan) from any source keep it. A new start after the heater is off
  sets it again.
- The source is not kept over a restart: after a restart the board never resumes heating anyway.

## Automatic starts, fuel and alerts

The board does these itself, so they work with no app connected; the app sets them up and shows
them.

### Settings (board settings in the parameter table)

| Id | Name | Unit | Range, default | Group, level |
|---|---|---|---|---|
| `0x130` | Fuel pump size | mL per 1000 strokes, 1 decimal | 10.0-80.0, 22.0 | Fuel use, advanced |
| `0x131` | Fuel tank size | L, 1 decimal | 0-200.0, 0 = no tank tracking | Fuel use, basic |
| `0x132` | Low fuel alert | % | 0-50, 20 (0 = none) | Fuel use, basic |
| `0x133` | Stop at fuel left | % | 0-50, 10 (0 = never) | Fuel use, basic |
| `0x140` | Frost protection | on/off | off | Frost protection, basic |
| `0x141` | Frost protection temperature | °C, 1 decimal | 1.0-15.0, 5.0 | Frost protection, basic |
| `0x142` | Frost protection stop above | °C difference, 1 decimal | 0.5-10.0, 3.0 | Frost protection, basic |
| `0x150` | Board probe offset | °C difference, 1 decimal | -5.0-+5.0, 0 | Room temperature, advanced |
| `0x151` | Home Assistant temperature offset | °C difference, 1 decimal | -5.0-+5.0, 0 | Room temperature, advanced |
| `0x152` | Bluetooth thermometer offset | °C difference, 1 decimal | -5.0-+5.0, 0 | Room temperature, advanced |

The pump size default is the common Chinese-heater pump's: 22 mL per 1000 strokes (one stroke per
Hz). "Max heater power in temperature mode" is **Thermostat max power** (`0x113`, basic): the most
power the thermostat asks for, lower for a quieter heater.

### Clock

Timers and the 24 h / 7 day / 30 day meters need the local time. Sources: the app sends
`CLOCK_SET` on every connection; Home Assistant's time when it is connected. The time survives a
board restart but not a power cut, until a source sets it again.

Every `CLOCK_SET` is taken, but the event log gets "clock set" (and the board saves the source and
zone) only when it changes something: the clock was not known, it was off by more than 60 s, or the
time zone changed. A new source alone is saved, not logged; `CLOCK_GET` always names the source
that set it last. After a restart the board takes the time back from its own clock without a log
entry.

- `CLOCK_SET`: u32 UTC Unix seconds, u8 length, a POSIX time zone (`TZ`) string such as
  `CET-1CEST,M3.5.0,M10.5.0/3` or `CST6CDT,M3.2.0,M11.1.0`, which the board keeps (so daylight
  saving changes happen without the app). The app builds it from the phone's time zone (its
  current and next daylight-saving transitions). Zones whose abbreviations are not letters use
  quoted names (`<+0530>-5:30`, `<-04>4<-03>,M9.1.6/24,M4.1.6/24`), which the board's C library
  reads. Refused with `12` for a time before 2025 or a string the board cannot read.
- `CLOCK_GET` returns u32 UTC Unix seconds (0: not known), u8 source (0 none, 1 an app, 2 Home
  Assistant), then the time zone string.

### Fuel meters and tank

The board counts fuel from the pump: every pump stroke (one per Hz) is "Fuel pump size" / 1000 mL.
Burner time counts while the heater is starting or heating; glow plug time while the plug draws
power. Priming counts as fuel but not as burner time.

`METERS` returns 14 × u32, fuel in mL and times in seconds:

| Offset | Field |
|---|---|
| 0, 4, 8 | this run (or the last one, while off): fuel, burner time, glow plug time |
| 12, 16 | last 24 hours: fuel, burner time |
| 20, 24 | last 7 days: fuel, burner time |
| 28, 32 | last 30 days: fuel, burner time |
| 36, 40, 44 | lifetime: fuel, burner time, glow plug time |
| 48 | fuel used since the tank was last set (`TANK_SET`) |
| 52 | burner time at low power since the last carbon burn-off, s (the reminder's counter) |

The 24 h, 7 and 30 day values are `0xFFFFFFFF` while the board does not know the time (they need
it to place fuel in hours and days). Meters survive restarts (saved every 5 minutes while running
and at every stop). A factory reset clears them with everything else the board stored.

**Tank:** with a "Fuel tank size", fuel left (State byte 21) is size minus the fuel used since
`TANK_SET`, in %. `TANK_SET(100)` is "Mark tank as filled" (Home Assistant: the Fuel tank filled
button); another value says how full it is now.

- At or below "Low fuel alert": alert bit 0.
- At or below "Stop at fuel left" (when not 0): a run is stopped and starts are refused, from
  every source, with `2F` "fuel low: fill the tank, then Mark tank as filled"; alert bit 1. Both
  clear with `TANK_SET` above them.

The estimate drifts with the pump (a few %): the app says "about".

### Frost protection

With "Frost protection" on, the board starts the heater when the room temperature falls to the
frost protection temperature, and stops it again once the room is "stop above" warmer:

- Only from off (no run of any kind), with a known room temperature, automatic starts armed, and
  every check a start from an app passes. Source 5 in State.
- The run is a temperature-mode heat run with the target at temperature + stop above; it stops
  when the room reaches it, and the user's own heat mode and target are put back.
- No room temperature (source and fallbacks all unknown): no start, alert bit 6.
- An Off from a person during a frost run turns frost protection off (the setting), so it does
  not start again at once; the app shows it.

### Timers

8 slots, each a weekly timer (days and a start time) or a one-off (a date and time). Record, 15
bytes:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | bit 0 enabled; bit 1 one-off (else weekly) |
| 1 | u8 | weekly: days, bit 0 Monday … bit 6 Sunday |
| 2 | u16 | weekly: start, minutes after local midnight (0-1439) |
| 4 | u32 | one-off: start, UTC Unix seconds |
| 8 | u16 | how long, minutes (1-1440) |
| 10 | u8 | mode: 1 heat, 2 fan only |
| 11 | u8 | heat mode: 0 power, 1 temperature |
| 12 | u8 | power, 0-100 % (power mode, and the fan speed in fan-only mode) |
| 13 | i16 | target, 0.1 °C (temperature mode) |

`TIMER_GET(slot)` returns u8 slot, u8 slots (8), the record (all zero: empty). `TIMER_SET(slot,
record)` stores one (`12` for a field out of range), `TIMER_DELETE(slot)` empties it.

- At its start (local time, needing the clock), a timer starts the heater as set, if it is off,
  automatic starts are armed, and every start check passes. Source 6. While the heater cools down
  from an earlier run it waits, until the end of its time.
- At the end of its time it stops the run, if that run is still its own, and the user's own heat
  mode, target and power are put back (as after a frost run).
- The heater already running (from anyone): the timer does nothing, and stops nothing.
- An Off from a person ends that occurrence; the timer stays.
- A one-off is disabled once its time has passed (run or not); weekly ones repeat.
- A board restart in the middle of a timer's time does not resume it.
- Timers enabled but the time not known: alert bit 5.

### Run end

`RUN_UNTIL(end)` (the app's "stop in X min") gives the current run an end, whoever started it (an
app, Home Assistant, the heater's own controls, a timer). At `end` the board stops it as a timer's
end does (stop source 6, not "stopped by itself"). State bytes 24-27 show the end; byte 23 still
names a timer only for a timer's run.

- Only while a run is on: otherwise `2F` "the heater is not running". An end needs the board's
  clock (`2F` without it; the app sets it on connecting), and must be in the next 24 hours (`12`).
- A new `RUN_UNTIL` replaces the end, a timer's included; `0` removes it (the run goes on until
  stopped).
- The run's end clears it, however it ends. A board restart stops the heater (stop-on-restart), so
  no end is kept over a restart.

### Start at

`START_AT(start, minutes)` (the app's "start in X min", the mirror of `RUN_UNTIL`). At `start` the
board starts a heat run as a one-off timer's start does (run source 6; the heat mode, target and
power as they stand then) and stops it `minutes` after `start`, as a timer's run ends. State bytes
33-38 show the start to come; once it runs, bytes 24-27 show the end and byte 23 stays `0xFF` (no
timer slot). Automatic-start rules apply as for a one-off timer: none while disarmed, and a refused
or failed start disarms; a start that finds the heater still cooling down waits for it until the
run's end.

- Only while the requested mode is off: otherwise `20`. It needs the board's clock (`2F`); `start`
  ahead and within the next 24 hours, minutes 1-1440 (`12`). `start` 0 cancels and is never refused.
- One per board: a new one replaces it.
- Cleared when it starts its run, when any other run starts first (a phone, Home Assistant, the
  heater's controls, a timer, frost protection, a burn-off, a prime), and by a factory reset.
- Kept over a restart while still ahead. One whose time passed while the board was off or did not
  know the time is dropped, not started late.

### Automatic starts and problems

Frost protection and timers stay armed across a board restart and start fresh when due. When an
automatic start is refused or fails, or the heater stops by itself during an automatic run (a
fault, a flame-out), **both are disarmed** (State byte 20 bit 1, alert bit 3) until someone sends
`AUTO_REARM` (an app, or the Home Assistant button). The disarmed state survives restarts. They
never retry by themselves. A start that only waits for the cooldown is not a failure.

### Alerts

State bytes 18-19, one bit per alert; event `08` goes to every app when one comes on, and a push
to the phones that want it ([Push notifications](#push-notifications)).

| Bit | Alert | Clears |
|---|---|---|
| 0 | fuel low (at or below "Low fuel alert") | `TANK_SET` above it |
| 1 | fuel too low to run: stopped, starts refused | `TANK_SET` above "Stop at fuel left" |
| 2 | burn problem: a heater fault, the heater stopped by itself, or the heat exchanger cooling while it burns (the flame may be failing; a fading flame stops the run) | `ALERTS_CLEAR` |
| 3 | automatic starts disarmed after a problem | `AUTO_REARM` |
| 4 | the Bluetooth thermometer has sent no reading for 10 minutes | when it sends one |
| 5 | timers enabled, but the board does not know the time | when the time is set |
| 6 | frost protection on, but no room temperature | when there is one |
| 7 | battery low: the heater was stopped, starts refused | the supply 0.5 V above the cut-off |
| 8 | the glow plug's resistance has drifted: it may be wearing out | `ALERTS_CLEAR`, or `GLOW_NEW_PLUG` |
| 9 | a carbon burn-off is due | a burn-off run, or 20 minutes at high power |
| 10 | CO alarm: the heater was stopped | the heater's fault reset, once the air is clean |
| 11 | CO sensor fault: starts refused | the sensor reading plausible again, and the fault reset |
| 12 | glow plug type: a run started although the plug may not be the type set (a 12 V supply; on 24 V it would not start) | `GLOW_NEW_PLUG` |

`ALERTS_CLEAR(mask)` clears only the bits that say so (bits 2 and 8); the others follow their
cause. Home Assistant has an "Alerts" text sensor with the active alerts, for its own
notifications.

**Burn problem** detection: any fault the heater reports; the heater going to off by itself
during a run (not asked by anyone); while heating for 5 minutes or more, the heat exchanger 30 °C
below its highest reading of the last 2 minutes on two readings in a row, 10 s apart (a flame gone
out; after a pump cut or a fan rise, which cool it too, those 2 minutes start only 2 minutes after
the change); and a fading flame (a blocked
exhaust or air intake, fuel running out, coking), which smokes before any fault: the heat exchanger
15 °C below its highest reading of the last 5 minutes while the pump and the fan have been steady
for 10 minutes (no pump cut of more than 0.15 Hz, no fan rise of more than 10 %; it needs the
pump rate). If it then falls 25 °C below where the fading began, still on steady fuel, the board
stops the run (source board), raises the alert again with "the heater was stopped" and turns the
automatic starts off (bit 3), so frost protection or a timer does not start it into the same
fault. A recovery to within 7.5 °C ends the fading without a stop. (A heater burning normally at
steady fuel dips 5-10 °C every half hour or so: the 15 °C line stays clear of those.) Like the fast fall, the warning
and the stop each need two readings in a row (10 s apart) past their line, so one odd reading
neither raises the alert nor stops the heater.

### Bluetooth thermometers

Capability bit 7. The board listens for these thermometers' adverts (nothing to pair): BTHome v2
(Shelly BLU H&T and others; not encrypted ones), Xiaomi LYWSD03MMC and similar with the pvvx or ATC
custom firmware, Govee (H5072, H5074, H5075, H5101, H5102, H5104, H5174, H5177, H5179), SwitchBot
Meter, Meter Plus, Outdoor Meter and Meter Pro, RuuviTag (the RuuviTag and RuuviTag Pro in raw
mode, data formats 5 and 3, and in the older tags' "weather station" mode, formats 2 and 4; also
format 6), Qingping (CGG1, CGDK2, CGP1W and others), and Xiaomi models on their own firmware that
send their readings unencrypted (Mi Temperature and Humidity Monitor LYWSDCGQ; the LYWSD03MMC on its
own firmware encrypts them and needs the pvvx/ATC firmware). Listening is passive: about 9 % of the
radio's time, and about 31 % while `SENSOR_SCAN` runs (back to 9 % when it ends), so a thermometer
that advertises every few seconds shows up within the first 10-30 s of a scan.

- `SENSOR_SCAN(seconds)`: starts looking for that long (5-60 s; the list is emptied). `00` at
  once; the app then reads the list with `SENSOR_FOUND` as it fills.
- `SENSOR_FOUND(index)`: u8 index, u8 count, u8 flags (bit 0 still scanning, bit 1 the chosen
  one), u8[6] address (as printed: `A4:C1:38:…` is `A4` first), u8 kind (1 BTHome, 2 Xiaomi with
  custom firmware, 3 Govee, 4 SwitchBot, 5 RuuviTag, 6 Qingping, 7 Xiaomi), i8 RSSI dBm, i16
  temperature 0.1 °C, u8 humidity %, u8 battery % (`0xFF` not in its adverts), then its name
  (UTF-8, at most 20 bytes; may be empty). With nothing found, index 0 returns just index, count 0
  and flags. Up to 16 are listed.
- `SENSOR_USE(address, kind)`: chooses it (kept over restarts); all-zero address forgets it. It
  does not change the room source: `SET_ROOM_SOURCE(2)` does.
- `SENSOR_STATUS`: the chosen one, as in `SENSOR_FOUND` without the first three bytes and the
  name: u8[6] address (zero: none chosen), u8 kind, i8 RSSI, i16 temperature 0.1 °C (as the sensor
  sends it, no offset), u8 humidity, u8 battery; then u16 seconds since it was last heard (`0xFFFF`
  never).

No reading for 10 minutes: the reading is dropped (alert bit 4) and the room temperature falls back.

**Room temperature with three sources:** the chosen one; if it has no value, the first that has
one of the board probe, Home Assistant, the Bluetooth thermometer (State flags bit 2, "fallback").
Each source's offset setting is added to its readings.

## Maintenance, battery, CO and the event log

### Settings

| Id | Name | Unit | Range, default | Group, level |
|---|---|---|---|---|
| `0x160` | Low battery cut-off | V, 1 decimal | 0 or 9.0-28.0, 11.5 (0 = off) | Battery, basic |
| `0x161` | Low battery delay | s | 10-300, 30 | Battery, advanced |
| `0x170` | Carbon burn-off reminder | h | 0-500, 50 (0 = never) | Maintenance, basic |
| `0x171` | Carbon burn-off length | min | 10-30, 20 | Maintenance, advanced |
| `0x172` | Low power for the reminder | % | 10-80, 40 | Maintenance, expert |
| `0x173` | Glow plug drift alert | % | 0-80, 30 (0 = never) | Maintenance, advanced |

The app asks for the pump size (`0x130`) at onboarding: the same 22 mL and 28 mL pumps are sold for
2, 5 and 8 kW heaters. A pump's size is what it moves in 1000 strokes (a "22 mL" pump moves
0.022 mL a stroke), which is what the setting holds: the app asks for the size printed on the
pump, 22.0 or 28.0.

### Low battery

With "Low battery cut-off" set (the app asks for it at onboarding: 11.5 V suits a 12 V lead-acid
or LiFePO4 battery, 23.0 V a 24 V one):

- **Starts are refused** from every source while the supply is below the cut-off + 0.2 V, with `2F`
  "battery low (11.3 V): charge it first". A refused automatic start disarms frost protection and
  timers, as any refused automatic start.
- **A run is stopped** (a normal stop: burn-off and cooldown) when the supply stays below the
  cut-off for "Low battery delay" while heating or in fan-only mode, or below the cut-off − 1.0 V
  while starting (the glow plug's 8-10 A pulls any battery down; only a collapsing one goes that
  low). The cooldown after a stop is never cut short. Alert bit 7.
- Bit 7 clears when the supply is back 0.5 V above the cut-off.

### Glow plug health

At every start the board reads the glow plug's hot resistance (glow voltage / current, the median
over the plug's last 10 s on, once it has been on 15 s). The first 3 starts after a new plug (or
the first ever) set the plug's baseline. When 3 starts in a row read more than "Glow plug drift
alert" % away from it, alert bit 8 (a plug wearing out reads higher). Where the heater reports the
plug's drive only as averages, the figure is good only for comparing a plug with itself.

`GLOW_HEALTH` returns u16 baseline in mΩ (`0xFFFF` none yet), u16 last reading in mΩ (`0xFFFF`
none), u16 starts read since the new plug, u8 n, then the last n readings (u16 mΩ each, oldest
first, at most 16). `GLOW_NEW_PLUG` forgets the readings and starts a new baseline.

### Carbon burn-off

These heaters soot up when they run at low power for long. The board counts burner time at or
below "Low power for the reminder"; after "Carbon burn-off reminder" hours of it, alert bit 9 (the
app's Maintenance section offers the run). 20 minutes in a row at 90 % or more also counts as a
burn-off.

`BURNOFF_START(minutes)` (the app's Maintenance section, Home Assistant's "Carbon burn-off" button)
starts the heater at 100 % power for that long, then stops it and puts the user's heat mode and
power back. Only from off, with every start check; the run's source is the person who asked. State
byte 22 counts the minutes down. Off ends it early. A completed run clears the counter and bit 9.

### CO

The rev A2 board's CO sensor (MiCS-5524) is read and judged by the safety MCU, which stops the
heater itself and latches the fault: CO above limit (fault 11) or CO sensor fault (fault 14),
cleared only by `FAULT_RESET` once the air is clean (or the reading plausible) again. The ESP32
reports it (Telemetry byte 30, CO in ppm), raises alert bit 10 or 11, refuses starts while either
fault is latched, and asks for off itself if the MCU reports a CO alarm and the heater still runs.
A board without a CO sensor reports Telemetry byte 30 as 0 (not fitted) and CO as unknown.

Alarm thresholds: 300 ppm for 60 s, or 1000 ppm for 10 s (from the sensor's curve, Rs/R0 about
0.03 at 300 ppm and 0.01 at 1000 ppm: an alarm for really high CO only). Not a certified CO alarm:
the apps say so next to the reading.

### Event log

The board keeps its last 128 events (starts, stops and who asked, faults, alerts, automatic
starts, low battery, burn-off runs, glow plug readings, tank and clock changes, restarts, relay
sessions), over restarts. Each has a sequence number that only grows, so an app reads only what is
new:

`LOG_READ(first)` returns u32 newest sequence number (0: empty), u8 n, then n records (at most 7)
from `first` on (or the oldest kept, if `first` is older). Record, 16 bytes:

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | sequence number |
| 4 | u32 | UTC Unix seconds (0: the board did not know the time) |
| 8 | u8 | kind, below |
| 9 | u8 | source (the State byte 12 codes; 0 the board itself) |
| 10 | u16 | a |
| 12 | u32 | b |

| Kind | Event | a | b |
|---|---|---|---|
| 1 | run started | mode (1 heat, 2 fan only, 3 priming) | |
| 2 | stop asked | | |
| 3 | the heater stopped by itself | fault (0 none) | |
| 4 | heater fault | fault | the heater's own code |
| 5 | alert | alert bit | the reason it was raised ([Snapshots](#snapshots)) |
| 6 | board started | reset reason (ESP-IDF's code) | |
| 7 | automatic starts disarmed | | |
| 8 | automatic starts re-armed | | |
| 9 | low battery stop | | supply, mV |
| 10 | carbon burn-off run | 0 started, 1 finished | minutes |
| 11 | glow plug reading | | mΩ |
| 12 | tank set | % | |
| 13 | clock set | clock source | |
| 14 | relay session | phone key slot | 0 opened, 1 closed |

An app shows unknown kinds as "event n".

### Snapshots

When the board logs an alert (kind 5), a heater fault (kind 4) or "the heater stopped by itself"
(kind 3), it also keeps a snapshot: why, and the readings at that moment. Entries logged in the same
step (a heater fault and the alert it raises) share one snapshot. The board keeps the last 16, over
restarts, written to flash only on those events; a factory reset clears them.

**Reasons** (an alert record's `b`, and a snapshot's reason byte; an app shows an unknown one as
"reason n"):

| Code | Reason | Code | Reason |
|---|---|---|---|
| 1 | a heater fault (alert bit 2) | 10 | timers without the time (bit 5) |
| 2 | the heater stopped by itself (bit 2) | 11 | frost protection without a room temperature (bit 6) |
| 3 | the heat exchanger fell fast (bit 2) | 12 | battery low (bit 7) |
| 4 | the flame is fading: warning (bit 2) | 13 | glow plug drift (bit 8) |
| 5 | the flame faded: the heater was stopped (bit 2) | 14 | carbon burn-off due (bit 9) |
| 6 | fuel low (bit 0) | 15 | CO alarm (bit 10) |
| 7 | fuel too low to run (bit 1) | 16 | CO sensor fault (bit 11) |
| 8 | automatic starts disarmed (bit 3) | 17 | glow plug type (bit 12) |
| 9 | the Bluetooth thermometer not heard (bit 4) | | |

`LOG_DETAIL(seq, part)` answers for any log entry a kept snapshot covers; otherwise `12` ("no
readings were kept for this entry"). Part 0, the readings, 89 bytes:

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | the first log sequence number the snapshot covers |
| 4 | u8 | reason, above (the first entry's) |
| 5 | 39 bytes | [State](#state-1002), as it was then |
| 44 | 45 bytes | [Telemetry](#telemetry-1003), as it was then |

Part 1, the heat exchanger window the burn check judged, only for reasons 3, 4 and 5 (otherwise
`12`), 40 bytes:

| Offset | Type | Field |
|---|---|---|
| 0 | i16 | the window's top, whole °C (`0x8000` none) |
| 2 | u8 | n, the samples kept (at most 30, 10 s apart, oldest first) |
| 3 | u8[30] | each sample, whole °C below the top (255: 255 or more); past n, 0 |
| 33 | u8 | samples since the last fuel cut or air rise (or since heating began), at most 30 |
| 34 | u8 | the fuel reference, 0.1 Hz (`0xFF` none) |
| 35 | u16 | the air reference, rpm (`0xFFFF` none) |
| 37 | u16 | seconds the fuel and air had been steady |
| 39 | u8 | 0 |

## Board firmware updates

The phone app installs a new build of the board's own firmware (not the heater's), downloaded from
the release manifest for its board, **over Bluetooth only**: near the heater, on one bonded,
encrypted connection. The board accepts only images signed by the project's release key, checks
them before switching to them, and falls back to the previous build if the new one does not come
up. (Flashing from a computer with ESPHome, and the safety MCU's own update through the ESP32, are
separate.)

**Releases.** One manifest per board, keyed by board (`rev-a2`), at
`https://firmware.flamekeeper.app/<board>/manifest.json`:

```json
{"board": "rev-a2", "version": "1.2.3", "build": 1795000000, "size": 1512345,
 "sha256": "<64 hex>", "url": "https://firmware.flamekeeper.app/rev-a2/1.2.3.bin",
 "notes": "...", "min_protocol": [1, 0]}
```

`version` and `build` are what Info bytes 7-13 report once it runs. The app offers the update when
the manifest's version is newer than Info's (or the same version with a later build), and only if
it speaks `min_protocol`'s major. The image is the signed app image as published; `sha256` and
`size` are over its exact bytes.

**Rules.**

- Over Bluetooth only: `UPDATE_BEGIN` over Wi-Fi or the relay is refused with `28` ("firmware
  updates go over Bluetooth"). `UPDATE_STATUS`, `UPDATE_FINISH` and `UPDATE_ABORT` are allowed over
  every transport (FINISH only installs bytes that came over Bluetooth).
- Only with the heater off: `UPDATE_BEGIN` is refused with `20` unless the heater state is off, or
  the heater does not answer at all (a board that cannot reach its heater can still be fixed).
- While an update runs (State flag bit 4), every heater start is refused with `22`, from every
  source; frost protection and timers wait for it rather than disarming.
- One update at a time, known by its size and SHA-256. `UPDATE_BEGIN` with the same image resumes
  it from wherever it is, from any phone; a different image is refused with `22` until the kept one
  is dropped: after 5 minutes without data (State bit 4 clears; starts are allowed again), by
  `UPDATE_ABORT` from any phone, or by a restart of the board (the next `UPDATE_BEGIN` starts from
  0).
- Any bonded phone may update. Starting one changes nothing on the heater.

**Commands.**

| Op | Result data (`00`) | Refusals, in the order they are checked |
|---|---|---|
| `90` UPDATE_BEGIN (u32 size, u8[32] SHA-256) | u32 offset to send from (0, or where the kept update with the same size and SHA-256 stands), u16 the largest image bytes per data write on this connection (ATT MTU − 7, at most 508) | `13` (capability bit 8 missing), `11` (not 36 bytes), `28` (not over Bluetooth), `22` (an update is being installed); then the same image resumes with `00`; `22` (the heater's firmware is being updated), `20` (heater not off), `12` (size 0), `2A` (larger than the update slot), `22` (another image is kept), `2F` (the slot could not be prepared) |
| `91` UPDATE_STATUS | u8 state, u32 size, u32 bytes received, u8[32] SHA-256, u8 last result | `13`, `11` |
| `92` UPDATE_FINISH | (none): the board restarts about 1 s after this result | `13`, `11`, `2F` (no update under way), `22` (being installed), `2B` (bytes still missing: the update is kept, send the rest), `20` (heater not off: kept), `2B` (the SHA-256 does not match: dropped), `2C` (not signed by a trusted key, or not a valid image: dropped), `2F` (could not be installed: dropped) |
| `93` UPDATE_ABORT | (none): drops a kept update | `13`, `11` |

UPDATE_STATUS states: 0 none, 1 receiving, 2 every byte received, 3 checking and installing.
Last result (kept until the next `UPDATE_BEGIN`, over restarts): 0 nothing yet, 1 the build
installed by the last update is running, 2 the last update was installed but the board went back to
the previous build (it did not come up), 3 the last update was dropped or refused.

**Data.** App → board on characteristic `1006`, write without response, never answered:

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | offset of these bytes in the image |
| 4 | ... | image bytes (at most the chunk size `UPDATE_BEGIN` gave, and the app's own MTU − 7) |

The board takes a write only while receiving, at exactly the offset it expects next, with at least
1 image byte and not past the size, and writes it to the update slot as it arrives (there is no
second copy). Any other write is ignored, silently. A slot write that fails drops the update
(state 0, last result 3).

**The exchange: stop and wait.**

1. `UPDATE_BEGIN`; the board answers the offset to send from and the chunk size.
2. A block: from that offset, up to 16 data writes in order, each one chunk (the last write of the
   image may be shorter), without waiting between them beyond what the phone's Bluetooth stack
   makes it wait. A block is counted in writes, not bytes: each write is one event in the BLE
   stack's queue, which the thermometer scanner shares (3,840 bytes at MTU 247).
3. `UPDATE_STATUS` right after the block's last write. Bluetooth delivers a connection's writes in
   order and the board handles them in order, so the answer counts every write of the block that
   arrived.
4. On the status:
   - state 1 (receiving): the next block starts at `received`; data that did not arrive is simply
     sent again. When `received` has not grown over 3 blocks in a row, the update has stalled: the
     app sends `UPDATE_ABORT` and says so.
   - state 2 (every byte received): `UPDATE_FINISH`.
   - state 0 (dropped by the board): `UPDATE_BEGIN` again (from 0), at most 3 times in a run.
   - state 3 (being installed): wait, as for no answer.
   - no answer, or the link drops: wait for a Bluetooth link, then `UPDATE_BEGIN` again with the
     same image (it resumes).
5. `UPDATE_FINISH` checks that every byte arrived and their SHA-256, then the signature and the
   image itself (this takes a few seconds), selects the new build, answers, and restarts.

A 1.5 MB image takes a few minutes. While an update runs the app holds the phone to Bluetooth.

**After `UPDATE_FINISH`.** The board restarts into the new build: Bluetooth advertising is back in
about 5 s, Wi-Fi in about 10-20 s. The app reconnects on any link and reads Info: the new version
and build mean it is running. The new build confirms itself after 60 s of running normally; one
that crashes or restarts before then is replaced by the previous build at its next start (ESP-IDF
app rollback), which can take a few restarts: the app keeps trying to reconnect for 2 minutes, then
reads Info and `UPDATE_STATUS` (last result 2 = went back).

**Signing.** ESPHome's signed OTA (`esp32: signed_ota_verification`), with the image signed outside
the build (`esp32/tools/signing_key.sh`, used by `esp32/tools/release.sh` and
`esp32/tools/flash_signed.sh`); the private key never enters the build and reaches the signing
tool only through a pipe. ECDSA P-256 in the Secure Boot V2 signature format: the board trusts the
key in its running image's signature block, so its first USB image must be signed too. Every update
path checks it, including ESPHome's own OTA from a computer, so a board only ever installs builds
signed with that key. The public key is in `esp32/keys/`.

## Internet relay

The relay lets a paired phone reach its board from anywhere, and lets the board send push
notifications, through a server both can reach (`relay.flamekeeper.app` by default). Its wire
format, close codes, limits and push registrations are in the relay's own protocol document
([flamekeeper-heater/relay](https://github.com/flamekeeper-heater/relay), `PROTOCOL.md`); this
section keeps only what the board does. The relay is **off by default**; the user switches it on
from the app (`RELAY_SET`). With it off, nothing leaves the home network. The relay only forwards:
every app frame stays the end-to-end encrypted Wi-Fi frame, so the relay cannot read or send
commands, State or settings.

**The board's identity.** At the first `RELAY_SET 1` the board makes a P-256 key pair from its
hardware random generator and keeps it on the board. Its **board id** is the first 16 bytes of
SHA-256 of the 65-byte uncompressed public key, written as 26 lowercase base32 characters
(RFC 4648 alphabet `a-z2-7`, no padding, the last character's two spare bits zero). Example: the
generator point G gives `ngf6uy64isruizr76fbjv2qqqq`. A factory reset deletes the key pair, the
on switch and the host, so the id changes and every phone is cut off with the reset.

**Per phone key** ([Wi-Fi keys](#wi-fi-keys); browser keys never use the relay), both derived by
the phone itself from its key's secret, so nothing new crosses Bluetooth:

- relay token `T = HKDF-SHA256(key = secret, salt = empty, info = "fk1 relay")`, 32 bytes: the
  phone's ticket to open a channel. The board gives the relay `SHA-256(T)` per slot (the roster).
- push key `P = HKDF-SHA256(key = secret, salt = empty, info = "fk1 push")`, 16 bytes: decrypts that
  phone's pushes.

The phone key **slot** is the key's place in the board's table of phone keys (0-7).

### Commands

| Op | Result data (`00`) |
|---|---|
| `A0` RELAY_STATUS | u8 on, u8 state, u8[16] board id (zeros before the key pair exists), u32 seconds since the board last connected to the relay (`0xFFFFFFFF` never), u8 phones connected through it, u8 n, the host (n = 0 the default) |
| `A1` RELAY_SET (u8 on, u8 n, host) | (none). On makes the key pair if there is none and connects; off closes the relay connection and every phone on it (the result goes out first). Allowed over the relay too: the app warns first that the phone loses the heater until it is near it again |

Relay states: 0 off, 1 connecting (or waiting to retry), 2 connected, 3 refused by the relay
(its sign-in failed), 4 no internet (Wi-Fi down, or the host does not answer). Home Assistant has a
"Relay" text sensor with the same words. A host is a name or an address with an optional `:port`
(letters, digits, `.`, `-`, `:`; at most 63 bytes), otherwise `12`. The board keeps it, dials it and
signs it (AUTH) as the relay sees the URL: lower case, without a default `:443` (`Relay.Example.com:443`
becomes `relay.example.com`), and the default host as n = 0. RELAY_STATUS returns it that way.

### The board's connection

The board keeps one outbound WebSocket over TLS while the relay is on and Wi-Fi is up
(`wss://<host>/v1/board/<id>`, subprotocol `flamekeeper-relay.v1`). TLS checks the server's
certificate against the root certificates of the authorities behind Cloudflare's edge
certificates (and most other public sites), compiled into the firmware.

- **Sign-in:** the board answers the relay's CHALLENGE at once with AUTH (its public key and an
  ECDSA P-256 SHA-256 signature over `"fk1 relay board"` ‖ the challenge ‖ u8 host length ‖ the
  host as the board dialled it, with `:port` if any).
- **Roster:** right after AUTH, and again whenever a phone key is issued or revoked (`WIFI_KEY`,
  `FORGET_ME`, a factory reset), the board sends ROSTER: each phone key's slot and `SHA-256(T)`. An
  empty roster is sent too.
- **Keepalive:** a text `ping` every 45 s, which the relay answers with `pong`; two unanswered
  close and reconnect.
- **Reconnect:** after a failure, 2 s, then doubling to 5 minutes, ±20 % at random; back to 2 s
  after 10 minutes connected. After close code 4001 (another board took this id) it waits 60 s.
- Nothing goes out while no phone is on it except `ping`, the roster when it changes, pushes and
  live card records.

**A phone's session on a channel** is a Wi-Fi session ([Frames](#frames), [Sign-in with a
key](#sign-in-with-a-key)) with three differences: Info is not sent in the clear first (the board
sends Hello when the relay opens the channel); only sign-in with a key is accepted (never the web
password); and the key must be the one in the slot the relay named when it opened the channel,
otherwise `24` and the channel closes. After the sign-in the board sends Info sealed (type `01`),
then State and Telemetry as usual. Everything else is the same: the same commands with the same
checks (run source 7), the same limits, and no firmware updates (`28`). Up to 3 channels; a close
from either side ends that session (the board sends 1000 for a normal end, 1008 for a protocol
error, 4100 when the phone's key was removed).

## Push notifications

The board decides what to notify, filters it per phone and encrypts it per phone; the relay only
adds the phone's push token and hands it to the push service. The relay sees when and how urgent,
never what.

### Preferences, per phone key

`PUSH_PREFS` (u16 alert mask, u8 event mask, u8 flags) and `PUSH_PREFS_GET` (the same four bytes,
as stored) act on the phone's own key: the key it signed in with over Wi-Fi or the relay, or over
Bluetooth the key issued to that bonded phone (`24` if it has none: ask `WIFI_KEY` first). They are
kept on the board with the key and deleted with it; a new key starts with the defaults.

- Alert mask: the [alert bits](#alerts). Default every alert but bit 4 (`0x1FEF`). The CO alarm
  (bit 10) is not a preference: the board sends it to every phone whatever its bit says.
- Event mask: bit 0 frost protection started the heater, 1 frost protection's run ended, 2 a timer
  started the heater, 3 a timer's run ended, 4 someone else started the heater (another phone, Home
  Assistant, the heater's own controls), 5 someone else stopped it (or Home Assistant's failsafe),
  6 the board restarted; bit 7 is ignored. Default none.
- Flags: bit 0 notifications on (off: nothing but the CO alarm); bit 1 the live card on; bit 2
  live cards for the board's own runs (timer, frost protection, burn-off); bit 3 live cards for
  others' runs (another phone, Home Assistant, the heater's own controls). Default `0x07`. Bits 4-7
  are 0 (else `12`). A run this phone started always gets its card while bit 1 is on.

### What is pushed

- **An alert bit rising** (one push per rise), and the events a phone asked for.
- **Coalescing:** what rises within 3 s of the first goes out as one push, led by the most urgent.
- **Hold-off:** a bit that rises again within 60 minutes of its last push (6 hours for bit 4) is
  not pushed again; never for bits 10 and 11.
- **CO alarm repeats:** while bit 10 stays on, again 5 and 10 minutes after the first, unless an
  app signs in to the board meanwhile.
- **Relay down:** the board keeps the latest push per alert bit and sends it as soon as it is
  signed in to the relay again, if the alert is still on, flagged delayed and with its original
  time.
- **Who:** every slot with a phone key whose preferences take it; the relay drops the blobs of
  phones that have no push registration.

Class (the urgency the relay sees and maps to the phone's interruption levels): 3 alarm (bit 10),
2 urgent (bits 11, 7, 1, 2, 3, 6), 1 notice (bits 0, 5), 0 quiet (bits 8, 9, 12, 4 and every
event). A push's class is its most urgent item's.

### The blob

Per slot: `blob = nonce (12 random bytes) ‖ AES-128-GCM(P, nonce, payload, AD = board id (16 bytes,
raw) ‖ class) ‖ tag (16)`. The class is in the associated data, so a relay that changes it breaks
the tag. Payload, 27 bytes before the text:

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | sequence number: the event log record of the leading item (only grows, over restarts) |
| 4 | u32 | UTC Unix seconds of that record (0: the board did not know the time) |
| 8 | u8 | class |
| 9 | u8 | what leads: 0 an alert, 1 an event |
| 10 | u8 | its code: the alert bit, or the event bit (event mask) |
| 11 | u8 | run source (State byte 12) behind it (events; 0 for an alert) |
| 12 | u16 | alerts active now |
| 14 | u8 | flags: bit 0 delayed (the relay was down), bits 1-2 repeat number (CO alarm) |
| 15 | u8 | fuel left, % (`0xFF` unknown) |
| 16 | u16 | supply, 0.01 V (`0xFFFF` unknown) |
| 18 | i16 | room temperature, 0.1 °C (`0x8000` unknown) |
| 20 | i16 | target temperature, 0.1 °C (`0x8000` unknown) |
| 22 | u32 | the run's end, UTC: its timer's, or `RUN_UNTIL`'s (0 none) |
| 26 | u8 | n, then n bytes of UTF-8 text (at most 48): the alert's reason, as event `08` |

The longest blob is 12 + 27 + 48 + 16 = 103 bytes; with 8 phones a PUSH stays under 1 KB.

## Live card

The phone's live card (an iOS Live Activity) follows a run while the app is closed: the board sends
LIVE to the relay, which turns it into the card's pushes. The LIVE header (event, flags, run id,
the slots it is for) is the relay's to read; the record after it is not encrypted, and the relay
passes it to the phones' cards unread.

**When.** A card's run begins when the heater leaves off for starting, heating or fan only (never
priming or a firmware update) and ends when it is off again or another run begins.

- `start` (event 0) once, important.
- `update` (1) on a change of state, run kind, run source, fault, alerts, target, power or run end,
  or the room by 0.5 °C; otherwise every 120 s. Important on a state change, a new alert bit or a
  new fault.
- `end` (2) at off, important, with the final content; flags bit 1 when the run ended after a
  fault or with alert bit 1, 2, 7 or 10 on (the relay then keeps the ended card longer).
- At most one LIVE every 20 s, or 5 s for a start, an important change, or a change of target,
  power, run kind or run end: a change inside the window waits and goes with the latest content at
  its end. An end never waits.
- Relay down: nothing is queued. When the board is signed in again, it sends the current content at
  once as an important update (the relay may have ended the card meanwhile), or the end if the run
  ended meanwhile.

**Who.** Every slot with a phone key whose `PUSH_PREFS` flags have bit 1: the phone that started
the run (its key slot: the phone whose `SET_MODE` or `BURNOFF_START` started it, over any
transport) always; a timer's or frost protection's run with flags bit 2; any other source with
bit 3. The relay drops the slots that have no card.

**Record**, 28 bytes:

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | UTC Unix seconds of the content (0: the board does not know the time) |
| 4 | u8 | heater state (State byte 9) |
| 5 | u8 | run kind: 0 heat by temperature, 1 heat by power, 2 fan only, 3 carbon burn-off |
| 6 | u8 | run source (State byte 12) |
| 7 | u8 | fault (State byte 11) |
| 8 | u16 | alerts (State 18) |
| 10 | i16 | room temperature, 0.1 °C (`0x8000` unknown) |
| 12 | i16 | target temperature, 0.1 °C (`0x8000` unknown) |
| 14 | u8 | power, % (State byte 3) |
| 15 | u32 | the run's end, UTC (State 24-27: its timer's or `RUN_UNTIL`'s; 0 none) |
| 19 | u32 | the carbon burn-off's end, UTC (0 none) |
| 23 | u32 | when the run started, UTC (0 unknown) |
| 27 | u8 | fuel left, % (`0xFF` unknown) |

`dump_vectors.cpp` has a record, one with everything unknown, and whole start, update and end
frames for slot 2 (`liveStart`, `liveUpdate`, `liveEnd`).

## Where it is in the code

- `esp32/components/flamekeeper_ble/`: its own GATT application and attribute table on ESPHome's
  `esp32_ble` event queue (Bluedroid), so the stack enforces the permissions above. GATT and GAP
  callbacks only queue events; the main loop handles them, as every other entity change runs.
  ESPHome's `esp32_ble_server` runs next to it for Device Information.
- `esp32/components/flamekeeper/ble_protocol.*` (records, command handler) and `pairing.*` (window,
  PIN) are plain C++ with no ESPHome or ESP-IDF headers, host-tested in
  `esp32/components/flamekeeper/tests/`. A command becomes a `Controller` request with its source,
  exactly as an entity change becomes one with source "Home Assistant".
- **Wi-Fi transport:** `esp32/components/flamekeeper_net/`, on ESP-IDF's `esp_http_server` (with
  `CONFIG_HTTPD_WS_SUPPORT`), serving the page and the WebSocket. The server task only queues
  frames; the main loop runs the sessions. Records come from the same `encode_*()` functions and
  every command goes to the same `handle_command()` as on Bluetooth. The sign-in, the key tables,
  the lockout and the frame encryption are plain C++ on mbedTLS (`flamekeeper/wifi_session.*`,
  `access.*`, `crypto.*`), host-tested with the mbedTLS sources ESP-IDF ships. EC J-PAKE needs
  `CONFIG_MBEDTLS_ECJPAKE_C`, which the flamekeeper component sets.
- **Board firmware updates:** `flamekeeper/board_update.*` (plain C++, host-tested: offsets,
  resume, the heater-off rule, timeouts, the checks at the end, rollback detection) writes through
  ESPHome's OTA backend (`EspUpdateFlash` in `board_services.cpp`), which checks the signature and
  selects the new build; ESPHome's `safe_mode` confirms it after 60 s, ESP-IDF's app rollback brings
  the old one back otherwise. Built only with `signed_ota_verification`.
- **Internet relay:** `flamekeeper/relay.*` (identity, ids, tokens, the channel-0 messages) and
  `relay_client.*` (sign-in, roster, channels, back-off, status) are plain C++, host-tested. The
  socket is `flamekeeper_net/relay_socket.*`: esp-tls in a task that exists only while a connection
  is wanted; the server is checked against `flamekeeper_net/relay_roots.pem` through ESP-IDF's
  bundle verifier; mbedTLS buffers are dynamic. Relay channels are sessions of the Wi-Fi transport,
  so every command and record path is the same code.
- **Push notifications and the live card:** `flamekeeper/push.*`, plain C++, host-tested: it reads
  the event log (alerts, runs started and stopped, board started), so every trigger has the log
  record's sequence number and time; preferences are bound to the key id they were set for.
- **Board settings:** `flamekeeper/board_services.*`, owned by the hub and used by both
  transports: the PIN source, the key and password storage, Wi-Fi setup with the 60 s revert,
  Home Assistant on demand, factory reset (ESPHome's preferences reset, every bond removed).
- **Wi-Fi setup:** ESPHome refuses a `wifi:` block with no network, so the board's profile names a
  placeholder network with `enable_on_boot: false`; the radio stays off until `WIFI_SET` saves the
  app's network (ESPHome's `save_wifi_sta()`) and enables Wi-Fi.
- **Home Assistant on demand:** the hub sets up before the API; with Home Assistant off it marks the
  API failed, so it never listens, and takes its `_esphomelib` service out of mDNS. The key is set
  with `APIServer::save_noise_psk()` and read back from ESPHome's preferences.
