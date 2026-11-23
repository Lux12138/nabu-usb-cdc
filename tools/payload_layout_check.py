#!/usr/bin/env python3
"""Assert a payload's kernel cluster layout is one that actually boots.

Why this is a hard gate and not a note in a document: four board rounds came up
hanging in disk1s1's Data-volume mount with `nabu:` stopping after six lines,
and every one of them had a payload whose kernel.macho had been rewritten by
`mcopy -o`. mcopy deletes and re-allocates the 125 MB file, so the cluster
layout changes. Every payload that booted shares the layout below; every one
that hung had a different one.

    39151-61431  67551-97914  90-8102  8104-8827

The layout requirement is empirical, not understood. That is exactly why it is
enforced mechanically: "we found the cause" did not stop it happening three
times. This does.

The chain is read from `mshowfat`, because mtools already parses FAT correctly
and a second implementation here would only be another thing to get wrong.

Usage:
    payload_layout_check.py <payload.img>
Exit 0 = layout matches a known-booting payload, non-zero otherwise.
"""
import re
import subprocess
import sys

KNOWN_GOOD = "<39151-61431> <67551-97914> <90-8102> <8104-8827>"


def kernel_chain(payload):
    r = subprocess.run(
        ["mshowfat", "-i", payload, "::/EFI/UniXNU/Payloads/kernel.macho"],
        capture_output=True, text=True)
    if r.returncode:
        return None, "mshowfat failed: " + (r.stderr or "").strip()[:200]
    line = r.stdout.strip()
    m = re.search(r"kernel\.macho\s+(.*)$", line)
    if not m:
        return None, "no cluster list in: " + line[:200]
    return m.group(1).strip(), None


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: payload_layout_check.py <payload.img>")
    payload = sys.argv[1]
    chain, err = kernel_chain(payload)
    if err:
        print("LAYOUT FAIL: %s" % err)
        sys.exit(1)
    print("kernel chain: %s" % chain)
    if chain == KNOWN_GOOD:
        print("LAYOUT OK: matches the chain every booting payload has used")
        sys.exit(0)
    print("LAYOUT FAIL: not the chain the booting payloads use.")
    print("  expected: %s" % KNOWN_GOOD)
    print("  The kernel was almost certainly rewritten with `mcopy -o`, which")
    print("  reallocates 125 MB of clusters. Use /e/nabu/tmp/payload_swap_kernel.py")
    print("  instead: it writes the new kernel into the existing clusters and")
    print("  leaves the directory entry, FAT and layout untouched.")
    sys.exit(1)


if __name__ == "__main__":
    main()
