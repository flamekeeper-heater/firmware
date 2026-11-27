# Shared by the firmware tools (sourced, not run): the pinned ESPHome, the key folder and the board
# profiles. Sourcing scripts set HERE (this folder) first.

ESPHOME_VERSION=2026.10.2  # also in README.md
KEYS=${FLAMEKEEPER_KEYS_DIR:-"$HERE/../keys"}
ROOT=$(cd "$HERE/../.." && pwd)  # the repository

# Board profiles kept outside this repository: FLAMEKEEPER_PROFILES names a bash file, sourced
# here, that defines extra_profile BOARD-or-YAML setting the same variables as profile below.
if [[ -n ${FLAMEKEEPER_PROFILES:-} ]]; then
  . "$FLAMEKEEPER_PROFILES"
fi

# profile BOARD-or-YAML: sets board, profile_yaml (the YAML file), name (the ESPHome build name),
# scheme (signing format: v1 Secure Boot V1 for the classic ESP32, v2 for the ESP32-C6), public
# (false for a build that holds secrets: its release is never uploaded) and safety_mcu (true for a
# build that carries the safety MCU's firmware, stm32/build/safety.bin). Returns 1 for an unknown
# one.
profile() {
  public=true
  safety_mcu=false
  case $(basename "$1") in
    rev-a2 | flamekeeper-rev-a2.yaml)
      board=rev-a2; profile_yaml="$ROOT/esp32/flamekeeper-rev-a2.yaml"; name=flamekeeper; scheme=v2
      safety_mcu=true ;;
    *)
      declare -F extra_profile >/dev/null || return 1
      extra_profile "$1" ;;
  esac
}

# The app protocol major the firmware speaks (ble_protocol.h), as the manifest's JSON pair
# [major, 0] (the app reads the major; the minor is gone).
protocol_json() {
  local h="$ROOT/esp32/components/flamekeeper/ble_protocol.h" major
  major=$(sed -n 's/^static const uint8_t PROTOCOL_MAJOR = \([0-9]*\);.*/\1/p' "$h")
  [[ -n $major ]] || { echo "common.sh: protocol version not found in $h" >&2; return 1; }
  echo "[$major, 0]"
}

# Builds the safety MCU's firmware from this checkout (stm32/build.sh) for a profile with
# safety_mcu=true to carry. The old image goes first, so a failed build leaves none; the ESPHome
# build checks the new one again (safety_link).
build_safety_mcu() {
  local bin="$ROOT/stm32/build/safety.bin" v
  rm -f "$bin" "$ROOT/stm32/build/safety.elf"
  "$ROOT/stm32/build.sh" || { echo "the safety MCU firmware did not build" >&2; return 1; }
  v=$(LC_ALL=C grep -ao 'Flamekeeper safety [0-9.]*' "$bin" | head -1)
  echo "   safety MCU firmware ${v#Flamekeeper safety }, $(stat -f %z "$bin") bytes"
}

# fits_app_slot SIGNED BUILD: whether a signed image fits the smallest app partition of an ESPHome
# build folder (.esphome/build/<name>). Signing pads the app to 64 KB and adds a 4 KB block, so an
# app that ESPHome reports as fitting can still be too big once signed; says so on stderr.
fits_app_slot() {
  local size slot
  size=$(stat -f %z "$1")
  slot=$(awk -F, '$2 ~ /^ *app *$/ { gsub(/ /, "", $5); print $5 }' "$2/partitions.csv" |
    while read -r n; do echo $((n)); done | sort -n | head -1)
  [[ -n $slot ]] || { echo "no app partition in $2/partitions.csv" >&2; return 1; }
  ((size <= slot)) && return 0
  echo "the signed image is $size bytes, the app partition $slot: $((size - slot)) bytes too big" >&2
  return 1
}
