#!/bin/bash
# THE ONLY WAY TO PACK A PAYLOAD.
#
# Do not call `mcopy` on a payload to replace kernel.macho. It deletes and
# re-allocates the 125 MB file, which changes the FAT cluster layout, and four
# board rounds (kb-73, kb-75, kb-82, and kb-73's retry) then hung in disk1s1's
# Data-volume mount with `nabu:` stopping after six lines. Every payload that
# booted shares one cluster chain:
#
#   <39151-61431> <67551-97914> <90-8102> <8104-8827>
#
# payload_swap_kernel.py writes the new kernel into exactly those clusters and
# leaves the directory entry, the FAT and the rest of the image untouched. This
# script drives it, verifies the result, and refuses to produce a package whose
# layout is not the known-booting one.
#
# Usage:  pack_round.sh <new-tag> [<base-tag>]
#         base-tag defaults to kb-70-kdbgfix
set -eu

TAG=${1:?usage: pack_round.sh <new-tag> [<base-tag>]}
BASE_TAG=${2:-kb-70-kdbgfix}
H=/mnt/e/nabu/hold/$TAG
B=/mnt/e/nabu/hold/$BASE_TAG
K=/root/merge/l738-full/out/nabu/ipados-17.7.11-21H461/nabu/kernel/kernel.macho

echo "== base: $BASE_TAG =="
ls -l "$B/payload.img" "$B/boot-merge-r3.img" "$K" || {
    echo "STOP: missing input"; exit 1; }

rm -rf "$H"; mkdir -p "$H"

echo "== swap kernel in place (layout preserved) =="
python3 -I /mnt/d/_nabu-usb-cdc/tools/payload_swap_kernel.py \
    "$B/payload.img" "$K" "$H/payload.img"

cp -f "$B"/kernel.macho.adt "$B"/boot-merge-r3.img \
      "$B"/gpu-addon.bin "$B"/owner-trust.bundle "$H"/
cp -f "$K" "$H/kernel.macho"

echo
echo "== three sha (must match) =="
echo -n "host      : "; sha256sum "$K" | cut -c1-20
rm -f /tmp/pk_check.macho
mcopy -o -i "$H/payload.img" ::/EFI/UniXNU/Payloads/kernel.macho /tmp/pk_check.macho
echo -n "in payload: "; sha256sum /tmp/pk_check.macho | cut -c1-20
echo -n "hold copy : "; sha256sum "$H/kernel.macho" | cut -c1-20
echo -n "boot red  : "; sha256sum "$H/boot-merge-r3.img" | cut -c1-16

echo
echo "== NOP18 gate (the kernel must already be patched) =="
# Every round that shipped an unpatched kernel hung in disk1s1's mount:
# kb-73, kb-75, kb-82, kb-83, kb-89, kb-90, kb-91. Do not pack an unpatched one.
if ! python3 - "$K" <<'PYEOF'
import sys
d = open(sys.argv[1], "rb").read()
i = d.find(bytes.fromhex("6100005408214439"))
if i < 0:
    print("NOP18: SIGNATURE NOT FOUND"); sys.exit(1)
cur = d[i + 8:i + 12].hex()
if cur == "1f2003d5":
    print("NOP18: patched (nop present)"); sys.exit(0)
if cur == "a8000037":
    print("NOP18: UNPATCHED - run /e/nabu/tools/_nop18_l800.py first"); sys.exit(1)
print("NOP18: UNEXPECTED bytes " + cur); sys.exit(1)
PYEOF
then
    echo "STOP: refusing to pack a kernel without NOP18"; exit 1
fi

echo
echo "== layout gate (the point of this script) =="
python3 -I /mnt/d/_nabu-usb-cdc/tools/payload_layout_check.py "$H/payload.img" || {
    echo "STOP: packed payload does not have the booting layout"; exit 1; }

echo
echo "== args (informational: a boot-arg change needs sync_payload_args.py,"
echo "   and that rewrites the toml only -- it must not touch the kernel) =="
python3 -I /mnt/e/nabu/tmp/args_count.py "$H/payload.img" | head -2

echo
echo "== done: $H =="
ls -l "$H"
