#!/bin/sh
# Builds and runs the host tests of the shared core: the controller (checks, run source, Home
# Assistant failsafe, room temperature), the protocol codec and command handler, the pairing
# window and label PIN, the Wi-Fi key tables, the Wi-Fi sign-in and frames, and the safety-MCU updater.
#
# The Wi-Fi tests use the mbedTLS sources ESP-IDF ships (the same library the board runs), built
# once into a cache. Set MBEDTLS_DIR to another mbedTLS 3.6 source tree if ESPHome's ESP-IDF is not
# in its usual cache.
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/.."
out=${TMPDIR:-/tmp}/flamekeeper_test.$$
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT
cxx="c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined -g"
$cxx "$here/test_controller.cpp" "$src/controller.cpp" "$src/tuning.cpp" "$src/thermostat.cpp" -o "$out/test_controller"
"$out/test_controller"
$cxx "$here/test_ble_protocol.cpp" "$src/ble_protocol.cpp" "$src/automation.cpp" "$src/ble_thermo.cpp" "$src/controller.cpp" "$src/tuning.cpp" "$src/thermostat.cpp" -o "$out/test_ble_protocol"
"$out/test_ble_protocol"
$cxx "$here/test_pairing.cpp" "$src/pairing.cpp" "$src/controller.cpp" "$src/tuning.cpp" "$src/thermostat.cpp" -o "$out/test_pairing"
"$out/test_pairing"
$cxx "$here/test_access.cpp" "$src/access.cpp" -o "$out/test_access"
"$out/test_access"
$cxx "$here/test_thermostat.cpp" "$src/thermostat.cpp" -o "$out/test_thermostat"
"$out/test_thermostat"
$cxx "$here/test_altitude.cpp" -o "$out/test_altitude"
"$out/test_altitude"
$cxx "$here/test_ble_thermo.cpp" "$src/ble_thermo.cpp" -o "$out/test_ble_thermo"
$cxx "$here/test_automation.cpp" "$src/automation.cpp" "$src/ble_thermo.cpp" "$src/ble_protocol.cpp" "$src/controller.cpp" "$src/tuning.cpp" "$src/thermostat.cpp" -o "$out/test_automation"
"$out/test_automation"
"$out/test_ble_thermo"
$cxx "$here/test_web_files.cpp" -o "$out/test_web_files"
"$out/test_web_files"
# The link client against a simulated safety MCU (the shared C codec in ../../../../common/link/).
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -g -c "$src/link.c" -o "$out/link.o"
$cxx "$here/test_link_client.cpp" "$src/link_client.cpp" "$src/heater_log.cpp" "$src/controller.cpp" "$src/tuning.cpp" "$src/thermostat.cpp" "$out/link.o" -o "$out/test_link_client"
$cxx "$here/test_link_client_replies.cpp" "$src/link_client.cpp" "$out/link.o" -o "$out/test_link_client_replies"
"$out/test_link_client_replies"
# The safety-MCU updater against a simulated STM32 ROM bootloader.
$cxx "$here/test_mcu_update.cpp" "$src/mcu_update.cpp" "$src/stm32_boot.cpp" "$src/link_client.cpp" "$src/controller.cpp" \
  "$src/tuning.cpp" "$src/thermostat.cpp" "$out/link.o" -o "$out/test_mcu_update"
"$out/test_mcu_update"
"$out/test_link_client"

# mbedTLS for the Wi-Fi tests
if [ -z "$MBEDTLS_DIR" ]; then
  for d in "$HOME"/Library/Caches/esphome/idf/frameworks/*/components/mbedtls/mbedtls \
           "$HOME"/.cache/esphome/idf/frameworks/*/components/mbedtls/mbedtls; do
    [ -f "$d/include/mbedtls/ecjpake.h" ] && MBEDTLS_DIR=$d
  done
fi
if [ -z "$MBEDTLS_DIR" ]; then
  echo "wifi_session: SKIPPED (no mbedTLS sources; set MBEDTLS_DIR)"
  exit 1
fi
version=$(sed -n 's/^#define MBEDTLS_VERSION_STRING *"\(.*\)"/\1/p' "$MBEDTLS_DIR/include/mbedtls/build_info.h")
lib=${TMPDIR:-/tmp}/flamekeeper_mbedtls_$version/libmbedcrypto.a
if [ ! -f "$lib" ]; then
  echo "building mbedTLS $version for the host tests (once)"
  objs=$(dirname "$lib")/obj
  mkdir -p "$objs"
  for c in "$MBEDTLS_DIR"/library/*.c; do
    cc -O1 -w -I"$MBEDTLS_DIR/include" -I"$MBEDTLS_DIR/library" -c "$c" -o "$objs/$(basename "$c" .c).o"
  done
  ar rcs "$lib" "$objs"/*.o
fi
$cxx -I"$MBEDTLS_DIR/include" "$here/test_wifi_session.cpp" "$src/wifi_session.cpp" "$src/crypto.cpp" \
  "$src/access.cpp" "$lib" -o "$out/test_wifi_session"
"$out/test_wifi_session"
$cxx -I"$MBEDTLS_DIR/include" "$here/test_board_update.cpp" "$src/board_update.cpp" "$src/crypto.cpp" \
  "$src/controller.cpp" "$src/tuning.cpp" "$src/thermostat.cpp" "$lib" -o "$out/test_board_update"
"$out/test_board_update"
$cxx -I"$MBEDTLS_DIR/include" "$here/test_relay.cpp" "$src/relay.cpp" "$src/relay_client.cpp" "$src/access.cpp" "$src/crypto.cpp" "$lib" -o "$out/test_relay"
"$out/test_relay"
$cxx -I"$MBEDTLS_DIR/include" "$here/test_push.cpp" "$src/push.cpp" "$src/relay.cpp" "$src/relay_client.cpp" \
  "$src/access.cpp" "$src/crypto.cpp" "$src/automation.cpp" "$src/ble_thermo.cpp" "$src/controller.cpp" \
  "$src/tuning.cpp" "$src/thermostat.cpp" "$lib" -o "$out/test_push"
"$out/test_push"
