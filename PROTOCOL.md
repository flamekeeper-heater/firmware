# App protocol

How apps talk to the board: the Flamekeeper phone app, and the board's own web page in a
browser. The same records and commands travel over two transports:

| Transport | Used for | Carries |
|---|---|---|
| Bluetooth LE, one GATT service | near the heater, and the first setup | each record in its own characteristic |
| Wi-Fi, one WebSocket on the board | the same network, the web page, the internet relay | each record in an encrypted binary frame |

There is one set of records, opcodes and result codes, and one command handler
(`ble_protocol.cpp`). Every command becomes the same controller call a Home Assistant entity makes,
so it meets the same checks and is refused for the same reasons.

The exact field layouts are in `esp32/components/flamekeeper/ble_protocol.h` and `.cpp`, and
`tests/dump_vectors.cpp` prints their exact bytes as JSON.

**Versioning.** Info bytes 0-1 hold the protocol major and minor (1.0). Until the app ships, layouts
change in place. After that, a minor only adds what older apps can ignore (trailing bytes, new
opcodes, codes and bits), and anything else is a new major.

## Encoding

Little-endian, fixed-size records, new fields only at the end. Fixed-point values are integers with
a stated scale; temperatures are always °C. "Unknown" is `0x8000` (i16), `0xFFFF` (u16), `0xFF` (u8),
`0x80000000` (i32) and `0xFFFFFFFF` (u32).

## Bluetooth LE

The board advertises the Flamekeeper service UUID, with the device name in the scan response, and
accepts up to 3 connections. Base UUID `81ceXXXX-024c-414c-9ca7-e0972fed52ee`:

| UUID | Characteristic | Properties | Security |
|---|---|---|---|
| `1000` | Flamekeeper service | | |
| `1001` | Info | read | none |
| `1002` | State | read, notify | bonded, encrypted |
| `1003` | Telemetry | read, notify | bonded, encrypted |
| `1004` | Command | write | bonded, encrypted |
| `1005` | Result | indicate | bonded, encrypted |
| `1006` | Update data | write without response, notify | bonded, encrypted |

Device Information (`0x180A`) gives the manufacturer, model and firmware version. Apps should
negotiate an ATT MTU of at least 135: records are up to 132 bytes and are never split.

**Pairing.** LE Secure Connections with passkey entry, bonding required. The passkey is the board's
6-digit PIN, printed on its label and changeable from the app. A new phone can pair only while the
pairing window is open: for 2 minutes after power-up, a short press of BOOT, or the Home Assistant
"Allow Bluetooth pairing" button. The window closes after one new phone or three wrong PINs. After
five failed pairings, power-up no longer opens it. Up to 8 phones can be bonded.

## Records

- **Info** (`1001`, readable by anyone): protocol version, board type (2 = Flamekeeper rev A2),
  pairing window, capability bits, key and bond counts, Wi-Fi and Home Assistant state, firmware
  version and build time. A missing capability bit means the app hides the control, and the board
  refuses the command with `13`.
- **State** (`1002`): what is asked of the heater and what the controller is doing: mode, heat mode,
  power, target and room temperature, heater state, controller status, fault, who started the run,
  alerts, automatic starts, fuel level, the run's identity, and the number of the newest entry in the
  heater's fault log. Notified on change, at most 4 times a second.
- **Telemetry** (`1003`): measurements: heat exchanger and intake temperatures, supply voltage, fan,
  pump and glow plug, CO, altitude and pressure, running times, and each room temperature source.
  Notified every second while the heater runs, every 10 s otherwise.

Heater states: 0 unknown (no reply), 1 off, 2 starting, 3 heating, 4 fan only, 5 stopping,
6 cooling down, 7 priming, 8 updating firmware.

## Commands and results

The app writes one command per write: request id (1-255), opcode, arguments. The outcome comes back
as an indication on Result: request id, opcode, result code, and data. A refusal carries its reason
as text. Send the next command after the previous result.

| Opcodes | Group |
|---|---|
| `01`-`0B` | Heater: SET_MODE, SET_HEAT_MODE, SET_POWER, SET_TARGET, SET_ROOM_SOURCE, PRIME_START (needs the confirmation byte `A5`), PRIME_STOP, FAULT_RESET, HEATER_LOG, HEATER_LOG_CLEAR |
| `20`-`27` | Parameters: DESCRIBE, GET, SET, APPLY, DISCARD, GROUP, HELP, DESCRIBE_ID |
| `30`, `32` | Access: FORGET_ME, WIFI_KEY (Bluetooth only) |
| `40`, `41` | PIN_SET, WEB_PASSWORD_SET |
| `50`-`52` | Wi-Fi: WIFI_STATUS, WIFI_SET, WIFI_OFF |
| `60`-`63` | Home Assistant: HA_STATUS, HA_SET, HA_KEY, HA_KEY_NEW |
| `70`-`7C` | Clock, meters, tank, alerts, timers, event log, glow plug health, carbon burn-off |
| `80`-`83` | Bluetooth thermometers: SENSOR_SCAN, SENSOR_FOUND, SENSOR_USE, SENSOR_STATUS |
| `90`-`93` | Board firmware update: UPDATE_BEGIN, UPDATE_STATUS, UPDATE_FINISH, UPDATE_ABORT |
| `A0`-`A4` | Internet relay and push notifications |

Opcode values and argument layouts are in `ble_protocol.h` (`namespace op`).

| Result | Meaning |
|---|---|
| `00` | done, or accepted and under way |
| `01` | stored, not acting yet |
| `10`-`13` | unknown opcode, wrong length, out of range, not supported by this heater |
| `20`-`23` | refused: heater not off, heater not answering, busy, confirmation missing |
| `24`-`29` | sign-in and access: key unknown, wrong password, locked out (u16 seconds left), key table full, Bluetooth only, no web password |
| `2A`-`2C` | update: image too large, SHA-256 mismatch, not signed by a trusted key |
| `2F` | refused for another reason (see the text) |

Apps treat an unknown result code as a refusal. **Events** use request id 0: parameters applied or
saved, a parameter changed elsewhere, the failsafe stopped the heater, a Wi-Fi change finished, a
fault reset answered, a factory reset answered, an alert came on.

**The heater's fault log** (capability bit 11): the safety MCU's own record of its last 32 faults,
dead-man stops and resets, kept over its resets. HEATER_LOG (`0A`) takes the first entry wanted,
counted back from the newest (u8, 0 = the newest), and returns the number of entries in the log
(u8), then up to 8 entries, newest first, of 14 bytes each:

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
HEATER_LOG_CLEAR (`0B`) empties it, in the safety MCU too.

**Parameters.** The heater's parameters and the board's own settings come as one table the board
describes (name, unit, range, step, menu, level), so the app holds no list of its own. Heater
parameters are staged with PARAM_SET and sent with PARAM_APPLY; board settings apply at once.

## Wi-Fi transport

A new board has Wi-Fi off. The app sets the network and a web password over Bluetooth. A failed
change never strands the board: it returns to the old network after 60 s.

The board serves HTTP on port 80: `GET /` and the web page's files, and `GET /api/v1`, a WebSocket
with subprotocol `flamekeeper.v1`. It answers only requests addressed to its IP address or
`<name>.local`, and accepts WebSocket origins only from itself or the app. It announces
`_flamekeeper._tcp` over mDNS.

**Frames.** Each WebSocket message is one binary frame: a record type byte, then the record. `01`
Info and `10`-`15` (sign-in) travel in the clear. Everything else is encrypted with AES-128-GCM:
type byte (authenticated) ‖ ciphertext ‖ 16-byte tag, with a per-direction 4-byte salt and a
64-bit frame counter as the nonce. A frame that does not verify closes the connection.

| Type | Content |
|---|---|
| `01` | Info |
| `02` / `03` | State / Telemetry |
| `04` / `05` | Command / Result and events |
| `06` | update data and progress |
| `10` | Hello: the board's 16-byte nonce |
| `11`-`15` | sign-in |

**Wi-Fi keys.** Apps sign in with a key: a 16-byte id and a 32-byte secret. A bonded phone gets its
key over Bluetooth (`WIFI_KEY`), so the user enters nothing more. A browser gets one by signing in
with the web password. There are 8 keys of each kind.

- **Sign-in with a key:** the app sends its key id, its nonce and an HMAC-SHA256 over both nonces;
  the board proves it knows the key the same way. HKDF-SHA256 then derives the two session keys and
  salts.
- **Sign-in with the web password:** EC J-PAKE on P-256 (mbedTLS), then key confirmation, and the
  board issues a browser key. After 5 wrong passwords the board waits 1 minute, doubling up to an
  hour, and password sign-ins start at most every 5 s.

Someone on the network cannot read or forge traffic, or pose as the board to the app. The web
page itself is served over plain HTTP, so on a hostile network it could be replaced; the phone app
does not have that weakness.

## Internet relay

Off by default and switched on from the app. The board keeps one outbound WebSocket over TLS to
the relay (`relay.flamekeeper.app` by default), and paired phones reach it through there. The relay
only forwards the same end-to-end encrypted Wi-Fi frames, so it cannot read or send commands.
Each phone derives its relay token and push key from its own Wi-Fi key, so nothing new crosses
Bluetooth.

**Push notifications** are chosen, filtered and encrypted per phone by the board; the relay only
hands them to the phone's push service.

## Board firmware updates

The app sends a new build over either transport, with the heater off. The board accepts only images
signed with the key that signed its running image (ECDSA P-256, Secure Boot V2 format), checks the
SHA-256 and the signature before switching, and falls back to the previous build if the new one
does not come up. An interrupted update resumes where it stopped. Heater starts are refused while
an update runs.

Official releases are published per board at `https://firmware.flamekeeper.app/<board>/manifest.json`:
version, build time, size, SHA-256, image URL, notes and the minimum protocol version.

## Home Assistant

Off on a new board. Switching it on from the app creates the API encryption key and shows it in
the app; the user adds the board in Home Assistant with it. Only runs started from Home Assistant
stop when Home Assistant has been unreachable for 15 minutes.

## Factory reset

Hold BOOT for 10 to 30 seconds while the board runs. It stops the heater and forgets every bond, key,
password and network. The PIN goes back to the label's, settings and the safety MCU's parameters go
back to their defaults, and timers, meters and the event log are cleared. Only the firmware stays.
