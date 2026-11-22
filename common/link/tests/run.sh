#!/bin/sh
# Host tests of the link codec, parser, server dispatch, start-number rule and dead-man.
# Built as strict C99, as the STM32 will build it, with the sanitizers on.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/flamekeeper_link_test.$$
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT
cc -std=c99 -pedantic -Wall -Wextra -Werror -Wconversion -Wno-sign-conversion \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined -g \
  "$here/test_link.c" "$here/../link.c" -o "$out/test_link"
"$out/test_link"
cc -std=c99 -pedantic -Wall -Wextra -Werror -Wconversion -Wno-sign-conversion \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined -g \
  "$here/test_parser_resync.c" "$here/../link.c" -o "$out/test_parser_resync"
"$out/test_parser_resync"
