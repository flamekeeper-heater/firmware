#!/bin/sh
# Host tests of the CO alarm (co_alarm.c), built as strict C99 as the STM32 will build it.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/flamekeeper_co_test.$$
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT
cc -std=c99 -pedantic -Wall -Wextra -Werror -Wconversion -Wno-sign-conversion \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined -g \
  "$here/test_co.c" "$here/../co_alarm.c" -o "$out/test_co"
"$out/test_co"
