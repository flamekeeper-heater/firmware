# Agent notes

Firmware for the Flamekeeper heater ECU. Read [README.md](README.md) for the layout and build,
[DESIGN.md](DESIGN.md) for the architecture and the safety MCU link, [PROTOCOL.md](PROTOCOL.md) for
the app protocol.

## Commands

```bash
esp32/components/flamekeeper/tests/run.sh       # core host tests (needs one board compile first, for mbedTLS)
stm32/tests/run.sh                              # safety core, and end to end with the ESP32 side
common/link/tests/run.sh && common/co/tests/run.sh
stm32/build.sh && cd esp32 && uv run --with esphome==2026.10.2 esphome compile flamekeeper-rev-a2.yaml
esp32/tools/boot_test.sh --app SIGNED.bin esp32/.esphome/build/flamekeeper   # exit 2 is a clean C6 run
```

Run the host tests after any change under `components/flamekeeper/` or `common/`, and compile the
board after any change to the ESPHome components or YAML.

## Rules

- **Safety first.** The safety MCU owns the burn; the ESP32 only asks. Never add a path that starts
  the heater without an explicit request, resumes heating after a restart, or bypasses the
  controller. Every request goes through `Controller`, whatever its source.
- **Never drive the safety MCU's reset or boot pins** outside the update path, and only after
  `UPDATE_PREPARE` succeeds.
- **Keep the core plain C++** (`components/flamekeeper/`): no ESPHome or ESP-IDF headers in files the
  host tests build. ESPHome glue goes in the hub and the platform files.
- **Protocols change in place** until the app's first release: no compatibility shims, no version
  bumps. After that, follow the versioning rules in PROTOCOL.md and DESIGN.md.
- **Golden vectors.** `tests/dump_vectors.cpp` produces the bytes app developers test against. A
  change to its output is a protocol change: say so in the commit message.
- **ESPHome is pinned** to 2026.10.2 (`esp32/tools/common.sh`, README.md). `board_services.cpp`
  reads one ESPHome preference id directly; check it before moving to a newer ESPHome.
- **No secrets in the repository.** No signing keys, Wi-Fi credentials or API keys; the board gets
  those at setup.
- **Docs stay short.** README.md for people building and installing, DESIGN.md and PROTOCOL.md for
  how it works. The code and its tests carry the details.
