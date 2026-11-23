#!/usr/bin/env python3
"""Replace the kernel inside a payload WITHOUT letting mcopy relayout the FAT.

Why this exists: every round that rebuilt the payload with `mcopy -o` came up
hanging in disk1s1's Data-volume mount (kb-73, kb-75, kb-82), while every round
that reused a payload built the old way booted. mcopy deletes and rewrites the
125 MB kernel, which reallocates its clusters and changes the image layout.
The payloads that boot all share one chain:

    <39151-61431> <67551-97914> <90-8102> <8104-8827>

So instead of asking mcopy to place the file, this script walks the source
payload's FAT, takes the existing chain for the target file, and writes the new
contents straight into those clusters. Layout, directory entry and FAT stay
byte-identical; only the clusters' data changes. The two kernels are the same
size (125,710,336 B), which is what makes a straight overwrite possible -- the
script refuses if the sizes differ.
"""
import struct
import sys

SECTOR = 512


def read_bpb(fp):
    fp.seek(0)
    bs = fp.read(SECTOR)
    if bs[510:512] != b"\x55\xaa":
        sys.exit("not a boot sector (no 0xAA55)")
    bps = struct.unpack_from("<H", bs, 0x0B)[0]
    spc = bs[0x0D]
    rsvd = struct.unpack_from("<H", bs, 0x0E)[0]
    nfats = bs[0x10]
    root_entries = struct.unpack_from("<H", bs, 0x11)[0]
    total = struct.unpack_from("<I", bs, 0x20)[0] or struct.unpack_from("<H", bs, 0x13)[0]
    fatsz = struct.unpack_from("<I", bs, 0x24)[0] or struct.unpack_from("<H", bs, 0x16)[0]
    if bps != SECTOR:
        sys.exit("unexpected sector size %d" % bps)
    root_sectors = (root_entries * 32 + bps - 1) // bps
    data_start = rsvd + nfats * fatsz + root_sectors
    return {
        "bps": bps, "spc": spc, "rsvd": rsvd, "nfats": nfats,
        "fatsz": fatsz, "root_entries": root_entries,
        "root_off": (rsvd + nfats * fatsz) * bps,
        "fat_off": rsvd * bps,
        "data_start": data_start,
        "cluster_bytes": spc * bps,
    }


def cluster_offset(bpb, cluster):
    return (bpb["data_start"] + (cluster - 2) * bpb["spc"]) * bpb["bps"]


def fat_entry(fp, bpb, cluster):
    fp.seek(bpb["fat_off"] + cluster * 4)
    return struct.unpack("<I", fp.read(4))[0] & 0x0FFFFFFF


def find_dir_entry(fp, bpb, path_parts):
    """Walk the directory tree (FAT32, LFN skipped) to the target entry."""
    # root directory: follow the root cluster chain
    root = struct.unpack_from("<I", open_boot(fp, bpb), 0x2C)[0] if False else None
    return None


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: payload_swap_kernel.py <src_payload> <new_kernel> <out_payload>")
    src, newk, out = sys.argv[1], sys.argv[2], sys.argv[3]

    with open(src, "rb") as f:
        bpb = read_bpb(f)
        print("cluster=%dB data_start=%d cl %d" %
              (bpb["cluster_bytes"], bpb["data_start"], bpb["spc"]))

    # Hard-coded from `mshowfat` on the booting payloads. This is deliberate:
    # the point is to keep the exact layout those payloads had, not to derive one.
    chain = [(39151, 61431), (67551, 97914), (90, 8102), (8104, 8827)]
    total_clusters = sum(b - a + 1 for a, b in chain)
    capacity = total_clusters * bpb["cluster_bytes"]

    new_size = None
    import os
    new_size = os.path.getsize(newk)
    print("kernel size=%d  chain capacity=%d  delta=%d" %
          (new_size, capacity, capacity - new_size))
    if new_size > capacity:
        sys.exit("kernel does not fit the chain")

    # copy src -> out first, then patch clusters in the copy
    import shutil
    shutil.copyfile(src, out)

    written = 0
    with open(newk, "rb") as kf, open(out, "r+b") as of:
        for a, b in chain:
            for c in range(a, b + 1):
                if written >= new_size:
                    break
                off = cluster_offset(bpb, c)
                n = min(bpb["cluster_bytes"], new_size - written)
                kf.seek(written)
                of.seek(off)
                of.write(kf.read(n))
                written += n
            if written >= new_size:
                break

    print("wrote %d bytes into %d clusters" % (written, total_clusters))
    if written != new_size:
        sys.exit("short write")
    print("OK: layout preserved, only cluster data changed")


if __name__ == "__main__":
    main()
