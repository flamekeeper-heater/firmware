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
  the MCU agrees (heater off and cold), or to finish its own update that the MCU agreed to.

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
| `31` GLOW_NEW_PLUG | | a new glow plug is fitted: its remembered type check goes (Status flag bit 6 reports a start that ran with a plug-type warning) |
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
off -> pre-check -> fan check -> preheat -> ignition -> warm-up -> running -> burn-off -> cooldown -> off
                                   ^            | no flame
                                   +-- purge <--+  (one retry, then a start fault)
off -> fan only -> off          off -> pre-check -> priming -> off
```

- The fan must turn before anything else: the glow plug only heats with measured airflow, and a
  fan that does not reach its speed counts as a stall.
- Glow on-time is capped per attempt and per 10 minutes, so a start that would exceed the cap
  waits.
- Flame is seen as a rise of the heat exchanger temperature; a fall from the run's peak is a
  flame-out. Every stop after fuel has gone in burns off and cools down; a fan stall stops
  everything at once.
- A stalled fan tries again after 5 s: a run that had fuel cools down, an earlier start or a
  fan-only run carries on. After two retries the next stall locks the fan out until a fault reset;
  the count forgets after 10 minutes of normal turning.
- Overheat (firmware limit or the hardware latch), CO, an open sensor or the supply out of range
  end the run with a fault. The latch and CO faults need a fault reset once their cause is gone.
- After a reset with a hot heat exchanger, it cools down first; nothing restarts by itself. A fault
  reset that finds it hot (after a fan lockout) cools it down too; a fan still jammed locks out
  again. The first heat start within 4 minutes of a reset runs the fan 30 s longer before the
  glow, so the glow switches cool from a run the reset may have cut short (provisional).
- Fuel follows air: the pump falls at once and rises slowly, the fan rises at once and falls
  slowly, so the fuel never outruns the air.
- A hot board (its thermistor above 85 C) holds the fan back: the allowed fan current falls with
  the temperature, and the run is capped at the highest power level whose fan current fits. Still
  too hot at the lowest level, it stops.

Every timing and limit is a named parameter (`burn_params`). The defaults are provisional until
they are tuned on a real heater.

`stm32/core/glow.*` turns the sequencer's glow request into pulses, once per 5 ms PWM period. The
plug is switched straight from the supply, so every pulse width is fixed before it starts, from the
current read in the last pulse scaled by the supply since:

- A 150 us probe first, and again after any gap over 50 ms.
- Per pulse: short pulses at high current (0.3 ms above 100 A, 1.5 ms above 22 A), 15 A rms, at
  most 99 % duty. Duties too low for a readable pulse become 150 us pulses at a lower rate.
- The power asked (90 W, 75 W on a low supply), reached by a gentle start from about 2.5 V.
- A heat budget for the switching transistors (I² x t, drained only while the plug is off; a heat
  start waits until half of it is free). It starts empty after an MCU reset only (a fault reset
  keeps it): the longer fan run before the first glow stands in for it.
- Current between pulses (the low side stuck on) or repeated high-side trips: glow off at once
  and a fault that needs a reset. No airflow or a hot board: no glow.

`stm32/core/fan.*` drives the fan's low-side switch (25 kHz) from the speed the sequencer wants:

- A speed loop on the Hall reading, at most 10 V average at the motor whatever the supply. Its
  integral does not grow while the fan turns more than a quarter below its command (spinning up,
  slowed or blocked).
- The start limit: the duty rises from 0 over at least 1 s, and while the fan turns below half its
  command the motor current (the shunt's average switch current divided by the duty) is held at or
  below 6 A, so a frozen fan never reaches the fan's one-time 8 A fuse. Below 10 % duty the
  estimate is too coarse and the ramp runs on time alone.
- Faults: a stalled motor (current, no Hall edges, 1 s), current at zero duty (the switch stuck
  on), and speed with no command, or 20 % over it for 0.5 s, once the fan has had 30 s to slow
  down. A stall stops everything at once and the sequencer retries it (the stage clears the stall
  itself once the fan is asked for 0); the others stop the fuel and burn off.

`stm32/core/checks.*` tests the interlocks themselves, in 1 ms steps the hardware layer carries
out (heartbeat modes, the glow enable, single short pump pulses, the latch reset line):

- After every reset: with the overheat latch still tripped, a pump pulse draws no current; the
  latch is re-armed (only with a plausible, cool sensor, 500 ms after power-up) and confirmed; a
  static heartbeat, high or low, does not pass for a running one; with everything right, a pump
  pulse does draw current. Any failure refuses starts until the next reset.
- Before every start that may stroke the pump or heat the plug (the pre-check): with the heartbeat
  stopped, neither + side is on, and the glow enable alone does not switch the plug on; with the
  heartbeat running, the pump's high side is up without a fault, and the plug's + side reads the
  supply with no current flowing. A failure ends the start with a pump or glow fault.

The heartbeat, which enables both high sides, otherwise runs from the fan check through burn-off
and while priming.

`stm32/core/pump.*` turns the fuel rate into strokes, one millisecond at a time: 38 ms strokes at a
12 V equivalent duty whatever the supply, at most 6 Hz (10 Hz priming), the first only once the
heartbeat has run 20 ms with the high side reporting no fault. It stops the pump and the heartbeat
for the overcurrent cut, current still flowing 10 ms after a stroke or appearing between strokes,
and a stroke with no current rise (an open coil). A high-side fault gets one heartbeat restart per
run; a second locks out. The overcurrent cut is the ADC's watchdog, armed from a stroke's first
pulse until 10 ms after its last: above 3.3 A on two scans in a row (40 us), its interrupt sets
PUMP_CMD low and stops the heartbeat itself, then the driver latches the fault.

`stm32/core/plug.*` checks the glow plug's type against its setting before a start heats it (a
12 V plug on a 24 V supply would overheat), from the resistance the first glow probe measures. On
a cold plug (no glow for 10 minutes since the last glow or reset, heat exchanger near the board's
temperature or below 40 C) it tells a 12 V plug from a 24 V one and remembers a match, in a small
flash record, with the setting it was checked against. A warm start needs that remembered match,
and a reading that fits it. On a 24 V supply a mismatch or a missing check refuses the start; on a
12 V supply it is only a warning. A plug outside every range never starts.

`stm32/core/params.*` is the heater's parameter table, which the apps show through the link: the
pump and fan ranges, the start-up and shut-down timings, the glow power and the overheat limits,
each with a name, unit, range, step and help text. Changes are staged, then applied together only
with the heater off, after a check of the rules between them (no minimum above its maximum);
"apply and save" has the hardware layer write the table to flash (with a version and a CRC; an
invalid image leaves the defaults). The glow time caps, the glow's minimum fan speed and the
pump's 6 Hz ceiling are not settings.

`stm32/board/` runs the core on the chip through a small hardware interface (`hal.h`): the ADC
counts become volts, amps and temperatures (the thermistor curves are generated tables, since the
chip has no maths library), and a 1 ms tick schedules the self-tests and pump strokes (1 ms), the
glow planner (once per 5 ms glow period) and the safety loop pass (10 ms). Only a completed pass
re-arms the heartbeat's burst and refreshes the watchdog. Saved data goes to flash only with the
heater off.

**The ADC schedule.** The one ADC scans every channel back to back, for ever, into a DMA buffer
(20 us a scan), and nothing else starts a conversion: the main loop copies the buffer each
millisecond; the glow timer picks GLOW_ISENSE 105-125 us into each pulse (settled, inside the
shortest 150 us pulse) and GLOW_ISENSE and VIN in the gap just before the next pulse, and the
planner runs on those once per period; the analog watchdogs watch every PUMP_ISENSE conversion
during strokes. A stopped ADC is restarted from its first channel, so no reading lands in another
channel's place.

`stm32/hal/` is that interface on the STM32G030's registers (no vendor HAL, no C library), with the
start-up code, the linker script and `main`. It is built, but has not run on a chip yet: the clock
tree, the alternate functions, the DMA requests, the ADC order, scan time and pick instants, the
overcurrent cut's latency and the flash programming all need checking on the first board or a
NUCLEO-G031K8.

`stm32/bringup/` is the bring-up image (`build.sh bringup`): the production start-up, board layer
and core, plus a text console on the link UART that sits between the board layer and the chip's
UART read and watchdog refresh (and watches the glow period's timed readings, the pump cut's
arming and the CO heater pin). It shows the raw and converted readings, the reset reason, the
option bytes, flash pages and the CO alarm, and runs bench actions: a fixed fan duty or speed, the
heartbeat alone, a few pump strokes, one stroke with the overcurrent cut's threshold lowered (to see
it act on a dummy load), one glow probe pulse, the CO heater held off, the pre-start self-test, runs
of the sequencer
through the link's POLL handler, and the watchdog tests. Each action is asked for explicitly and
time-limited, and reaches the stages only through the core's bench requests (compiled in only with
`SAFETY_BENCH`), which the core honours only while the burn is off and no self-test runs, through
the same stage drivers and rules as a run; a glow request is one probe pulse, cleared by the core.

`stm32/core/safety.*` joins them all to the link: the start-number rule, the dead-man, the status and
the update handshake.

## Firmware updates and signing

The C6 build verifies every update against the key in its running image's signature block
(ECDSA P-256, Secure Boot V2 format), so the first image, flashed over USB, must be signed too.
`tools/signing_key.sh` signs and verifies images; the project's release key stays off this
repository, and `keys/` holds its public half. `tools/release.sh` builds, signs and writes the
release manifest.

**The safety MCU** is updated by the ESP32 with the image its own build carries (`mcu_update.*` on
`stm32_boot.*`, Home Assistant's "Update heater firmware"). That image is `stm32/build/safety.bin`
from the same checkout (`safety_link`'s `mcu_firmware:`; the release tools build it first), and the
ESPHome build refuses one that is missing, older than its sources, has no vector table, or whose
HELLO text is not the version in `stm32/core/safety.h`. An update:

- `UPDATE_PREPARE` first: only with the heater off and cold, and the MCU then refuses starts until
  it resets. The ESP32 holds STM_BOOT high, pulses STM_NRST, and talks to the STM32's ROM
  bootloader on the same UART at 8E1 (ST AN3155). This needs the option bytes' nBOOT_SEL at 0.
- It checks the chip id (0x466) and reads the option bytes, erases only the image's pages (never
  pages 30 and 31: the saved parameters and the plug record), reads every 256-byte block back, and
  writes page 0 last, so a half-written image has no vector table and never runs. Go starts it; it
  is done when HELLO reports the new version.
- A failure before the erase restarts the old firmware. After it, the MCU waits in its bootloader
  with every output off, and the update can be run again: the ESP32 remembers it over a restart
  and takes the MCU without `UPDATE_PREPARE` once it has been silent for 5 s.
- Writing the option bytes (the watchdog in hardware, boot from the BOOT0 pin) is a separate step,
  not part of an update, and not yet proven on a chip.

## CO alarm

`common/co/` judges the MiCS-5524 metal-oxide sensor on the safety MCU, with integer maths only.
It warms the sensor up, learns its clean-air resistance, and alarms on the resistance ratio. On an
alarm the MCU stops the heater and latches the fault; an implausible reading latches a sensor fault
and refuses starts. It is not a certified CO alarm.

The safety core owns it and the board feeds it a reading a second, with the sensor's heater on from
power-up (off a few seconds once per warm-up, to check it). Whether a sensor is fitted is the
"CO sensor fitted" parameter alone, on by default; a sensor fault ends a run like the alarm, with
its own fault code. Setting it to "not fitted" is refused while the alarm is on (a board without
the sensor, showing a sensor fault, can still be set). Below -30 C the state reads "too cold" and the alarm still acts. The learnt
clean-air resistance is kept in a small log in the plug record's flash page, so the page is rarely
erased. DETAIL carries the state and a rough ppm reading.

## Tests

- `esp32/components/flamekeeper/tests/run.sh`: the core on the host, with sanitizers, against a
  simulated heater and a simulated safety MCU, and the safety-MCU updater against a simulated STM32
  ROM bootloader (NACKs, page erase, option bytes, lost bytes, a power loss mid-write).
- `stm32/tests/run.sh`: the safety core (`stm32/core/`): the burn sequencer against a heater model
  (whole cycles, every way a run ends, the glow caps and the airflow rule), and end to end with the
  ESP32's real link client over a byte wire (dead-man, an MCU reset mid-run).
- Stress, also in `stm32/tests/run.sh` with fixed seeds: `fuzz_link.cpp` fuzzes both ends of the
  link (the parser, the server dispatch and the safety core behind it, the link client and the
  record decoders), and `test_soak.cpp` runs the whole safety MCU for hours of simulated time per
  seed under random user actions, link loss and noise, MCU resets and injected faults, checking the
  safety rules after every call (no glow without airflow, the glow caps, fuel only with the
  heartbeat and air, no start without a new start number, latched faults, the dead-man, ...).
  `stm32/tests/stress.sh [fuzz|soak]` runs the long versions (`SOAK_SEEDS`, `SOAK_HOURS`,
  `FUZZ_ITERS`; libFuzzer when the compiler has it), and `test_soak --shrink SEED` cuts a failing
  seed down to a regression test.
- `common/*/tests/run.sh`: the link codec and the CO alarm.
- `stm32/tools/check_registers.py`: the driver's register map (`stm32/hal/`: bases, offsets, every
  bit it writes, IRQ numbers, the flash layout) against ST's SVD for the STM32G030; usage at its top.
- `esp32/tools/boot_test.sh`: boots a compiled image in Espressif's QEMU and fails on a crash or a
  restart. A C6 build checks its own signature at start-up, so pass the signed app with `--app`.
  The C6's log goes to USB Serial/JTAG, which QEMU does not emulate, so a clean C6 run ends with
  exit code 2 (no crash, setup not confirmed).
