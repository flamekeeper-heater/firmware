# Design

## Two processors

| | ESP32-C6 (this firmware) | STM32G030 safety MCU |
|---|---|---|
| Start and stop sequences, fault checks, glow plug, pump and fan outputs | asks | owns |
| Heater parameters (ignition, glow, pump and fan limits, cooldown) | shows and forwards them | owns, keeps in flash |
| Thermostat | owns: sends power and starts or stops | follows |
| Altitude fuel trim | owns: adds it to the fuel trim | applies it |
| Room temperature, barometer, apps, Home Assistant | owns | |
| CO alarm | alerts and refuses starts | reads and judges the sensor, stops the heater |

The safety MCU is the only way to the heater. A hardware heartbeat from its safety loop enables
the glow plug and pump outputs, so a hung MCU switches them off whatever the ESP32 does.

## ESP32 firmware

```
apps (Bluetooth, Wi-Fi, relay)    Home Assistant entities    board inputs
              \                           |                       /
               +---------------  Controller  ---------------------+
                                     |
                                heater.h (Heater)
                                     |
                         link_client  ->  safety MCU (UART)
```

- **Every request goes through the controller** (`controller.*`), whatever its source. Same
  checks, same refusals, same log line. It also runs the thermostat, the altitude trim, the room
  temperature sources and the Home Assistant failsafe.
- **`heater.h` is the only face of the heater.** Nothing above it knows which backend is in use.
  `safety_link` is the board's backend: `link_client.*` speaks the link protocol on UART1. Another
  heater can be supported with another `Heater` implementation, or with an emulator that answers
  the link client in memory.
- **The app protocol** (`ble_protocol.*`) is one encoder and command handler for both transports:
  `flamekeeper_ble` (Bluetooth GATT on Bluedroid) and `flamekeeper_net` (the HTTP server, the
  encrypted WebSocket and the relay connection). Sign-in and frame encryption are in
  `wifi_session.*` and `crypto.*`, on mbedTLS.
- **Board services** (`board_services.*`): the PIN, Wi-Fi setup, Home Assistant on demand, firmware
  updates (`board_update.*`) and the factory reset, with everything they keep in NVS.
- **Automation** (`automation.*`): frost protection, timers, fuel meters and tank, alerts,
  maintenance and the event log. A refused or failed automatic start disarms frost protection and
  timers until the user re-arms them; they never retry by themselves.
- **Bluetooth thermometers**: `flamekeeper_thermo` listens to ESPHome's BLE tracker, and
  `ble_thermo.*` decodes the adverts.

The core in `components/flamekeeper/` is plain C++ with no ESPHome or hardware dependency, so it is
host-tested in `tests/run.sh`. The ESPHome glue is the hub (`flamekeeper.*`) and the entity
platforms.

**Rules the code keeps:**

- A start needs an explicit request. Nothing resumes heating after a restart of either processor.
- A dropped Bluetooth or Wi-Fi connection changes nothing on the heater.
- Only runs started from Home Assistant stop when Home Assistant is unreachable (15 minutes).
- The ESP32 never drives the safety MCU's reset and boot pins except to update it, and only after
  the MCU agrees (heater off and cold).

## Link to the safety MCU

UART at 115200 8N1, point to point. The ESP32 asks, the MCU answers: one request, one reply. The
codec is `common/link/`, shared by both sides.

```
ESP32 -> MCU:  7A  len  seq  op  payload...          crc_hi crc_lo
MCU -> ESP32:  7B  len  seq  op  result  payload...  crc_hi crc_lo
```

`len` counts every byte before the CRC; a frame is at most 64 bytes. The CRC is CRC16-Modbus,
high byte first. `seq` (1-255) is echoed in the reply, and result codes are the app protocol's.

| Op | Request | Reply |
|---|---|---|
| `01` HELLO | the ESP32's link version | Info: version, MCU kind, firmware version, capabilities, parameter count, reset reason, start-up check results |
| `02` POLL | the desired state: start number, mode, power, fuel and fan trims | Status: heater state, fault, flags, start number acted on, measurements |
| `03` DETAIL | | slower measurements: stage temperatures, glow and fan voltage and current, pump current, CO |
| `20`-`26` | parameters: describe, get, set, apply, discard, defaults, help | |
| `30` FAULT_RESET | | clears a latched fault once its cause is gone |
| `40` UPDATE_PREPARE | | agrees to an update; refuses starts until reset |

**Timing.** The ESP32 polls every 100 ms; the MCU replies within 20 ms. Three missed replies and the
ESP32 reports "no reply from the heater".

**Start number.** Every POLL repeats the whole desired state, so a lost frame costs nothing. The
ESP32 increments a start number for each new start a user or the thermostat asks for. The MCU
starts only on a number it has not acted on, while it is off. A repeated poll never starts the
heater twice, a heater that stopped by itself stays off, and after an MCU reset the first number it
sees counts as already acted on.

**Dead-man.** If the MCU hears no valid frame for 2 s while the heater runs, it burns off and cools
down on its own, then stays off until a new start number arrives.

**Versioning.** HELLO carries the link major and minor. A minor only adds what the other side can
ignore. Either side refuses to run with a major it does not know.

## Burn sequence (safety MCU)

`stm32/core/burn.*` runs the heater from measurements and says what the fan, pump and glow stages
should do, one pass of the safety loop at a time:

```
off -> self-test -> preheat -> ignition -> warm-up -> running -> burn-off -> cooldown -> off
                      ^            | no flame
                      +-- purge <--+  (one retry, then a start fault)
off -> fan only -> off          off -> priming -> off
```

- The fan must turn before anything else: the glow plug only heats with measured airflow, and a
  fan that does not reach its speed ends the start.
- Glow on-time is capped per attempt and per 10 minutes, so a start that would exceed the cap
  waits.
- Flame is seen as a rise of the heat exchanger temperature; a fall from the run's peak is a
  flame-out. Every stop after fuel has gone in burns off and cools down; a fan stall stops
  everything at once.
- Overheat (firmware limit or the hardware latch), CO, an open sensor or the supply out of range
  end the run with a fault. The latch and CO faults need a fault reset once their cause is gone.
- After a reset with a hot heat exchanger, it cools down first; nothing restarts by itself.

Every timing and limit is a named parameter (`burn_params`). The defaults are provisional until
they are tuned on a real heater.

`stm32/core/safety.*` joins it to the link: the start-number rule, the dead-man, the status and
the update handshake.

## Firmware updates and signing

The C6 build verifies every update against the key in its running image's signature block
(ECDSA P-256, Secure Boot V2 format), so the first image, flashed over USB, must be signed too.
`tools/signing_key.sh` signs and verifies images; the project's release key stays off this
repository, and `keys/` holds its public half. `tools/release.sh` builds, signs and writes the
release manifest.

The safety MCU is updated by the ESP32 through the STM32's ROM bootloader on the same UART, after
`UPDATE_PREPARE`.

## CO alarm

`common/co/` judges the MiCS-5524 metal-oxide sensor on the safety MCU, with integer maths only.
It warms the sensor up, learns its clean-air resistance, and alarms on the resistance ratio. On an
alarm the MCU stops the heater and latches the fault; an implausible reading latches a sensor fault
and refuses starts. It is not a certified CO alarm.

## Tests

- `esp32/components/flamekeeper/tests/run.sh`: the core on the host, with sanitizers, against a
  simulated heater and a simulated safety MCU.
- `stm32/tests/run.sh`: the safety core (`stm32/core/`): the burn sequencer against a heater model
  (whole cycles, every way a run ends, the glow caps and the airflow rule), and end to end with the
  ESP32's real link client over a byte wire (dead-man, an MCU reset mid-run).
- `common/*/tests/run.sh`: the link codec and the CO alarm.
- `esp32/tools/boot_test.sh`: boots a compiled image in Espressif's QEMU and fails on a crash or a
  restart. A C6 build checks its own signature at start-up, so pass the signed app with `--app`.
  The C6's log goes to USB Serial/JTAG, which QEMU does not emulate, so a clean C6 run ends with
  exit code 2 (no crash, setup not confirmed).
