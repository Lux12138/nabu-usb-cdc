#!/bin/bash
# One unattended round: arm misc -> fastboot(flash+boot) -> wait recovery -> collect.
# Every wait is a 10 s poll with a status line. No silent sleeps.
set -u

ADB="${ADB:-/mnt/c/platform-tools/adb.exe}"
FB="${FB:-/mnt/c/platform-tools/fastboot.exe}"
SERIAL="${SERIAL:-}"
IMAGE=""
FLASH=""
MISC="${MISC:-./misc-bootrecovery.bin}"
BACK_LIMIT=300
START_LIMIT=120
OUT="${OUT:-./logs}"

while [ $# -gt 0 ]; do
  case "$1" in
    --serial) SERIAL="$2"; shift 2 ;;
    --image) IMAGE="$2"; shift 2 ;;
    --flash) FLASH="$2"; shift 2 ;;
    --misc) MISC="$2"; shift 2 ;;
    --back-limit) BACK_LIMIT="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) echo "unknown arg: $1"; exit 2 ;;
  esac
done

mkdir -p "$OUT"
win() { wslpath -w "$1" 2>/dev/null || echo "$1"; }
say() { echo "$(date +%H:%M:%S) $*"; }
stop() { say "STOP: $*"; exit 1; }

[ -n "$IMAGE" ] || stop "--image is required"

if [ -z "$SERIAL" ]; then
  SERIAL="$("$ADB" devices | tr -d '\r' | awk 'NR>1 && $2=="recovery" {print $1; exit}')"
fi
[ -n "$SERIAL" ] || stop "no device in recovery (enter TWRP with volume-up + power)"
say "serial=$SERIAL"

# ---- 0. window baseline -------------------------------------------------
part="$("$ADB" -s "$SERIAL" shell 'readlink -f /dev/block/by-name/userdata' | tr -d '\r')"
name="${part##*/}"
disk="${name:0:3}"
size="$("$ADB" -s "$SERIAL" shell "cat /sys/block/$disk/$name/size" | tr -d '\r')"
case "$size" in ''|*[!0-9]*) stop "cannot size userdata";; esac
SKIP=$(( size / 8 - 8192 ))
[ "$SKIP" -gt 0 ] || stop "window offset not sane: $SKIP"
"$ADB" -s "$SERIAL" exec-out "dd if=/dev/block/by-name/userdata bs=4096 skip=$SKIP count=1 2>/dev/null" > "$OUT/base.bin"
base_hash="$(sha256sum "$OUT/base.bin" | cut -d' ' -f1)"
base_head="$(head -c 16 "$OUT/base.bin" | tr -d '\0' | tr -c '[:print:]' '.')"
say "baseline window=$SKIP head=$base_head sha=${base_hash:0:16}"

# ---- 1. arm misc (hard gate) -------------------------------------------
"$ADB" -s "$SERIAL" push "$(win "$MISC")" /tmp/m0arm.bin >/dev/null || stop "push misc failed"
"$ADB" -s "$SERIAL" shell 'dd if=/tmp/m0arm.bin of=/dev/block/by-name/misc bs=4096 count=1 2>/dev/null'
arm="$("$ADB" -s "$SERIAL" shell 'dd if=/dev/block/by-name/misc bs=16 count=1 2>/dev/null | od -An -tc | head -1' | tr -d '\r')"
case "$(echo "$arm" | tr -s ' ')" in
  *b\ o\ o\ t\ -\ r\ e\ c\ o\ v\ e\ r\ y*) say "misc armed ok";;
  *) stop "misc readback not boot-recovery: $arm";;
esac

# ---- 2. recovery -> fastboot (10 s poll) -------------------------------
"$ADB" -s "$SERIAL" reboot bootloader
deadline=$(( $(date +%s) + START_LIMIT ))
while :; do
  fb="$("$FB" devices | tr -d '\r' | awk 'NF>=2 {print $1; exit}')"
  left=$(( deadline - $(date +%s) ))
  say "wait-fastboot state=${fb:-none} left=${left}s"
  [ -n "$fb" ] && break
  [ "$left" -le 0 ] && stop "fastboot never appeared"
  sleep 10
done

# ---- 3. flash (optional) + boot, same session --------------------------
if [ -n "$FLASH" ]; then
  say "flash cust $(basename "$FLASH")"
  "$FB" -s "$fb" flash cust "$(win "$FLASH")" || stop "flash cust failed"
fi
say "boot $(basename "$IMAGE")"
"$FB" -s "$fb" boot "$(win "$IMAGE")" || stop "boot command failed"
started=$(date +%s)

# ---- 4. wait for recovery (10 s poll) ----------------------------------
deadline=$(( started + BACK_LIMIT ))
while :; do
  st="$("$ADB" devices | tr -d '\r' | awk 'NR>1 && NF>=2 {print $2; exit}')"
  left=$(( deadline - $(date +%s) ))
  say "wait-recovery elapsed=$(( $(date +%s) - started ))s left=${left}s state=${st:-none}"
  [ "$st" = recovery ] && break
  [ "$left" -le 0 ] && stop "did not return to recovery within ${BACK_LIMIT}s"
  sleep 10
done
seconds=$(( $(date +%s) - started ))
serial2="$("$ADB" devices | tr -d '\r' | awk 'NR>1 && $2=="recovery" {print $1; exit}')"
pon="$("$ADB" -s "$serial2" shell 'dmesg 2>/dev/null | grep -i "Powerup reason" | head -1' | tr -d '\r')"

# ---- 5. collect --------------------------------------------------------
"$ADB" -s "$serial2" exec-out "dd if=/dev/block/by-name/userdata bs=4096 skip=$SKIP count=1 2>/dev/null" > "$OUT/after.bin"
after_hash="$(sha256sum "$OUT/after.bin" | cut -d' ' -f1)"
after_head="$(head -c 16 "$OUT/after.bin" | tr -d '\0' | tr -c '[:print:]' '.')"
new=0; [ "$after_hash" != "$base_hash" ] && new=1

say "back in ${seconds}s pon=${pon##*reason=} window_new=$new head=$after_head"
printf '{"t":"%s","seconds":%s,"pon":"%s","window_new":%s,"head":"%s"}\n' \
  "$(date -Iseconds)" "$seconds" "${pon##*reason=}" "$new" "$after_head" >> "$OUT/loop.jsonl"
say "recorded $OUT/loop.jsonl"
