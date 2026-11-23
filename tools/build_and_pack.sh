#!/bin/bash
# ONE COMMAND: build the kernel and produce a flashable round.
#
#   bash /e/nabu/tmp/build_and_pack.sh <tag> [<base-tag>]
#
# It does the whole chain and refuses to hand back a package if any step looks
# wrong. The reason every step is in here, and not in a document, is 2026-11-23:
# four rounds came up hanging in disk1s1's Data-volume mount, and the cause was
# never in the kernel source -- it was the build/pack pipeline. Notes did not
# prevent it. A gate does.
#
# Steps, each of which must pass:
#   1. unmount every leftover out-v6 bind layer (they had accumulated to 91)
#   2. mount exactly three layers, verify the count is 3
#   3. build the kernel; require rc=0 and zero ` error:` lines
#   4. unmount in a trap, even on failure
#   5. pack the payload by swapping the kernel in place (never mcopy the kernel)
#   6. assert the kernel extracted from the payload matches the built one
#   7. assert the payload's kernel cluster chain is the known-booting one
#   8. assert boot-merge-r3.img is the untouched original
#   9. print the args count so a silent boot-arg change is visible
#
# Anything unexplained: stop. A package that was not produced cannot brick a round.
set -uo pipefail

TAG=${1:?usage: build_and_pack.sh <tag> [<base-tag>]}
BASE_TAG=${2:-kb-70-kdbgfix}

OUT=/root/merge/out-v6
WORK=/root/merge/l738-full/out
BOARD=/root/merge/l738-device
NABU=nabu/ipados-17.7.11-21H461/nabu
M1="$OUT/$NABU"
M2="$OUT/feeds"
M3="$OUT/state.json"

H=/mnt/e/nabu/hold/$TAG
B=/mnt/e/nabu/hold/$BASE_TAG
K="$WORK/$NABU/kernel/kernel.macho"
LOG=/tmp/build_$TAG.log

say() { echo "[$(date +%H:%M:%S)] $*"; }
die() { echo "STOP: $*"; exit 1; }

# ---------------------------------------------------------------- 1. mounts
unmount_all() {
    local n
    for mp in "$M3" "$M2" "$M1"; do
        n=0
        while mountpoint -q "$mp" 2>/dev/null && [ $n -lt 300 ]; do
            umount "$mp" 2>/dev/null || break
            n=$((n+1))
        done
    done
}

trap 'say "unmounting"; unmount_all' EXIT

say "clearing leftover out-v6 mounts"
unmount_all
left=$(mount 2>/dev/null | grep -c "out-v6" || true)
[ "${left:-0}" = "0" ] || die "$left out-v6 mounts survived cleanup; not building on an unpredictable stack"

# ---------------------------------------------------------------- 2. mount
say "mounting 3 layers"
mount --bind "$WORK/$NABU" "$M1" || die "bind $M1 failed"
mount --bind "$WORK/feeds" "$M2" || die "bind $M2 failed"
touch "$WORK/state.json"
mount --bind "$WORK/state.json" "$M3" || die "bind $M3 failed"

active=$(mount | grep -c "out-v6")
[ "$active" = "3" ] || die "expected 3 active layers, found $active"

[ -f "$OUT/ipsw/21H461/kernelcache.decompressed" ] || die "missing kernelcache"

# ---------------------------------------------------------------- 3. build
say "building (log: $LOG)"
find "$OUT/$NABU/tree/build" -name compile-command.json -delete 2>/dev/null
( cd /root/fresh/UniXNU-latest && \
  python3 -I unixnu build kernel --out "$OUT" -D "$BOARD" \
      --kernelcache "$OUT/ipsw/21H461/kernelcache.decompressed" -j 4 ) >"$LOG" 2>&1
rc=$?
[ $rc -eq 0 ] || die "build returned $rc (see $LOG)"
errs=$(grep -cE " error:" "$LOG" || true)
[ "$errs" = "0" ] || die "build logged $errs errors (see $LOG)"

sha_built=$(sha256sum "$K" | cut -d' ' -f1)
say "built kernel $sha_built"
[ -f "$K" ] || die "kernel not produced"

# ---------------------------------------------------------------- 3.5 NOP18
# This is not optional and it is not a note. Every round that skipped it came up
# hanging in disk1s1's Data-volume mount: kb-73, kb-75, kb-82, kb-83, kb-89,
# kb-90, kb-91. Every round that had it booted: kb-56 through kb-70. The patch
# turns one `cbnz` (a8000037) into a `nop` (1f2003d5) at the site identified by
# 6100005408214439; without it the kernel does not survive early init.
say "applying NOP18 (mandatory)"
python3 /mnt/e/nabu/tools/_nop18_l800.py > /tmp/nop18_$TAG.txt 2>&1
nop_rc=$?
cat /tmp/nop18_$TAG.txt
[ $nop_rc -eq 0 ] || die "NOP18 failed (rc=$nop_rc)"
grep -qE "^(PATCHED|already)$" /tmp/nop18_$TAG.txt || die "NOP18 did not report PATCHED/already"
nop_count=$(grep -oE "nop18 [0-9]+" /tmp/nop18_$TAG.txt | awk '{print $2}')
[ "${nop_count:-0}" = "1" ] || die "expected nop18 count 1, got ${nop_count:-0}"

# The patch rewrites the kernel in place, so the hash changes after it runs.
sha_built=$(sha256sum "$K" | cut -d' ' -f1)
say "post-NOP18 kernel $sha_built"

# ---------------------------------------------------------------- 4/5. pack
say "packing $TAG from base $BASE_TAG"
[ -f "$B/payload.img" ] || die "missing $B/payload.img"
rm -rf "$H"; mkdir -p "$H"

python3 -I /mnt/d/_nabu-usb-cdc/tools/payload_swap_kernel.py \
    "$B/payload.img" "$K" "$H/payload.img" || die "in-place kernel swap failed"

cp -f "$B"/kernel.macho.adt "$B"/boot-merge-r3.img \
      "$B"/gpu-addon.bin "$B"/owner-trust.bundle "$H"/ || die "sidecar copy failed"
cp -f "$K" "$H/kernel.macho"

# ---------------------------------------------------------------- 6. sha
say "verifying the kernel inside the payload"
rm -f /tmp/pk_verify.macho
mcopy -o -i "$H/payload.img" ::/EFI/UniXNU/Payloads/kernel.macho /tmp/pk_verify.macho \
    || die "cannot extract kernel from payload"
sha_in=$(sha256sum /tmp/pk_verify.macho | cut -d' ' -f1)
[ "$sha_in" = "$sha_built" ] || die "payload kernel $sha_in != built $sha_built"
cp -f "$K" "$H/kernel.macho"

# ---------------------------------------------------------------- 7. layout
say "checking payload layout"
python3 -I /mnt/d/_nabu-usb-cdc/tools/payload_layout_check.py "$H/payload.img" \
    || die "payload layout is not the booting one"

# ---------------------------------------------------------------- 8. boot img
boot_sha=$(sha256sum "$H/boot-merge-r3.img" | cut -c1-16)
[ "$boot_sha" = "ead4b53135d67516" ] || die "boot-merge-r3.img changed ($boot_sha)"
[ "$(stat -c %s "$H/boot-merge-r3.img")" = "3041280" ] || die "boot image size changed"

# ---------------------------------------------------------------- 9. args
say "boot args"
python3 -I /mnt/e/nabu/tmp/args_count.py "$H/payload.img" 2>/dev/null | head -2

echo
say "READY: $H"
echo "  kernel  : $sha_built"
echo "  payload : $(sha256sum "$H/payload.img" | cut -c1-16)"
echo "  boot    : $boot_sha"
echo
echo "Flash with: bash /e/nabu/tmp/go_round_next.sh $TAG"
