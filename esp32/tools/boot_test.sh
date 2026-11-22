#!/usr/bin/env bash
# Boot smoke test: boots an ESPHome build in Espressif's QEMU and fails if it crashes at startup.
#
#   boot_test.sh [--app SIGNED.bin] <ESPHome build dir or firmware.factory.bin> [seconds]
#
# --app puts a signed app image in place of the factory image's app: an ESP32-C6 build refuses to
# run unsigned (it checks its own signature at start-up).
#
# The build dir is .esphome/build/<name> (or its build/ folder). Nothing there is written: the image
# is copied to a temp dir, which also gets the serial logs.
#
# QEMU has no Wi-Fi or Bluetooth radio, and the image crashes as soon as it starts one. So in the
# temp copy esp_wifi_start() and esp_bt_controller_init() return ESP_FAIL at once ("radio shim"),
# found through firmware.elf next to the image. The rest of the firmware runs unchanged: generated
# setup code, every component's setup(), dump_config and the main loop.
#
# Exit codes: 0 boots cleanly, 1 crash or setup never finished, 2 the chip or its log is not
# supported by QEMU, 3 bad input or QEMU missing.

set -uo pipefail

QEMU_VERSION=esp_develop_9.2.2_20261109
QEMU_TAG=esp-develop-9.2.2-20261109
QEMU_HOME=${FLAMEKEEPER_QEMU_HOME:-$HOME/.cache/flamekeeper-qemu}

# Every line must appear for a pass: ESPHome's end of setup, and the hub's dump_config (shown at
# the logger's default level, DEBUG; a build logging at INFO or less cannot pass).
PASS_MARKERS=("setup() finished successfully" "Flamekeeper hub")
BOOT_TIMEOUT=120
LOOP_SECONDS=10
# Any of these fails the run.
CRASH_RE="Guru Meditation|abort\(\) was called|panic'ed|Backtrace:|CRASH DETECTED|stack overflow|Stack overflow|assert failed|Assertion failed|assertion \"|Rebooting\.\.\.|Task watchdog got triggered|Interrupt wdt timeout|Unsuccessful boot attempts: [1-9]|No bootable app"

die() { echo "boot test: $*" >&2; exit 3; }

usage() {
  echo "usage: $0 [--app SIGNED.bin] <ESPHome build dir or firmware.factory.bin> [seconds, default 30]" >&2
  echo "  A setup still running after <seconds> gets up to $BOOT_TIMEOUT s; the loop then runs $LOOP_SECONDS s more." >&2
  exit 3
}

install_hint() {
  local host
  case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) host=aarch64-apple-darwin ;;
    Darwin-x86_64) host=x86_64-apple-darwin ;;
    Linux-aarch64) host=aarch64-linux-gnu ;;
    *) host=x86_64-linux-gnu ;;
  esac
  cat >&2 <<EOF
Espressif QEMU ($QEMU_TAG) is not installed. Get the official release into $QEMU_HOME:

  v=$QEMU_VERSION; url=https://github.com/espressif/qemu/releases/download/$QEMU_TAG
  mkdir -p "$QEMU_HOME/\$v" && cd "$QEMU_HOME/\$v"
  curl -fLO \$url/qemu-\$v-checksum.sha256
  for a in xtensa riscv32; do curl -fLO \$url/qemu-\$a-softmmu-\$v-$host.tar.xz; done
  grep -v '^#' qemu-\$v-checksum.sha256 | shasum -a 256 -c --ignore-missing
  for f in *.tar.xz; do tar -xJf "\$f"; done

On macOS it also needs: brew install libgcrypt glib pixman sdl2-compat
EOF
}

APP_IMAGE=
if [[ ${1:-} == --app ]]; then
  [[ -f ${2:-} ]] || die "--app: no such file: ${2:-}"
  APP_IMAGE=$2
  shift 2
fi
[[ $# -ge 1 && $# -le 2 ]] || usage
INPUT=$1
RUN_SECONDS=${2:-30}
[[ $RUN_SECONDS =~ ^[0-9]+$ && $RUN_SECONDS -ge 5 ]] || die "seconds must be a whole number, 5 or more"

# --- Find the factory image and its ELF ------------------------------------------------------
if [[ -d $INPUT ]]; then
  IMAGE=
  for c in "$INPUT/build/firmware.factory.bin" "$INPUT/firmware.factory.bin"; do
    [[ -f $c ]] && { IMAGE=$c; break; }
  done
  [[ -n $IMAGE ]] || die "no firmware.factory.bin in $INPUT or $INPUT/build"
elif [[ -f $INPUT ]]; then
  IMAGE=$INPUT
else
  die "$INPUT: no such file or folder"
fi
ELF="$(dirname "$IMAGE")/firmware.elf"
[[ -f $ELF ]] || ELF=

TMP=${TMPDIR:-/tmp}
WORK=$(mktemp -d "${TMP%/}/boot_test.XXXXXX") || die "cannot make a temp dir"
trap 'rm -rf "$WORK"' EXIT   # until the boot: the logs stay after it

# --- Prepare the flash and eFuse images (copies only) ----------------------------------------
# Prints shell assignments: CHIP, FLASH, SHIM, PROJECT.
PREP=$(python3 -I - "$IMAGE" "$ELF" "$WORK" "$APP_IMAGE" <<'PY'
import hashlib, struct, sys

image, elf, work, app_image = sys.argv[1:5]
data = bytearray(open(image, 'rb').read())

def q(s):
    return "'" + s.replace("'", "'\\''") + "'"

def fail(msg):
    print('echo %s >&2; exit 3' % q('boot test: ' + msg))
    sys.exit(0)

# The second-stage bootloader: at 0x1000 on the classic ESP32, at 0 on the newer chips.
boot = next((o for o in (0x0, 0x1000) if len(data) > o + 16 and data[o] == 0xE9), None)
if boot is None or data[0x8000:0x8002] != b'\xaa\x50':
    fail('%s is not a full flash image (bootloader and partition table); use firmware.factory.bin' % image)
chips = {0x00: 'esp32', 0x02: 'esp32s2', 0x05: 'esp32c3', 0x09: 'esp32s3', 0x0C: 'esp32c2',
         0x0D: 'esp32c6', 0x10: 'esp32h2', 0x12: 'esp32p4', 0x14: 'esp32c61', 0x17: 'esp32c5'}
chip_id = struct.unpack_from('<H', data, boot + 12)[0]
chip = chips.get(chip_id, 'chip-id-%d' % chip_id)
flash_mb = 1 << (data[boot + 3] >> 4)
if len(data) > flash_mb << 20:
    fail('image is larger than its %d MB flash' % flash_mb)

# The app the bootloader starts: the factory partition, else ota_0 (otadata is blank).
apps = {}
for i in range(0, 0xC00, 32):
    e = data[0x8000 + i:0x8000 + i + 32]
    if e[:2] != b'\xaa\x50':
        break
    if e[2] == 0:
        apps[e[3]] = struct.unpack_from('<I', e, 4)[0]
app = apps.get(0x00, apps.get(0x10))
if app is None or data[app] != 0xE9:
    fail('no app image in the factory or ota_0 partition')
if app_image:
    signed = open(app_image, 'rb').read()
    if signed[:1] != b'\xe9':
        fail('%s is not an app image' % app_image)
    data[app:app + len(signed)] = signed

desc = app + 24 + 8                     # esp_app_desc_t opens the first segment
project = 'unknown'
elf_sha = None
if struct.unpack_from('<I', data, desc)[0] == 0xABCD5432:
    project = bytes(data[desc + 48:desc + 80]).split(b'\0')[0].decode(errors='replace')
    elf_sha = bytes(data[desc + 0x90:desc + 0xB0])

def segments():
    out, p = [], app + 24
    for _ in range(data[app + 1]):
        vaddr, size = struct.unpack_from('<II', data, p)
        out.append((vaddr, size, p + 8))
        p += 8 + size
    return out, p

def symbols(path, wanted):
    e = open(path, 'rb').read()
    if e[:4] != b'\x7fELF' or e[4] != 1:
        return {}
    shoff, = struct.unpack_from('<I', e, 0x20)
    shentsize, shnum = struct.unpack_from('<HH', e, 0x2E)
    sh = [struct.unpack_from('<IIIIIIIIII', e, shoff + i * shentsize) for i in range(shnum)]
    found = {}
    for s in sh:
        if s[1] != 2:                   # SHT_SYMTAB
            continue
        strtab = sh[s[6]]
        for j in range(s[5] // 16):
            name, value, size, info = struct.unpack_from('<IIIB', e, s[4] + j * 16)
            if info & 0xF != 2:         # STT_FUNC
                continue
            start = strtab[4] + name
            n = e[start:e.index(b'\0', start)].decode()
            if n in wanted:
                found[n] = value
    return found

def image_ok():
    # The app's own checksum and SHA-256, as the bootloader checks them. A corrupt image must stay
    # corrupt: the shim below rewrites both.
    segs, end = segments()
    cs = 0xEF
    for sv, size, fp in segs:
        for b in data[fp:fp + size]:
            cs ^= b
    p = end + 15 - ((end - app) % 16)
    if data[p] != cs:
        return False
    return not data[app + 23] or hashlib.sha256(data[app:p + 1]).digest() == data[p + 1:p + 33]

shim = 'off: no firmware.elf next to the image'
if not image_ok():
    shim = 'off: the app image fails its checksum or SHA-256 (corrupt?)'
elif elf:
    if elf_sha is not None and hashlib.sha256(open(elf, 'rb').read()).digest() != elf_sha:
        shim = 'off: firmware.elf does not match the image'
    else:
        wanted = ('esp_wifi_start', 'esp_bt_controller_init')
        syms = symbols(elf, wanted)
        segs, end = segments()
        xtensa = chip in ('esp32', 'esp32s2', 'esp32s3')
        done = []
        for name in wanted:
            va = syms.get(name)
            if va is None:
                continue
            for sv, size, fp in segs:
                if sv <= va < sv + size:
                    p = fp + va - sv
                    if xtensa:
                        if data[p] != 0x36:          # entry a1, N
                            break
                        # after entry: movi.n a2, -1 ; retw.n
                        data[p + 3:p + 7] = bytes.fromhex('7cf21df0')
                    else:
                        # c.li a0, -1 ; c.ret
                        data[p:p + 4] = bytes.fromhex('7d558280')
                    done.append(name)
                    break
        if done:
            # New checksum and SHA-256, so the bootloader still accepts the app.
            cs = 0xEF
            for sv, size, fp in segs:
                for b in data[fp:fp + size]:
                    cs ^= b
            p = end + 15 - ((end - app) % 16)
            data[p] = cs
            if data[app + 23]:
                data[p + 1:p + 33] = hashlib.sha256(data[app:p + 1]).digest()
            shim = 'on: ' + ', '.join(n + '() returns ESP_FAIL' for n in done)
        else:
            shim = 'off: radio start functions not found in firmware.elf'

data += b'\xff' * ((flash_mb << 20) - len(data))
open(work + '/flash.bin', 'wb').write(data)

if chip == 'esp32':
    # ESP-IDF's QEMU eFuse defaults (chip revision 3) plus a MAC address with its CRC, so the
    # firmware gets a valid MAC: 02:46:4B:00:00:01, locally administered.
    efuse = bytearray(124)
    efuse[12:16] = struct.pack('<I', 0x00008000)
    efuse[20:24] = struct.pack('<I', 0x00100000)
    mac = bytes([0x02, 0x46, 0x4B, 0x00, 0x00, 0x01])
    crc = 0
    for b in mac:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8C if crc & 1 else crc >> 1
    efuse[4:10] = mac[::-1]
    efuse[10] = crc
    open(work + '/efuse.bin', 'wb').write(efuse)

print('CHIP=%s FLASH=%d SHIM=%s PROJECT=%s' % (chip, flash_mb, q(shim), q(project)))
PY
) || die "could not prepare the image (python3 failed)"
eval "$PREP"

# --- Find QEMU ---------------------------------------------------------------------------------
case $CHIP in
  esp32|esp32s2|esp32s3) QEMU_BIN=qemu-system-xtensa ;;
  *) QEMU_BIN=qemu-system-riscv32 ;;
esac
QEMU=
for c in "$QEMU_HOME/$QEMU_VERSION/qemu/bin/$QEMU_BIN" $(ls -1d "$QEMU_HOME"/*/qemu/bin/$QEMU_BIN 2>/dev/null | sort -r); do
  [[ -x $c ]] && { QEMU=$c; break; }
done
[[ -n $QEMU ]] || QEMU=$(command -v "$QEMU_BIN" || true)
[[ -n $QEMU ]] || { install_hint; exit 3; }
if ! QEMU_V=$("$QEMU" --version 2>&1 | head -1); then
  echo "$QEMU_V" >&2
  echo "boot test: $QEMU does not run (missing libraries?)" >&2
  install_hint
  exit 3
fi
if ! "$QEMU" -machine help 2>/dev/null | grep -q "^$CHIP "; then
  echo "NOT SUPPORTED: $QEMU_V has no $CHIP machine (ESP32-C6 came in $QEMU_TAG)." >&2
  exit 2
fi

# --- Boot -------------------------------------------------------------------------------------
ARGS=(-M "$CHIP" -nographic -nic none -monitor none
  -drive "file=$WORK/flash.bin,if=mtd,format=raw"
  -global "driver=timer.$CHIP.timg,property=wdt_disable,value=true"
  # UART0, UART1, then the USB-Serial-JTAG console on the newer chips.
  -serial "file:$WORK/serial0.log" -serial "file:$WORK/serial1.log" -serial "file:$WORK/serial2.log")
if [[ $CHIP == esp32 ]]; then
  ARGS+=(-m 4M -drive "file=$WORK/efuse.bin,if=none,format=raw,id=efuse"
    -global "driver=nvram.esp32.efuse,property=drive,value=efuse")
fi

echo "Booting $PROJECT ($CHIP, ${FLASH} MB flash) in QEMU for $RUN_SECONDS s"
echo "  image: $IMAGE"
echo "  radio shim: $SHIM"
echo "  logs: $WORK"

"$QEMU" "${ARGS[@]}" </dev/null >"$WORK/qemu.out" 2>&1 &
QPID=$!
trap 'kill $QPID 2>/dev/null' EXIT   # the logs stay

# The console logs: the ports that got the ROM's boot banner (not UART1, the heater bus).
console_logs() {
  local f
  for f in "$WORK"/serial0.log "$WORK"/serial2.log; do
    grep -aq "rst:" "$f" 2>/dev/null && echo "$f"
  done
}

# Runs for the given seconds. A setup still running then (a busy host) gets up to BOOT_TIMEOUT s
# to finish, and the main loop always gets LOOP_SECONDS after it.
START=$(date +%s)
END=$RUN_SECONDS
CRASH_AT=
SETUP_AT=
while kill -0 $QPID 2>/dev/null; do
  sleep 0.5
  NOW=$(($(date +%s) - START))
  LOGS=$(console_logs)
  if [[ -n $LOGS ]]; then
    # shellcheck disable=SC2086
    if [[ -z $SETUP_AT ]] && grep -aqF "${PASS_MARKERS[0]}" $LOGS; then
      SETUP_AT=$NOW
      END=$((NOW + LOOP_SECONDS > RUN_SECONDS ? NOW + LOOP_SECONDS : RUN_SECONDS))
    elif [[ -z $SETUP_AT && $END -lt $BOOT_TIMEOUT ]] && grep -aqF "Running through setup()" $LOGS; then
      END=$BOOT_TIMEOUT
    fi
    # shellcheck disable=SC2086
    if [[ -z $CRASH_AT ]] && grep -aqE "$CRASH_RE" $LOGS; then CRASH_AT=$NOW; fi
    # shellcheck disable=SC2086
    for f in $LOGS; do [[ $(grep -ac "rst:" "$f") -gt 1 ]] && { CRASH_AT=${CRASH_AT:-$NOW}; }; done
  fi
  # After a crash, a few seconds more for the backtrace and the next boot's crash report.
  [[ -n $CRASH_AT && $NOW -ge $((CRASH_AT + 4)) ]] && break
  [[ $NOW -ge $END ]] && break
done
QEMU_DIED=0
kill -0 $QPID 2>/dev/null || QEMU_DIED=1
kill $QPID 2>/dev/null
wait $QPID 2>/dev/null
ELAPSED=$(($(date +%s) - START))

# --- Verdict ----------------------------------------------------------------------------------
LOGS=$(console_logs)
# The main log: the one the bootloader wrote to.
MAIN=
for f in $LOGS; do grep -aq "2nd stage bootloader" "$f" && { MAIN=$f; break; }; done
MAIN=${MAIN:-$WORK/serial0.log}
# One readable copy: no colour codes, no carriage returns.
sed -e $'s/\x1b\\[[0-9;]*m//g' -e $'s/\r$//' "$MAIN" >"$WORK/serial.log" 2>/dev/null

PROBLEMS=()
if [[ $QEMU_DIED -eq 1 && $ELAPSED -lt $END && -z $CRASH_AT ]]; then
  PROBLEMS+=("QEMU stopped by itself: $(tail -1 "$WORK/qemu.out")")
fi
n=$(grep -ac "rst:" "$MAIN")
[[ $n -gt 1 ]] && PROBLEMS+=("the chip restarted: $n boot banners")
for f in $LOGS; do
  line=$(grep -aE -m1 "$CRASH_RE" "$f" | sed -e $'s/\x1b\\[[0-9;]*m//g' -e $'s/\r$//')
  [[ -n $line ]] && PROBLEMS+=("crash: $line")
done
APP_LINES=0
# shellcheck disable=SC2086
[[ -n $LOGS ]] && APP_LINES=$(cat $LOGS | grep -ac '\[[EWICDV]\]\[[A-Za-z0-9_.]*:[0-9]*\]')
MISSING=()
for m in "${PASS_MARKERS[@]}"; do
  # shellcheck disable=SC2086
  [[ -n $LOGS ]] && grep -aqF "$m" $LOGS || MISSING+=("$m")
done

if [[ ${#PROBLEMS[@]} -eq 0 && ${#MISSING[@]} -eq 0 ]]; then
  # The flash copy holds the build's secrets (Wi-Fi password, API key): kept only on failure.
  rm -f "$WORK/flash.bin" "$WORK/efuse.bin"
  echo "PASS: $PROJECT booted, setup finished after ${SETUP_AT:-?} s, no crash or restart in $ELAPSED s."
  echo "  log: $WORK/serial.log"
  exit 0
fi

if [[ ${#PROBLEMS[@]} -eq 0 && $APP_LINES -eq 0 && $CHIP != esp32 ]]; then
  rm -f "$WORK/flash.bin"
  echo "NOT SUPPORTED: no crash or restart in $ELAPSED s, but none of the app's log reached QEMU,"
  echo "  so setup could not be confirmed. On $CHIP the ESPHome logger writes to USB-Serial-JTAG"
  echo "  through its interrupt-driven driver, which this QEMU does not emulate (only panics show)."
  echo "  log: $WORK/serial.log"
  exit 2
fi

echo "FAIL: $PROJECT ($CHIP) after $ELAPSED s"
for p in ${PROBLEMS[@]+"${PROBLEMS[@]}"}; do echo "  $p"; done
for m in ${MISSING[@]+"${MISSING[@]}"}; do echo "  never printed: $m"; done
if grep -aq "assert failed: esp_phy_enable" "$WORK/serial.log" && [[ $SHIM == off* ]]; then
  echo "  (that assert is QEMU's missing radio: pass the build dir, with firmware.elf, for the shim)"
fi
echo "  log: $WORK/serial.log"
echo "--- last 40 lines ---"
tail -40 "$WORK/serial.log"
exit 1
