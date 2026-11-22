#!/usr/bin/env bash
# Builds a board profile, signs it with the release key and installs it over the network with
# ESPHome's own OTA: the everyday way to flash a board once it runs a build with signed updates
# (every unsigned image is refused from then on).
#
#   flash_signed.sh PROFILE.yaml [DEVICE]
#
# PROFILE.yaml is esp32/flamekeeper-rev-a2.yaml; DEVICE an address or name (default: what ESPHome
# finds). The board must be running a build signed with the same key. Flash only with the heater off.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/common.sh"

die() { echo "flash_signed: $*" >&2; exit 1; }
[[ $# -ge 1 && -f $1 ]] || die "usage: $0 PROFILE.yaml [DEVICE]"
yaml=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
dir=$(dirname "$yaml")
profile "$yaml" || die "unknown profile $(basename "$yaml")"
[[ -f $KEYS/release_public.bin ]] || die "no public key in $KEYS (run tools/signing_key.sh create first)"

esphome() { (cd "$dir" && uv run --quiet --with "esphome==$ESPHOME_VERSION" esphome "$@"); }
esphome -s update_public_key "$KEYS/release_public.bin" compile "$(basename "$yaml")"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
signed="$tmp/signed.bin"
"$HERE/signing_key.sh" sign "$scheme" "$dir/.esphome/build/$name/build/firmware.ota.bin" "$signed"
args=(upload "$(basename "$yaml")" --file "$signed")
[[ -n ${2:-} ]] && args+=(--device "$2")
esphome -s update_public_key "$KEYS/release_public.bin" "${args[@]}"
