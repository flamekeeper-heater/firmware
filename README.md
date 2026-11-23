# Flamekeeper firmware

Open-source firmware for the **Flamekeeper ECU**, a replacement controller board for diesel
air heaters. The board has two processors:

- an **ESP32-C6** that runs everything the user sees: the phone app over Bluetooth LE and Wi-Fi,
  a web page, Home Assistant, the thermostat, timers and frost protection;
- an **STM32G030 safety MCU** that owns the burn: the start and stop sequences, the glow plug,
  fuel pump and fan, and every fault check. The ESP32 can only ask it for things.

This repository holds the ESP32 firmware (ESPHome on ESP-IDF) and the C code shared with the
safety MCU (the link protocol between the two, and the CO alarm).

> **Status: in development.** The rev A2 board has not been produced yet, the safety MCU firmware
> has not run on a chip yet, and the app protocol may still change. The install steps below
> have not been tried on a real board.

## What it does

- Heat by power level or to a room temperature, fan only, and fuel priming.
- Frost protection, timers, fuel and hour meters, maintenance reminders and an event log.
- Room temperature from the board's probe, Home Assistant, or a Bluetooth thermometer (BTHome,
  Xiaomi, Govee, SwitchBot, RuuviTag).
- The Flamekeeper app over Bluetooth, or over Wi-Fi with every frame encrypted end to end; optional
  access from anywhere through an internet relay that cannot read the traffic.
- Home Assistant through ESPHome's native API, off until switched on from the app.
- Signed firmware updates from the app.

## Layout

| Path | |
|---|---|
| `esp32/flamekeeper-rev-a2.yaml` | The board configuration (rev A2). Build this. |
| `esp32/packages/flamekeeper.yaml` | Everything shared: the hub, Bluetooth, Wi-Fi and every Home Assistant entity. |
| `esp32/components/` | The ESPHome components. `flamekeeper/` is the core: plain C++ with host tests. |
| `esp32/tools/` | Signing, release, network flashing and a QEMU boot test. |
| `esp32/keys/` | The project's public release key. |
| `stm32/` | The safety MCU firmware: its core (`core/`: the burn sequencer, the glow pulse planner, the fan controller, the pump stroke driver, the interlock self-tests, the glow plug type check, the parameter table and the link glue), host-tested against a heater model; the board layer (`board/`) and the STM32G030 driver (`hal/`), not yet run on a chip. |
| `common/` | C99 shared with the safety MCU: the link codec (`link/`), the CO alarm (`co/`), the STM32G030's flash map and option bits (`g030/`), and the host tests' checks (`test/`). |
| [PROTOCOL.md](PROTOCOL.md) | The app protocol, over Bluetooth LE and Wi-Fi. |
| [DESIGN.md](DESIGN.md) | How the firmware is put together, and the link to the safety MCU. |

## Build

You need [uv](https://docs.astral.sh/uv/) (or Python with `pip install esphome==2026.10.2`). ESPHome
is pinned to 2026.10.2.

```bash
cd esp32
uv run --with esphome==2026.10.2 esphome compile flamekeeper-rev-a2.yaml
```

The image ends up in `esp32/.esphome/build/flamekeeper/build/`. To serve the app's web page from the
board, pass its build folder at compile time with `-s web_app <folder>`. Without it the board serves
a placeholder page.

## Install on your board

A board takes only firmware signed with the key that signed the firmware it is running. Official
releases are signed with the project's key, and the app installs them. If you install your own
build, you sign it with your own key, and from then on your board accepts only builds signed with
that key. To go back to official releases, flash an official image over USB.

1. **Make a signing key** (once) and keep it safe:

   ```bash
   openssl ecparam -name prime256v1 -genkey -noout -out ~/flamekeeper-signing-key.pem
   export FLAMEKEEPER_SIGNING_KEY=~/flamekeeper-signing-key.pem
   ```

2. **Build and sign:**

   ```bash
   cd esp32
   uv run --with esphome==2026.10.2 esphome compile flamekeeper-rev-a2.yaml
   tools/signing_key.sh sign v2 .esphome/build/flamekeeper/build/firmware.ota.bin signed.bin
   ```

3. **First install, over USB.** Switch the heater off. Hold the board's BOOT button while you plug
   in the USB-C cable, then write the factory image and your signed app over it:

   ```bash
   uv run --with esphome==2026.10.2 esptool --chip esp32c6 write-flash 0x0 .esphome/build/flamekeeper/build/firmware.factory.bin
   uv run --with esphome==2026.10.2 esptool --chip esp32c6 write-flash 0x10000 signed.bin
   ```

4. **Set it up from the app.** Open the Flamekeeper app near the heater and pair with the PIN on the
   board's label. Wi-Fi and Home Assistant stay off until you switch them on there.

5. **Later updates, over Wi-Fi.** Once the board is on your network, with the heater off:

   ```bash
   tools/flash_signed.sh flamekeeper-rev-a2.yaml <board address>
   ```

Holding BOOT for 10 seconds while the board runs is a factory reset: every paired phone, setting
and network is forgotten. The firmware and its signing key stay.

## Safety MCU firmware

Needs `arm-none-eabi-gcc` (no vendor HAL or C library). It has not run on a chip yet.

```bash
stm32/build.sh           # stm32/build/safety.elf and .bin, and the flash and RAM use
stm32/build.sh bringup   # the bring-up image: stm32/build/bringup.elf and .bin
python3 -I stm32/tools/check_registers.py --svd STM32G030.svd   # the register map against ST's SVD
```

The bring-up image is for a new board's first power-up, not for a heater in use: the same firmware
with a text console on the link UART (J_DEBUG pins 6 and 7, 115200 8N1) that shows every reading
and runs single, time-limited uses of each stage through the same drivers and interlocks.
`stm32/bringup/console.h` lists its commands; `stm32/bringup/bringup.py` drives it from a USB-UART.

## Tests

```bash
esp32/components/flamekeeper/tests/run.sh   # the core, on your computer
stm32/tests/run.sh                          # the safety core, and end to end with the ESP32 side
common/link/tests/run.sh                    # the link codec
common/co/tests/run.sh                      # the CO alarm
esp32/tools/boot_test.sh --app esp32/signed.bin esp32/.esphome/build/flamekeeper   # boots it in QEMU
```

The core tests build mbedTLS from the copy ESPHome downloads, so compile the board once first.
`dump_vectors.cpp` in the core's tests prints golden vectors (exact bytes for records, sign-in and
encrypted frames) for testing an app against.

## Licence

GPL-3.0. See [LICENSE](LICENSE).
