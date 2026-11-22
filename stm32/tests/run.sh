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
cc $strict $san -c "$core/params.c" -o "$out/params.o"
cc $strict $san -c "$here/../../common/link/link.c" -o "$out/link_p.o"
cc -std=c99 -Wall -Wextra -Werror $san "$here/test_params.c" "$out/params.o" "$out/burn.o" "$out/link_p.o" -o "$out/test_params"
cc $strict $san -c "$core/plug.c" -o "$out/plug.o"
cc -std=c99 -Wall -Wextra -Werror $san "$here/test_plug.c" "$out/plug.o" "$out/link_p.o" -o "$out/test_plug"
"$out/test_plug"
"$out/test_params"
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
cc $strict $san -c "$here/../../common/co/co_alarm.c" -o "$out/co_alarm.o"
cc -std=c99 -Wall -Wextra -Werror $san -c "$here/heater_model.c" -o "$out/heater_model.o"
c++ -std=c++17 -Wall -Wextra -Werror $san "$here/test_e2e.cpp" "$fk/link_client.cpp" "$fk/controller.cpp" \
  "$fk/tuning.cpp" "$fk/thermostat.cpp" "$out/safety.o" "$out/burn.o" "$out/glow.o" "$out/fan.o" "$out/checks.o" "$out/pump.o" "$out/params.o" "$out/plug.o" "$out/link.o" "$out/co_alarm.o" "$out/heater_model.o" \
  -o "$out/test_e2e"
"$out/test_e2e"

# The board layer against a fake chip.
cc $strict $san -c "$here/../board/analog.c" -o "$out/analog.o"
cc $strict $san -c "$here/../board/board.c" -o "$out/board.o"
cc $strict $san -c "$here/../board/pump_oc.c" -o "$out/pump_oc.o"
cc -std=c99 -Wall -Wextra -Werror $san "$here/test_board.c" "$out/board.o" "$out/analog.o" "$out/pump_oc.o" "$out/safety.o" "$out/burn.o" \
  "$out/glow.o" "$out/fan.o" "$out/checks.o" "$out/pump.o" "$out/params.o" "$out/plug.o" "$out/link.o" "$out/co_alarm.o" -o "$out/test_board"
"$out/test_board"

# The bring-up build (bringup/): the console and the bench actions, over the core built with its
# bench requests (SAFETY_BENCH), against a fake chip with a fan, a pump and a glow plug.
for f in core/burn core/glow core/fan core/checks core/pump core/params core/plug core/safety board/analog \
  board/board board/pump_oc bringup/bench bringup/console; do
  cc $strict $san -DSAFETY_BENCH -c "$here/../$f.c" -o "$out/bench_$(basename $f).o"
done
cc $strict $san -DSAFETY_BENCH -c "$link/link.c" -o "$out/bench_link.o"
cc $strict $san -DSAFETY_BENCH -c "$here/../../common/co/co_alarm.c" -o "$out/bench_co_alarm.o"
cc -std=c99 -Wall -Wextra -Werror $san -DSAFETY_BENCH "$here/test_bringup.c" "$out"/bench_*.o -o "$out/test_bringup"
"$out/test_bringup"
