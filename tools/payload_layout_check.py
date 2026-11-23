#!/usr/bin/env python3
"""Assert a payload's kernel cluster layout will boot.

History that shaped this check: four rounds (kb-73/75/82/83) hung in disk1s1's
Data-volume mount after `mcopy -o` replaced the 125 MB kernel inside an existing
payload. mcopy deletes and reallocates, so the layout changed. Every payload that
booted at the time shared one chain:

    39151-61431  67551-97914  90-8102  8104-8827

But that chain was an artefact of how those payloads were built, not a
requirement. A payload generated fresh by `unixnu payload` lays the kernel down
in one contiguous run (kb-100 measured `<6-61387>`, exactly 61382 clusters =
125,710,336 bytes), which is strictly better than any fragmented chain.

So the rule is not "match this exact chain" -- it is "the kernel occupies a
single contiguous run, or the known-good historical chain". Anything else means
somebody rewrote the file in place and must be re-packed with
payload_swap_kernel.py.

Usage:
    payload_layout_check.py <payload.img>
Exit 0 = layout is one that boots, non-zero otherwise.
"""
import re
import subprocess
import sys

HISTORICAL_GOOD = "<39151-61431> <67551-97914> <90-8102> <8104-8827>"
KERNEL_BYTES = 125710336
CLUSTER_BYTES = 2048


def kernel_chain(payload):
    r = subprocess.run(
        ["mshowfat", "-i", payload, "::/EFI/UniXNU/Payloads/kernel.macho"],
        capture_output=True, text=True)
    if r.returncode:
        return None, "mshowfat failed: " + (r.stderr or "").strip()[:200]
    m = re.search(r"kernel\.macho\s+(.*)$", r.stdout.strip())
    if not m:
        return None, "no cluster list in: " + r.stdout.strip()[:200]
    return m.group(1).strip(), None


def parse_runs(chain):
    runs = []
    for a, b in re.findall(r"<(\d+)-(\d+)>", chain):
        runs.append((int(a), int(b)))
    return runs


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: payload_layout_check.py <payload.img>")
    payload = sys.argv[1]
    chain, err = kernel_chain(payload)
    if err:
        print("LAYOUT FAIL: %s" % err)
        sys.exit(1)
    print("kernel chain: %s" % chain)

    if chain == HISTORICAL_GOOD:
        print("LAYOUT OK: the historical 4-run chain that booted kb-56..kb-99")
        sys.exit(0)

    runs = parse_runs(chain)
    if len(runs) == 1:
        a, b = runs[0]
        have = (b - a + 1) * CLUSTER_BYTES
        if have >= KERNEL_BYTES:
            print("LAYOUT OK: single contiguous run of %d clusters (%d bytes)"
                  % (b - a + 1, have))
            sys.exit(0)
        print("LAYOUT FAIL: contiguous but too small (%d < %d)" % (have, KERNEL_BYTES))
        sys.exit(1)

    print("LAYOUT FAIL: %d runs and not the known-good chain." % len(runs))
    print("  A freshly generated payload is one contiguous run; the historical")
    print("  good chain is %s." % HISTORICAL_GOOD)
    print("  A fragmented chain means the kernel was replaced with `mcopy`.")
    print("  Re-pack with /e/nabu/tmp/payload_swap_kernel.py instead.")
    sys.exit(1)


if __name__ == "__main__":
    main()
