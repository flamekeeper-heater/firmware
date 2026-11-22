#!/usr/bin/env bash
# The firmware signing key: an ECDSA P-256 private key. The project's release key lives in the
# maintainer's macOS login keychain, never in a file; FLAMEKEEPER_SIGNING_KEY names a PEM file to use
# instead (your own key, on any OS). A board accepts only images signed with the key that signed
# its running image (PROTOCOL.md); keys/ holds the project's public key.
#
#   signing_key.sh create         make the key (once), store it in the keychain, write the public key
#   signing_key.sh public         write the public key files again from the keychain
#   signing_key.sh backup FILE    write the private key (PEM) to FILE, for an offline backup
#   signing_key.sh sign v1|v2 IN OUT
#                                 sign an app image: v1 = Secure Boot V1 format (classic ESP32),
#                                 v2 = V2 format (ESP32-C6, rev A2)
#   signing_key.sh verify v1|v2 IMAGE
#                                 check a signed image against keys/release_public.pem (no keychain)
#
# The key reaches openssl and espsecure only through pipes. Losing it means no board accepts a
# new build over the air any more (each would need a USB flash), so keep the backup somewhere safe.

set -euo pipefail

SERVICE="Flamekeeper firmware signing key"
ACCOUNT="release"
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/common.sh"

die() { echo "signing_key: $*" >&2; exit 1; }
espsecure() { uv run --quiet --with "esphome==$ESPHOME_VERSION" espsecure "$@"; }

have_key() { security find-generic-password -s "$SERVICE" -a "$ACCOUNT" >/dev/null 2>&1; }
# The private key PEM on stdout (macOS may ask to allow the keychain access). FLAMEKEEPER_SIGNING_KEY
# names a PEM file to use instead.
private_pem() {
  if [[ -n ${FLAMEKEEPER_SIGNING_KEY:-} ]]; then
    cat "$FLAMEKEEPER_SIGNING_KEY"
    return
  fi
  have_key || die "no key in the keychain (run: $0 create)"
  security find-generic-password -s "$SERVICE" -a "$ACCOUNT" -w | base64 -D
}

# The public key images are checked against: keys/release_public.pem, or with
# FLAMEKEEPER_SIGNING_KEY set, that key's own public half (written next to it once).
public_pem_path() {
  if [[ -n ${FLAMEKEEPER_SIGNING_KEY:-} ]]; then
    local pub="${FLAMEKEEPER_SIGNING_KEY%.pem}.pub.pem"
    [[ -f $pub ]] || openssl ec -in "$FLAMEKEEPER_SIGNING_KEY" -pubout -out "$pub" 2>/dev/null
    echo "$pub"
  else
    echo "$KEYS/release_public.pem"
  fi
}

write_public() {
  mkdir -p "$KEYS"
  private_pem | openssl ec -pubout -out "$KEYS/release_public.pem" 2>/dev/null
  private_pem | espsecure extract-public-key --version 1 --keyfile /dev/stdin "$KEYS/release_public.bin" >/dev/null
  echo "Public key: keys/release_public.pem and keys/release_public.bin (64 raw bytes, for classic ESP32 builds)"
  echo "SHA-256 of the raw public key: $(shasum -a 256 "$KEYS/release_public.bin" | cut -d' ' -f1)"
}

case "${1:-}" in
  create)
    have_key && die "a key is already in the keychain; refusing to replace it"
    pem=$(openssl ecparam -name prime256v1 -genkey -noout 2>/dev/null)
    [[ $pem == *"BEGIN EC PRIVATE KEY"* ]] || die "openssl did not make a key"
    # Through security's own command line on stdin, so the key is never in a process's arguments.
    printf 'add-generic-password -s "%s" -a "%s" -T /usr/bin/security -w %s\n' \
      "$SERVICE" "$ACCOUNT" "$(printf '%s\n' "$pem" | base64)" | security -i >/dev/null
    pem=
    have_key || die "the key did not reach the keychain"
    echo "Key stored in the login keychain as \"$SERVICE\"."
    write_public
    echo "Now make an offline backup: $0 backup /Volumes/<usb stick>/flamekeeper-release-key.pem"
    ;;
  public)
    write_public
    ;;
  backup)
    [[ -n ${2:-} ]] || die "usage: $0 backup FILE"
    [[ -e $2 ]] && die "$2 exists; refusing to overwrite it"
    (umask 077 && private_pem > "$2")
    echo "Private key written to $2. Keep it offline; anyone with it can sign firmware for every board."
    ;;
  sign)
    [[ $# -eq 4 ]] || die "usage: $0 sign v1|v2 IN OUT"
    case $2 in v1) v=1 ;; v2) v=2 ;; *) die "scheme must be v1 or v2" ;; esac
    [[ -f $3 ]] || die "no such image: $3"
    private_pem | espsecure sign-data --version "$v" --keyfile /dev/stdin -o "$4" "$3" >/dev/null
    # Check it with the public key alone before anyone ships it: the project's, or your own key's.
    "$0" verify "$2" "$4" || die "the signed image does not verify with $(public_pem_path)"
    echo "Signed ($2): $4"
    ;;
  verify)
    [[ $# -eq 3 ]] || die "usage: $0 verify v1|v2 IMAGE"
    case $2 in v1) v=1 ;; v2) v=2 ;; *) die "scheme must be v1 or v2" ;; esac
    [[ -f $3 ]] || die "no such image: $3"
    pub=$(public_pem_path)
    espsecure verify-signature --version "$v" --keyfile "$pub" "$3" >/dev/null 2>&1
    ;;
  *)
    sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
