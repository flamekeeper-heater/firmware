#!/usr/bin/env bash
# Builds, signs and describes a firmware release for the app's board updates
# (PROTOCOL.md):
#
#   release.sh VERSION [--notes FILE] [--out DIR] [BOARD...]
#
# VERSION is "major.minor.patch"; BOARD is a board profile (common.sh; default rev-a2). For each
# board it compiles the profile with that version, signs the app image with the release key
# (signing_key.sh), checks the signature, and writes to DIR (default dist/, not in git):
#
#   DIR/<board>/<version>.bin    the signed image, as the app sends it
#   DIR/<board>/manifest.json    version, build time, size, SHA-256, URL, notes, minimum protocol
#
#   release.sh upload VERSION [--out DIR]
#
# puts the image and manifest in the R2 bucket behind firmware.flamekeeper.app (wrangler, signed in
# to the project's Cloudflare account): the image first, then the manifest, so the manifest never
# names an image that is not there. It checks the image against its manifest and its signature
# first. Maintainers only: it needs the project's release key.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/common.sh"
URL_BASE="https://firmware.flamekeeper.app"  # the release host, apart from the store
MIN_PROTOCOL=$(protocol_json)
BUCKET=flamekeeper-firmware  # R2, served at firmware.flamekeeper.app

die() { echo "release: $*" >&2; exit 1; }

# ---------------------------------------------------------------- upload
if [[ ${1:-} == upload ]]; then
  version=${2:-}
  [[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "usage: $0 upload MAJOR.MINOR.PATCH [--out DIR]"
  out="$ROOT/dist"
  [[ ${3:-} == --out ]] && out=$4
  board=rev-a2
  manifest="$out/$board/manifest.json"
  image="$out/$board/$version.bin"
  [[ -f $manifest && -f $image ]] || die "no $board $version release in $out (run: $0 $version $board)"
  python3 - "$manifest" "$image" "$board" "$version" "$URL_BASE" <<'PY' || exit 1
import hashlib, json, os, sys
manifest, image, board, version, base = sys.argv[1:]
m = json.load(open(manifest))
def no(why):
    sys.exit("release: refusing to upload: " + why)
if m.get("private"):
    no("the manifest is marked private (it holds secrets)")
if m.get("board") != board or m.get("version") != version:
    no("the manifest is for %s %s" % (m.get("board"), m.get("version")))
if m.get("url") != "%s/%s/%s.bin" % (base, board, version):
    no("the manifest's url is %s" % m.get("url"))
data = open(image, "rb").read()
if len(data) != m.get("size") or hashlib.sha256(data).hexdigest() != m.get("sha256"):
    no("the image does not match the manifest's size and SHA-256")
PY
  "$HERE/signing_key.sh" verify v2 "$image" || die "refusing to upload: the image's signature does not verify"
  wrangler() { npx --yes wrangler@4 "$@"; }
  echo "== uploading $board $version to $BUCKET"
  wrangler r2 object put "$BUCKET/$board/$version.bin" --file "$image" --remote \
    --content-type application/octet-stream --cache-control "public, max-age=31536000, immutable"
  wrangler r2 object put "$BUCKET/$board/manifest.json" --file "$manifest" --remote \
    --content-type application/json --cache-control "public, max-age=300"
  echo "Published: $URL_BASE/$board/manifest.json -> $version"
  exit 0
fi

# ---------------------------------------------------------------- build and sign
version=${1:-}
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "usage: $0 MAJOR.MINOR.PATCH [--notes FILE] [--out DIR] [BOARD...]"
shift
notes=""
out="$ROOT/dist"
boards=()
while [[ $# -gt 0 ]]; do
  case $1 in
    --notes) notes=$(cat "$2"); shift 2 ;;
    --out) out=$2; shift 2 ;;
    -*) die "unknown argument: $1" ;;
    *) boards+=("$1"); shift ;;
  esac
done
[[ ${#boards[@]} -gt 0 ]] || boards=(rev-a2)
[[ -f $KEYS/release_public.bin && -f $KEYS/release_public.pem ]] ||
  die "no public key in $KEYS (run tools/signing_key.sh create first)"

for board in "${boards[@]}"; do
  profile "$board" || die "unknown board $board"
  yaml=$profile_yaml
  build="$(dirname "$yaml")/.esphome/build/$name"
  echo "== $board $version: compiling $(basename "$yaml")"
  (cd "$(dirname "$yaml")" &&
    uv run --quiet --with "esphome==$ESPHOME_VERSION" esphome -q \
      -s firmware_version "$version" -s update_public_key "$KEYS/release_public.bin" \
      compile "$(basename "$yaml")") || die "$board: the build failed"
  image="$build/build/firmware.ota.bin"
  [[ -f $image ]] || die "$board: no image at $image"
  mkdir -p "$out/$board"
  signed="$out/$board/$version.bin"
  "$HERE/signing_key.sh" sign "$scheme" "$image" "$signed"
  size=$(stat -f %z "$signed")
  sha=$(shasum -a 256 "$signed" | cut -d' ' -f1)
  built=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["build_time"])' "$build/build_info.json")
  url="$URL_BASE/$board/$version.bin"
  $public || url="$version.bin"  # never published: next to its manifest
  python3 - "$out/$board/manifest.json" "$board" "$version" "$built" "$size" "$sha" "$url" "$notes" "$public" \
    "$MIN_PROTOCOL" <<'PY'
import json, sys
path, board, version, built, size, sha, url, notes, public, minp = sys.argv[1:]
m = {"board": board, "version": version, "build": int(built), "size": int(size), "sha256": sha,
     "url": url, "notes": notes, "min_protocol": json.loads(minp)}
if public != "true":
    m["private"] = True  # holds secrets: upload refuses it
json.dump(m, open(path, "w"), indent=2)
open(path, "a").write("\n")
PY
  echo "   $signed: $size bytes, SHA-256 $sha, build $built"
done
echo "Manifests and images in $out"
