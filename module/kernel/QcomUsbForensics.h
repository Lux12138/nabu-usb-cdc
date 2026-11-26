/* SPDX-License-Identifier: MIT
 * Pure, allocation-free parsing and planning for the forensics verbs
 * (DEVTREE / FBRAW / NABULOGRING). This header performs no I/O and touches no
 * memory: the kernel handler calls it after CRC-validated arguments arrive,
 * and the host conformance tests include it directly.
 */
#ifndef QCOM_USB_FORENSICS_H
#define QCOM_USB_FORENSICS_H

#include <stddef.h>
#include <stdint.h>

/* NABULOG1 slot header layout, measured from the periodic/panic slots on the
 * userdata tail window (T117-persisted-log-findings.md section 1):
 *   magic[8] version u32 is_panic u32 available u32 dropped u32 total_lo u32 total_hi u32
 * All fields little-endian, header total 32 bytes. The body text starts right
 * after the header and runs to min(available, slot_bytes - 32). */
#define QUD_NABULOG_MAGIC "NABULOG1"
#define QUD_NABULOG_MAGIC_BYTES 8u
#define QUD_NABULOG_HEADER_BYTES 32u

struct qud_nabulog_header {
    uint8_t magic[QUD_NABULOG_MAGIC_BYTES];
    uint32_t version, is_panic, available, dropped, total_lo, total_hi;
};

static inline uint32_t
qud_load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Parse one slot header. Returns 1 on success, 0 when fewer than
 * QUD_NABULOG_HEADER_BYTES bytes are available, -1 on a bad magic. */
static inline int
qud_nabulog_header_parse(const uint8_t *hdr, size_t bytes,
                         struct qud_nabulog_header *out)
{
    size_t i;
    if (!hdr || !out) return 0;
    if (bytes < QUD_NABULOG_HEADER_BYTES) return 0;
    for (i = 0; i < QUD_NABULOG_MAGIC_BYTES; i++)
        if (hdr[i] != (uint8_t)QUD_NABULOG_MAGIC[i]) return -1;
    for (i = 0; i < QUD_NABULOG_MAGIC_BYTES; i++) out->magic[i] = hdr[i];
    out->version = qud_load_le32(hdr + 8);
    out->is_panic = qud_load_le32(hdr + 12);
    out->available = qud_load_le32(hdr + 16);
    out->dropped = qud_load_le32(hdr + 20);
    out->total_lo = qud_load_le32(hdr + 24);
    out->total_hi = qud_load_le32(hdr + 28);
    return 1;
}

/* Clamp a body window [off, off+len) against the slot's available byte
 * count. Returns 1 with *want set (0 is legal: empty window), or 0 when the
 * offset lies beyond available (a hard error, not an empty read). */
static inline int
qud_nabulog_body_window(uint64_t off, uint64_t len, uint64_t available,
                        uint64_t *want)
{
    if (!want) return 0;
    if (off > available) return 0;
    *want = len;
    if (*want > available - off) *want = available - off;
    return 1;
}

/* Live geometry: the log window opened by native_ufs_log_open() covers the
 * last kLogReserveBlocks (8192 x 4 KiB) of userdata. The periodic slot is the
 * first block of the window, the panic slot the third-from-last block
 * (blocks - 2), matching anchors 121718715 / 121726905 measured live. */
static inline int
qud_nabulog_slot_rel(uint64_t blocks, int panic, uint64_t *rel)
{
    if (!rel || blocks < 2u) return 0;
    *rel = panic ? blocks - 2u : 0u;
    return 1;
}

/* Plan one FBRAW transfer step. len is the total range, off the current
 * offset, chunk_max the per-reply ceiling. Returns 1 with *chunk set and
 * *last = (off + chunk >= len); 0 on bad arguments. */
static inline int
qud_fbraw_plan(uint64_t off, uint64_t len, uint32_t chunk_max,
               uint32_t *chunk, int *last)
{
    uint64_t left;
    if (!chunk || !last || !chunk_max || off > len) return 0;
    left = len - off;
    *chunk = left > chunk_max ? chunk_max : (uint32_t)left;
    *last = (off + *chunk >= len) ? 1 : 0;
    return 1;
}

/* DEVTREE path argument: non-empty, '/' prefixed, printable, bounded. */
static inline int
qud_devtree_path_valid(const char *path, size_t max_bytes)
{
    size_t n = 0;
    if (!path) return 0;
    if (path[0] != '/') return 0;
    while (path[n]) {
        if (n >= max_bytes) return 0;
        if (path[n] < 0x21 || path[n] > 0x7e) return 0;
        n++;
    }
    return n > 0;
}

/* Prefix filter used by DEVTREE: everything under filter matches, the root
 * filter "/" matches all, and an exact path matches itself. */
static inline int
qud_path_or_prefix_match(const char *path, const char *filter)
{
    size_t n = 0;
    if (!path || !filter) return 0;
    if (filter[0] == '/' && filter[1] == 0) return 1;
    while (filter[n] && path[n] && filter[n] == path[n]) n++;
    if (filter[n]) return 0;
    return path[n] == 0 || path[n] == '/';
}

#endif /* QCOM_USB_FORENSICS_H */
