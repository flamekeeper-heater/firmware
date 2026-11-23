#!/bin/sh
# The long versions of the link fuzzer and the safety MCU soak (run.sh runs their quick, fixed
# seeds). Single process each, with the sanitizers on.
#
#   stress.sh [fuzz|soak|all]       (default all)
#   FUZZ_ITERS=100000 FUZZ_SEED=1   mutated inputs for the standalone fuzzer, and its seed
#   FUZZ_SECONDS=180                run time when a clang with libFuzzer is found (FUZZ_CXX, or clang++)
#   SOAK_SEEDS=1-200 SOAK_HOURS=2   soak seeds, and simulated hours per seed
#
# A failing soak seed: `test_soak --shrink SEED --hours H` prints its schedule cut down to the few
# events that still fail, ready to paste into test_soak.cpp's regressions().
set -e
here=$(cd "$(dirname "$0")" && pwd)
core="$here/../core"
fk="$here/../../esp32/components/flamekeeper"
out=${TMPDIR:-/tmp}/flamekeeper_stress.$$
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT
what=${1:-all}
strict="-std=c99 -pedantic -Wall -Wextra -Werror -Wconversion -Wno-sign-conversion"
san="-fsanitize=address,undefined -fno-sanitize-recover=undefined -g -O1"
objs=""
for f in burn glow fan checks pump params plug faultlog safety; do
  cc $strict $san -c "$core/$f.c" -o "$out/$f.o"
  objs="$objs $out/$f.o"
done
cc $strict $san -c "$here/../../common/link/link.c" -o "$out/link.o"
cc $strict $san -c "$here/../../common/co/co_alarm.c" -o "$out/co_alarm.o"
cc -std=c99 -Wall -Wextra -Werror $san -c "$here/heater_model.c" -o "$out/heater_model.o"
objs="$objs $out/link.o $out/co_alarm.o"

if [ "$what" = fuzz ] || [ "$what" = all ]; then
  fuzz_cxx=${FUZZ_CXX:-clang++}
  echo 'extern "C" int LLVMFuzzerTestOneInput(const unsigned char *, unsigned long) { return 0; }' > "$out/probe.cpp"
  if $fuzz_cxx -fsanitize=fuzzer "$out/probe.cpp" -o "$out/probe" 2>/dev/null; then
    # libFuzzer: the seed corpus from the standalone build, then coverage-guided for a while.
    c++ -std=c++17 -Wall -Wextra -Werror $san "$here/fuzz_link.cpp" "$fk/link_client.cpp" $objs -o "$out/fuzz_seeds"
    mkdir -p "$out/corpus"
    "$out/fuzz_seeds" --write-corpus "$out/corpus"
    lf=""
    for f in burn glow fan checks pump params plug faultlog safety; do
      $fuzz_cxx -x c $strict -fsanitize=fuzzer-no-link,address,undefined -g -O1 -c "$core/$f.c" -o "$out/lf_$f.o"
      lf="$lf $out/lf_$f.o"
    done
    $fuzz_cxx -x c $strict -fsanitize=fuzzer-no-link,address,undefined -g -O1 -c "$here/../../common/link/link.c" -o "$out/lf_link.o"
    $fuzz_cxx -x c $strict -fsanitize=fuzzer-no-link,address,undefined -g -O1 -c "$here/../../common/co/co_alarm.c" -o "$out/lf_co.o"
    $fuzz_cxx -std=c++17 -DUSE_LIBFUZZER -fsanitize=fuzzer,address,undefined -g -O1 "$here/fuzz_link.cpp" \
      "$fk/link_client.cpp" $lf "$out/lf_link.o" "$out/lf_co.o" -o "$out/fuzz_link"
    "$out/fuzz_link" -jobs=1 -workers=1 -max_total_time="${FUZZ_SECONDS:-180}" -seed="${FUZZ_SEED:-1}" "$out/corpus"
  else
    c++ -std=c++17 -Wall -Wextra -Werror $san "$here/fuzz_link.cpp" "$fk/link_client.cpp" $objs -o "$out/fuzz_link"
    "$out/fuzz_link" --iters "${FUZZ_ITERS:-100000}" --seed "${FUZZ_SEED:-1}"
  fi
fi

if [ "$what" = soak ] || [ "$what" = all ]; then
  c++ -std=c++17 -Wall -Wextra -Werror $san "$here/test_soak.cpp" "$fk/link_client.cpp" $objs "$out/heater_model.o" \
    -o "$out/test_soak"
  "$out/test_soak" --seeds "${SOAK_SEEDS:-1-200}" --hours "${SOAK_HOURS:-2}" --quiet
fi
