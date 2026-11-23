#!/bin/sh
# Host tests of the safety MCU: the core's stages, the core end to end against the ESP32's real link
# client, the link fuzzer's seeds and the soak's regressions and one seed (stress.sh runs their long
# versions), the board layer and the bring-up build over a fake chip, and the driver's option-byte
# check. The firmware's sources build as strict C99, as the STM32 builds them; everything at -O1
# with ASan and UBSan, each source compiled once per flag set, in two lanes. Each test's lines
# print in a fixed order at the end; any failure exits non-zero.
set -e
here=$(cd "$(dirname "$0")" && pwd)
core="$here/../core"
board="$here/../board"
common="$here/../../common"
fk="$here/../../esp32/components/flamekeeper"
out=${TMPDIR:-/tmp}/flamekeeper_stm32_test.$$
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT
san="-fsanitize=address,undefined -fno-sanitize-recover=undefined -g -O1"
strict="-std=c99 -pedantic -Wall -Wextra -Werror -Wconversion -Wno-sign-conversion $san"
ctest="-std=c99 -Wall -Wextra -Werror $san"
cxx="-std=c++17 -Wall -Wextra -Werror $san"
# The bring-up build (build.sh bringup): the core's bench requests, and the driver's five functions
# the console taps renamed to bringup_chip_* (here: the fake chip's).
bench="-DSAFETY_BENCH"
taps="-Dhal_uart_getc=bringup_chip_uart_getc -Dhal_iwdg_refresh=bringup_chip_iwdg_refresh \
  -Dhal_glow_period=bringup_chip_glow_period -Dhal_pump_oc_arm=bringup_chip_pump_oc_arm \
  -Dhal_co_heater=bringup_chip_co_heater"

# Only safety.h and safety.c read SAFETY_BENCH: the bring-up build recompiles safety.c and board.c
# (which includes safety.h) and shares every other object.
if [ "$(grep -rl SAFETY_BENCH "$core" "$board" "$common" | sort)" != "$(printf '%s\n' "$core/safety.c" "$core/safety.h")" ]; then
  echo "run.sh: SAFETY_BENCH is read outside core/safety.{c,h}: build those sources twice" >&2
  exit 1
fi

obj() { # obj NAME SOURCE FLAGS...: $out/NAME.o
  name=$1 src=$2
  shift 2
  case "$src" in
    *.cpp) c++ $cxx "$@" -c "$src" -o "$out/$name.o" ;;
    *) cc "$@" -c "$src" -o "$out/$name.o" ;;
  esac
}
exe() { # exe TEST OBJS...: links $out/TEST from TEST.o and the other objects named
  t=$1
  shift
  objs=""
  for o in "$@"; do objs="$objs $out/$o.o"; done
  c++ $san "$out/$t.o" $objs -lm -o "$out/$t"
}
run() { # run TEST ARGS...: its output kept for the end
  t=$1
  shift
  "$out/$t" "$@" > "$out/$t.log" 2>&1
}
need() { # need WHAT LANE: waits for the other lane's objects; fails if that lane stopped first
  while [ ! -e "$out/$1.ready" ]; do
    if [ -e "$out/$2.done" ]; then return 1; fi
    sleep 0.05
  done
}

core_objs="burn glow fan checks pump params plug faultlog safety link co_alarm"

# Lane 1: the firmware's objects, the stages' tests, the link fuzzer's seeds and 300 mutated
# inputs, the board layer and the bring-up build.
lane1() {
  for f in burn glow fan checks pump params plug faultlog safety; do obj $f "$core/$f.c" $strict; done
  obj link "$common/link/link.c" $strict
  obj co_alarm "$common/co/co_alarm.c" $strict
  obj heater_model "$here/heater_model.c" $ctest
  touch "$out/core.ready"
  for t in plug params pump checks fan glow burn faultlog; do obj test_$t "$here/test_$t.c" $ctest; done
  exe test_plug plug link
  run test_plug
  exe test_params params burn link
  run test_params
  exe test_pump pump
  run test_pump
  exe test_checks checks
  run test_checks
  exe test_fan fan
  run test_fan
  exe test_glow glow
  run test_glow
  exe test_burn burn heater_model
  run test_burn
  exe test_faultlog $core_objs
  run test_faultlog

  obj fuzz_link "$here/fuzz_link.cpp"
  need client lane2
  exe fuzz_link $core_objs link_client
  run fuzz_link --iters 300

  for f in analog board pump_oc; do obj $f "$board/$f.c" $strict; done
  obj fake_chip "$here/fake_chip.c" $strict
  obj test_board "$here/test_board.c" $ctest
  exe test_board fake_chip board analog pump_oc $core_objs
  run test_board

  obj bench_safety "$core/safety.c" $strict $bench
  obj bench_board "$board/board.c" $strict $bench
  obj bench "$here/../bringup/bench.c" $strict $bench
  obj console "$here/../bringup/console.c" $strict $bench
  obj bench_fake_chip "$here/fake_chip.c" $strict $bench $taps
  obj test_bringup "$here/test_bringup.c" $ctest $bench
  exe test_bringup bench_fake_chip bench_board bench_safety bench console analog pump_oc \
    burn glow fan checks pump params plug faultlog link co_alarm
  run test_bringup
}

# Lane 2: the ESP32's link client, end to end, the soak's regressions and seed 48 (fan stalls and
# a lockout, the dead-man, an MCU reset, a CO sensor fault), and the driver's option-byte check.
lane2() {
  obj link_client "$fk/link_client.cpp"
  touch "$out/client.ready"
  obj test_hal "$here/test_hal.cpp"
  exe test_hal
  run test_hal
  obj test_e2e "$here/test_e2e.cpp"
  obj test_soak "$here/test_soak.cpp"
  need core lane1
  exe test_e2e $core_objs heater_model link_client
  run test_e2e
  exe test_soak $core_objs heater_model link_client
  run test_soak --seeds 48
}

# Each lane stops at its first failure (a failed build, or a test's non-zero exit).
(
  trap 'touch "$out/lane1.done"' EXIT
  lane1
  touch "$out/lane1.ok"
) &
(
  trap 'touch "$out/lane2.done"' EXIT
  lane2
  touch "$out/lane2.ok"
) &
wait
for t in test_plug test_params test_pump test_checks test_fan test_glow test_burn test_faultlog test_e2e fuzz_link test_soak \
  test_board test_bringup test_hal; do
  if [ -f "$out/$t.log" ]; then cat "$out/$t.log"; fi
done
if [ ! -e "$out/lane1.ok" ] || [ ! -e "$out/lane2.ok" ]; then
  echo "run.sh: FAILED" >&2
  exit 1
fi
