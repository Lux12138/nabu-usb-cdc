#!/usr/bin/env python3
"""Replace the kernel inside a payload WITHOUT letting mcopy relayout the FAT.

Why this exists: every round that rebuilt the payload with `mcopy -o` came up
hanging in disk1s1's Data-volume mount (kb-73, kb-75, kb-82), while every round
that reused a payload built the old way booted. mcopy deletes and rewrites the
125 MB kernel, which reallocates its clusters and changes the image layout.

So instead of asking mcopy to place the file, this walks the source payload's
FAT, takes the existing chain for the target file, and writes the new contents
straight into those clusters. Layout, directory entry and FAT stay byte
identical; only the clusters' data changes.

The chain is read from the FAT rather than hard-coded. An earlier version carried
the four-segment chain of a long-gone fragmented payload; by 2026-11 every
package on disk is a single run <90-61511> (61,422 clusters x 2048 B =
125,792,256 B, exactly the kernel size). Keeping the old constant made the
capacity check report 125,710,336 B -- 81,920 short -- and refuse a swap that
was in fact fine.

The kernel must fit the existing chain. This never extends it: a longer file
means the payload needs rebuilding, not patching.
"""
import struct
import sys

SECTOR = 512

# Path of the kernel inside the payload, as FAT directory entries (8.3 names).
KERNEL_PATH = ["EFI", "UNIXNU", "PAYLOADS", "KERNEL~1.MAC"]


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
        "root_cluster": struct.unpack_from("<I", bs, 0x2C)[0],
        "data_start": data_start,
        "cluster_bytes": spc * bps,
    }


def cluster_offset(bpb, cluster):
    return (bpb["data_start"] + (cluster - 2) * bpb["spc"]) * bpb["bps"]


def fat_entry(fp, bpb, cluster):
    fp.seek(bpb["fat_off"] + cluster * 4)
    return struct.unpack("<I", fp.read(4))[0] & 0x0FFFFFFF


def follow(fp, bpb, start):
    """Walk the FAT from `start` and return every cluster in order."""
    out = []
    c = start
    seen = set()
    while 2 <= c < 0x0FFFFFF8:
        if c in seen:
            sys.exit("FAT cycle at cluster %d" % c)
        seen.add(c)
        out.append(c)
        c = fat_entry(fp, bpb, c)
    return out


def read_dir(fp, bpb, cluster):
    """Yield (short_name_upper, first_cluster, size) for a directory cluster chain."""
    for c in follow(fp, bpb, cluster):
        fp.seek(cluster_offset(bpb, c))
        block = fp.read(bpb["cluster_bytes"])
        for off in range(0, len(block), 32):
            e = block[off:off + 32]
            if len(e) < 32 or e[0] == 0x00:
                return
            if e[0] == 0xE5 or e[11] == 0x0F:
                continue
            if e[11] & 0x08:          # volume label
                continue
            name = e[0:8].decode("latin-1").rstrip()
            ext = e[8:11].decode("latin-1").rstrip()
            short = name if not ext else name + "." + ext
            first = struct.unpack_from("<H", e, 0x1A)[0]
            hi = struct.unpack_from("<H", e, 0x14)[0]
            first |= hi << 16
            size = struct.unpack_from("<I", e, 0x1C)[0]
            yield short.upper(), first, size


def find_kernel_chain(fp, bpb, path=KERNEL_PATH):
    """Resolve the kernel's cluster chain by walking directories from the root."""
    cur = bpb["root_cluster"]
    for want in path[:-1]:
        nxt = None
        for name, first, _size in read_dir(fp, bpb, cur):
            if name == want:
                nxt = first
                break
        if nxt is None:
            sys.exit("directory not found in payload: %s" % want)
        cur = nxt

    leaf = path[-1]
    for name, first, size in read_dir(fp, bpb, cur):
        if name == leaf:
            chain = follow(fp, bpb, first)
            return chain, size
    sys.exit("kernel not found in payload (looked for %s)" % leaf)


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: payload_swap_kernel.py <src_payload> <new_kernel> <out_payload>")
    src, newk, out = sys.argv[1], sys.argv[2], sys.argv[3]

    import os
    new_size = os.path.getsize(newk)

    with open(src, "rb") as f:
        bpb = read_bpb(f)
        print("cluster=%dB data_start=%d cl %d" %
              (bpb["cluster_bytes"], bpb["data_start"], bpb["spc"]))
        chain, dirent_size = find_kernel_chain(f, bpb)

    print("source chain: %d clusters (first %d, last %d), dirent size=%d" %
          (len(chain), chain[0], chain[-1], dirent_size))

    capacity = len(chain) * bpb["cluster_bytes"]
    print("kernel size=%d  chain capacity=%d  delta=%d" %
          (new_size, capacity, capacity - new_size))
    if new_size > capacity:
        sys.exit("kernel does not fit the chain (%d > %d); rebuild the payload"
                 % (new_size, capacity))
    if new_size != dirent_size:
        # Not fatal, but the directory entry records the old size and Explorer
        # would show a stale length. Both kernels have been the same size so far.
        print("WARNING: kernel is %d B but the directory entry says %d B"
              % (new_size, dirent_size))

    import shutil
    shutil.copyfile(src, out)

    written = 0
    with open(newk, "rb") as kf, open(out, "r+b") as of:
        for c in chain:
            if written >= new_size:
                break
            off = cluster_offset(bpb, c)
            n = min(bpb["cluster_bytes"], new_size - written)
            kf.seek(written)
            of.seek(off)
            of.write(kf.read(n))
            written += n

    print("wrote %d bytes into %d clusters" % (written, len(chain)))
    if written != new_size:
        sys.exit("short write")
    print("OK: layout preserved, only cluster data changed")


if __name__ == "__main__":
    main()
