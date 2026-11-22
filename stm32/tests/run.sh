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
cc -std=c99 -Wall -Wextra -Werror $san -c "$here/heater_model.c" -o "$out/heater_model.o"
c++ -std=c++17 -Wall -Wextra -Werror $san "$here/test_e2e.cpp" "$fk/link_client.cpp" "$fk/controller.cpp" \
  "$fk/tuning.cpp" "$fk/thermostat.cpp" "$out/safety.o" "$out/burn.o" "$out/glow.o" "$out/fan.o" "$out/checks.o" "$out/pump.o" "$out/params.o" "$out/plug.o" "$out/link.o" "$out/heater_model.o" \
  -o "$out/test_e2e"
"$out/test_e2e"

# The link fuzzer's seeds and a few hundred mutations, and the soak's regressions and one seed (33:
# fan stalls and a lockout, the dead-man, MCU resets). stress.sh runs the long versions. Built with
# -O1, since they run hours of simulated time, in two lanes at once.
o1="$san -O1"
mkdir -p "$out/o1"
(
  for f in burn glow fan checks pump params plug safety; do
    cc $strict $o1 -c "$core/$f.c" -o "$out/o1/$f.o"
  done
  cc $strict $o1 -c "$link/link.c" -o "$out/o1/link.o"
  c++ -std=c++17 -Wall -Wextra -Werror $o1 -c "$here/fuzz_link.cpp" -o "$out/fuzz_link.o"
) &
lane=$!
c++ -std=c++17 -Wall -Wextra -Werror $o1 -c "$fk/link_client.cpp" -o "$out/o1/link_client.o"
c++ -std=c++17 -Wall -Wextra -Werror $o1 -c "$here/test_soak.cpp" -o "$out/test_soak.o"
wait $lane
c++ $o1 "$out/fuzz_link.o" "$out"/o1/*.o -o "$out/fuzz_link"
c++ $o1 "$out/test_soak.o" "$out"/o1/*.o "$out/heater_model.o" -o "$out/test_soak"
"$out/fuzz_link" --iters 300
"$out/test_soak" --seeds 33

# The board layer against a fake chip.
cc $strict $san -c "$here/../board/analog.c" -o "$out/analog.o"
cc $strict $san -c "$here/../board/board.c" -o "$out/board.o"
cc -std=c99 -Wall -Wextra -Werror $san "$here/test_board.c" "$out/board.o" "$out/analog.o" "$out/safety.o" "$out/burn.o" \
  "$out/glow.o" "$out/fan.o" "$out/checks.o" "$out/pump.o" "$out/params.o" "$out/plug.o" "$out/link.o" -o "$out/test_board"
"$out/test_board"
