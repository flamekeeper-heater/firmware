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
# scheme (signing format: v1 Secure Boot V1 for the classic ESP32, v2 for the ESP32-C6) and public
# (false for a build that holds secrets: its release is never uploaded). Returns 1 for an unknown one.
profile() {
  public=true
  case $(basename "$1") in
    rev-a2 | flamekeeper-rev-a2.yaml)
      board=rev-a2; profile_yaml="$ROOT/esp32/flamekeeper-rev-a2.yaml"; name=flamekeeper; scheme=v2 ;;
    *)
      declare -F extra_profile >/dev/null || return 1
      extra_profile "$1" ;;
  esac
}

# The app protocol "major.minor" the firmware speaks (ble_protocol.h), as a JSON pair.
protocol_json() {
  local h="$ROOT/esp32/components/flamekeeper/ble_protocol.h" major minor
  major=$(sed -n 's/^static const uint8_t PROTOCOL_MAJOR = \([0-9]*\);.*/\1/p' "$h")
  minor=$(sed -n 's/^static const uint8_t PROTOCOL_MINOR = \([0-9]*\);.*/\1/p' "$h")
  [[ -n $major && -n $minor ]] || { echo "common.sh: protocol version not found in $h" >&2; return 1; }
  echo "[$major, $minor]"
}
