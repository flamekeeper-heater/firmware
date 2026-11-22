#!/bin/sh
# Host tests of the safety MCU's core, built as strict C99 (as the STM32 will build it) with the
# sanitizers on. The heater model is test-only.
set -e
here=$(cd "$(dirname "$0")" && pwd)
core="$here/../core"
out=${TMPDIR:-/tmp}/flamekeeper_stm32_test.$$
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT
strict="-std=c99 -pedantic -Wall -Wextra -Werror -Wconversion -Wno-sign-conversion"
san="-fsanitize=address,undefined -fno-sanitize-recover=undefined -g"
cc $strict $san -c "$core/burn.c" -o "$out/burn.o"
cc -std=c99 -Wall -Wextra -Werror $san "$here/test_burn.c" "$here/heater_model.c" "$out/burn.o" -lm -o "$out/test_burn"
cc $strict $san -c "$core/glow.c" -o "$out/glow.o"
cc -std=c99 -Wall -Wextra -Werror $san "$here/test_glow.c" "$out/glow.o" -lm -o "$out/test_glow"
cc $strict $san -c "$core/fan.c" -o "$out/fan.o"
cc -std=c99 -Wall -Wextra -Werror $san "$here/test_fan.c" "$out/fan.o" -lm -o "$out/test_fan"
cc $strict $san -c "$core/checks.c" -o "$out/checks.o"
cc -std=c99 -Wall -Wextra -Werror $san "$here/test_checks.c" "$out/checks.o" -o "$out/test_checks"
cc $strict $san -c "$core/pump.c" -o "$out/pump.o"
cc -std=c99 -Wall -Wextra -Werror $san "$here/test_pump.c" "$out/pump.o" -lm -o "$out/test_pump"
"$out/test_pump"
"$out/test_checks"
"$out/test_fan"
"$out/test_glow"
"$out/test_burn"

# Safety core against the burn tests' model, and end to end against the ESP32's real link client.
fk="$here/../../esp32/components/flamekeeper"
link="$here/../../common/link"
cc $strict $san -c "$core/safety.c" -o "$out/safety.o"
cc $strict $san -c "$link/link.c" -o "$out/link.o"
cc -std=c99 -Wall -Wextra -Werror $san -c "$here/heater_model.c" -o "$out/heater_model.o"
c++ -std=c++17 -Wall -Wextra -Werror $san "$here/test_e2e.cpp" "$fk/link_client.cpp" "$fk/controller.cpp" \
  "$fk/tuning.cpp" "$fk/thermostat.cpp" "$out/safety.o" "$out/burn.o" "$out/glow.o" "$out/fan.o" "$out/checks.o" "$out/pump.o" "$out/link.o" "$out/heater_model.o" \
  -o "$out/test_e2e"
"$out/test_e2e"
