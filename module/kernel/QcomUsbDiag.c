/* SPDX-License-Identifier: MIT
 * Kernel memory and diagnostics access for the USB control console.
 *
 * This file is the ONLY place that touches arbitrary memory. The framing
 * layer (QcomUsbControl.h) deliberately stays free of memory operations, and
 * everything here is reached from control_handle_frame() only for a verb that
 * arrived in a CRC-validated request.
 *
 * Reading uses the hardware address-translation instruction through
 * mmu_kvtop():
 *   - it does not walk page tables and takes no pmap lock, so it is safe from
 *     the USB worker thread and cannot deadlock against the pmap
 *   - an unmapped address makes the hardware report a fault and the helper
 *     returns 0, so a bad address yields an ERR reply instead of a panic
 * that is the same technique pmap_find_pa_nofault() uses for kernel_pmap
 * (osfmk/arm/pmap/pmap.c:6701) and the same one KDP relies on.
 *
 * The two kernel symbols are declared here rather than pulled in from
 * osfmk/arm/pmap/pmap.h, because that header is not part of the exported
 * kext header set this module builds against.
 *
 * Writing is compiled in but gated behind qcom-usb-write=1 so a stray request
 * cannot corrupt a running system during ordinary capture rounds.
 */
#ifndef QCOM_USB_DIAG_C
#define QCOM_USB_DIAG_C

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <pexpert/pexpert.h>

/* From osfmk/arm/pmap/pmap.h:299 and mach/vm_param.h (phystokv). Only the
 * pieces this file needs are declared. */
extern uint64_t mmu_kvtop(uint64_t va);
extern uintptr_t phystokv(uint64_t phys);

/* One READ/READNEXT chunk: 128 bytes render as 256 hex characters, which fits
 * inside SXNU_CONTROL_REPLY_BYTES (512) together with the state/crc fields. */
#define QUD_DIAG_CHUNK 192u   /* 384 hex chars + header < 512-byte reply cap */
/* Largest range a single READ may name; the host walks it with READNEXT. */
#define QUD_DIAG_MAX_LEN (1u << 20)
/* One physical mapping is at most one page of Io memory. */
#define QUD_DIAG_MAX_PHYS (1u << 10)

struct qud_diag_cursor {
    uint64_t va;        /* base address of the pending read          */
    uint32_t length;    /* total bytes requested                     */
    uint32_t offset;    /* next byte to hand out                     */
    uint32_t frame;     /* command sequence that created the cursor  */
    uint32_t crc;       /* CRC32 over the whole range                */
    uint8_t  active;
    uint8_t  is_phys;
    uint8_t  reserved[2];
};

static struct qud_diag_cursor qud_diag_cursor;
static int qud_diag_write_enabled = -1;

static void
qud_diag_cursor_clear(void)
{
    memset(&qud_diag_cursor, 0, sizeof(qud_diag_cursor));
}

static int
qud_diag_writes_allowed(void)
{
    if (qud_diag_write_enabled < 0) {
        int value = 0;
        PE_parse_boot_argn("qcom-usb-write", &value, sizeof(value));
        qud_diag_write_enabled = value ? 1 : 0;
    }
    return qud_diag_write_enabled;
}

/* Turn one kernel virtual address into something safely readable, or report
 * that the page is not mapped. */
static int
qud_diag_safe_pointer(uint64_t va, uint8_t **out)
{
    const uint64_t page = va & ~UINT64_C(0xfff);
    const uint64_t phys = mmu_kvtop(page);

    if (!phys) {
        return 0;
    }
    /* phystokv maps the physical page into the kernel's linear window, which
     * the kernel can always read. */
    *out = (uint8_t *)(phystokv(phys) | (uintptr_t)(va & 0xfff));
    return 1;
}

/* Copy len bytes without ever dereferencing an unmapped page. Returns the
 * number of bytes actually copied; a short count means a page was missing. */
static size_t
qud_diag_read(uint64_t va, void *dst, size_t len)
{
    uint8_t *out = dst;
    size_t done = 0;

    while (done < len) {
        const uint64_t cur = va + done;
        const size_t page_off = (size_t)(cur & 0xfff);
        const size_t room = 4096u - page_off;
        const size_t chunk = (len - done) < room ? (len - done) : room;
        uint8_t *src = NULL;

        if (!qud_diag_safe_pointer(cur, &src)) {
            break;
        }
        memcpy(out + done, src, chunk);
        done += chunk;
    }
    return done;
}

/* Writes are all-or-nothing: every page must be mapped before a byte lands, so
 * a straddling request cannot half-apply. Returns 1 on success, 0 when a page
 * is missing, -1 when writes are disabled by boot-arg. */
static int
qud_diag_write(uint64_t va, const void *src, size_t len)
{
    const uint8_t *in = src;
    size_t done = 0;

    if (!qud_diag_writes_allowed()) {
        return -1;
    }
    for (done = 0; done < len; done += 4096) {
        uint8_t *probe = NULL;
        if (!qud_diag_safe_pointer(va + done, &probe)) {
            return 0;
        }
    }
    for (done = 0; done < len; ) {
        const uint64_t cur = va + done;
        const size_t page_off = (size_t)(cur & 0xfff);
        const size_t room = 4096u - page_off;
        const size_t chunk = (len - done) < room ? (len - done) : room;
        uint8_t *dst = NULL;
        if (!qud_diag_safe_pointer(cur, &dst)) {
            return 0;
        }
        memcpy(dst, in + done, chunk);
        done += chunk;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
}

/* Read a physical range through a temporary device mapping. Returns the
 * number of bytes copied (0 on failure). ml_io_map() takes a byte size,
 * internally rounds to pages, and ml_io_unmap must get the same size. */
/* One cached device window. The console reads a few register blocks repeatedly
 * (MDSS, GPUCC, SMMU); reusing one permanent mapping avoids leaking a VM entry
 * per command. Different windows evict the previous one -- the mapping stays
 * valid in the kernel (XNU never unmaps IO space), we just stop tracking it. */
static uint64_t phys_cache_base;
static uint64_t phys_cache_span;
static uintptr_t phys_cache_va;

static size_t
qud_diag_read_phys(uint64_t pa, void *dst, size_t len)
{
    const uint64_t base = pa & ~UINT64_C(0xfff);
    const uint64_t off = pa & UINT64_C(0xfff);
    const uint64_t span = (off + len + 0xfffu) & ~UINT64_C(0xfff);
    uintptr_t va;

    if (phys_cache_va && base >= phys_cache_base &&
        (base + span) <= (phys_cache_base + phys_cache_span)) {
        va = phys_cache_va;
    } else {
        va = ml_io_map(base, span);
        if (!va) return 0;
        phys_cache_base = base;
        phys_cache_span = span;
        phys_cache_va = va;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    memcpy(dst, (const void *)(va + off), len);
    __asm__ volatile("dsb sy" ::: "memory");
    /* XNU never calls ml_io_unmap: device mappings from ml_io_map are
       permanent (all XNU callers keep them). Unmapping panics vm_map_delete
       on a permanent entry. */
    return len;
}

/* ---- WRITEPA: MMIO-only physical write ---------------------------------- */
/* Whitelist of MMIO windows a WRITEPA may touch. DDR physical addresses are
 * deliberately excluded: a stray write to RAM would corrupt a running system
 * without any fault. Only the SoC register blocks the console needs are listed
 * here; anything else returns "denied". Ranges are [lo, hi) in bytes. */
struct qud_mmio_range { uint64_t lo, hi; };
static const struct qud_mmio_range qud_write_whitelist[] = {
    { UINT64_C(0x0a600000), UINT64_C(0x0a700000) }, /* DWC3 USB controller */
    { UINT64_C(0x01000000), UINT64_C(0x01010000) }, /* SMMU */
    { UINT64_C(0x01050000), UINT64_C(0x01090000) }, /* GCC (partial) */
    { UINT64_C(0x02c90000), UINT64_C(0x02ca0000) }, /* GPUCC */
    { UINT64_C(0x0ae00000), UINT64_C(0x0af00000) }, /* MDSS */
    { UINT64_C(0x0af00000), UINT64_C(0x0af01000) }, /* disp_cc */
};

/* Returns 1 if the whole [pa, pa+len) lies inside one whitelisted MMIO range. */
static int
qud_write_pa_allowed(uint64_t pa, size_t len)
{
    if (!len) return 0;
    for (unsigned i = 0; i < sizeof(qud_write_whitelist)/sizeof(qud_write_whitelist[0]); i++) {
        const struct qud_mmio_range *r = &qud_write_whitelist[i];
        if (pa >= r->lo && (pa + (uint64_t)len) <= r->hi) return 1;
    }
    return 0;
}

/* Write len bytes to a physical MMIO address through the cached device window.
 * Returns 1 on success, 0 on bad args, -1 when writes are boot-arg disabled,
 * -2 when the PA is outside the MMIO whitelist, -3 on mapping failure.
 * A successful write is self-checked by reading back the first 32-bit word. */
static int
qud_diag_write_phys(uint64_t pa, const void *src, size_t len, uint32_t *readback)
{
    const uint64_t base = pa & ~UINT64_C(0xfff);
    const uint64_t off = pa & UINT64_C(0xfff);
    const uint64_t span = (off + len + 0xfffu) & ~UINT64_C(0xfff);
    uintptr_t va;
    volatile uint32_t *dst;

    if (!qud_diag_writes_allowed()) return -1;
    if (!src || !len) return 0;
    if (!qud_write_pa_allowed(pa, len)) return -2;
    /* MMIO registers are 32-bit; require 4-byte alignment and length. */
    if (pa & 3u) return 0;
    if (len & 3u) return 0;

    if (phys_cache_va && base >= phys_cache_base &&
        (base + span) <= (phys_cache_base + phys_cache_span)) {
        va = phys_cache_va;
    } else {
        va = ml_io_map(base, span);
        if (!va) return -3;
        phys_cache_base = base;
        phys_cache_span = span;
        phys_cache_va = va;
    }
    dst = (volatile uint32_t *)(va + off);
    memcpy((void *)dst, src, len);
    __asm__ volatile("dsb sy" ::: "memory");
    if (readback) *readback = dst[0];   /* self-attest: first word */
    return 1;
}


/* ---- DUMPUFS: kernel-side async dump to UFS export region ----------------
 * Storage driver exports native_ufs_export_write(); weak_import so a kernel
 * without the UFS log window still builds (verb returns unsupported).
 * The worker calls qud_dump_ufs_progress() every 5 ms; each tick writes at
 * most a few 4 KiB blocks, so no single call blocks the CDC path >10 ms. */
extern int native_ufs_export_write(uint64_t rel, uint32_t blocks, const void *src)
    __attribute__((weak_import));
extern uint64_t native_ufs_export_blocks(void) __attribute__((weak_import));
extern uint64_t native_ufs_export_first(void) __attribute__((weak_import));

#define QUD_DUMP_SLOTS 4u
#define QUD_DUMP_TICKBYTES (16u * 4096u)   /* <=64 KiB per 5 ms tick */

struct qud_dump_slot {
    uint64_t pa;          /* source physical address */
    uint32_t len;         /* total bytes */
    uint32_t done;        /* bytes written so far */
    uint32_t rel;         /* export-region block offset */
    uint8_t  state;       /* 0 free 1 queued 2 running 3 done 4 error */
    uint8_t  tag[16];     /* NUL-padded host tag */
};
static struct qud_dump_slot qud_dump_slots[QUD_DUMP_SLOTS];
static uint8_t qud_dump_scratch[4096];

int qud_dump_ufs_available(void)
{
    return (&native_ufs_export_write != 0) && (&native_ufs_export_blocks != 0);
}

/* Enqueue a dump. Returns slot index 0..N-1, or -1 full, -2 unsupported,
 * -3 bad args. Only the worker calls native_ufs_export_write(). */
int qud_dump_ufs_enqueue(uint64_t pa, uint32_t len, const uint8_t *tag, uint32_t taglen)
{
    if (!qud_dump_ufs_available()) return -2;
    if (!len || (len & 0xfffu) || native_ufs_export_blocks() < (len / 4096u)) return -3;
    for (uint32_t i = 0; i < QUD_DUMP_SLOTS; i++) {
        struct qud_dump_slot *s = &qud_dump_slots[i];
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != 0) continue;
        s->pa = pa; s->len = len; s->done = 0; s->rel = 0;
        memset(s->tag, 0, sizeof(s->tag));
        for (uint32_t j = 0; j < taglen && j < sizeof(s->tag) - 1; j++)
            s->tag[j] = tag[j];
        __atomic_store_n(&s->state, 1u, __ATOMIC_RELEASE);
        return (int)i;
    }
    return -1;
}

/* Worker tick: advance queued/running dumps by at most QUD_DUMP_TICKBYTES.
 * Runs outside the USB busy lock; reads PA through the cached IO window. */
void qud_dump_ufs_progress(void)
{
    if (!qud_dump_ufs_available()) return;
    for (uint32_t i = 0; i < QUD_DUMP_SLOTS; i++) {
        struct qud_dump_slot *s = &qud_dump_slots[i];
        uint32_t st = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
        if (st != 1 && st != 2) continue;
        __atomic_store_n(&s->state, 2u, __ATOMIC_RELEASE);
        uint32_t budget = QUD_DUMP_TICKBYTES;
        while (s->done < s->len && budget >= 4096u) {
            size_t got = qud_diag_read_phys(s->pa + s->done, qud_dump_scratch, 4096u);
            if (got != 4096u) { __atomic_store_n(&s->state, 4u, __ATOMIC_RELEASE); break; }
            if (!native_ufs_export_write(s->rel, 1u, qud_dump_scratch)) {
                __atomic_store_n(&s->state, 4u, __ATOMIC_RELEASE); break;
            }
            s->done += 4096u; s->rel++; budget -= 4096u;
        }
        if (s->done >= s->len) __atomic_store_n(&s->state, 3u, __ATOMIC_RELEASE);
        return;   /* one slot per tick */
    }
}

/* Snapshot a slot for DUMLIST. Returns state; fills outs. */
uint32_t qud_dump_ufs_slot(uint32_t idx, uint64_t *pa, uint32_t *len,
                            uint32_t *done, const char **tag)
{
    if (idx >= QUD_DUMP_SLOTS) return 0xffu;
    struct qud_dump_slot *s = &qud_dump_slots[idx];
    uint32_t st = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
    if (pa) *pa = s->pa;
    if (len) *len = s->len;
    if (done) *done = s->done;
    if (tag) *tag = (const char *)s->tag;
    return st;
}

/* ---- READ / READNEXT ---------------------------------------------------- */

/* Starts a buffered read so the host can pull large ranges 128 bytes at a
 * time, mirroring the SHOTNEXT cursor the screenshot path uses. Returns 1 on
 * success, -2 when a page inside the range is unmapped, 0 on bad arguments. */
static int
qud_diag_read_begin(uint64_t va, uint32_t len, uint32_t frame, int phys)
{
    uint32_t crc = 0xffffffffu;
    uint8_t scratch[QUD_DIAG_CHUNK];
    uint32_t at = 0;

    if (!len || len > QUD_DIAG_MAX_LEN) {
        return 0;
    }
    qud_diag_cursor_clear();
    /* Walk the range once: this both proves every page is readable and folds
     * the CRC the host uses to verify the reassembled buffer. */
    while (at < len) {
        const uint32_t take = (len - at) < QUD_DIAG_CHUNK ? (len - at) : QUD_DIAG_CHUNK;
        const size_t got = phys ? qud_diag_read_phys(va + at, scratch, take)
                                : qud_diag_read(va + at, scratch, take);
        if (got != take) {
            return -2;
        }
        for (uint32_t i = 0; i < take; i++) {
            crc ^= scratch[i];
            for (unsigned bit = 0; bit < 8; bit++) {
                crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
            }
        }
        at += take;
    }
    qud_diag_cursor.va = va;
    qud_diag_cursor.length = len;
    qud_diag_cursor.offset = 0;
    qud_diag_cursor.frame = frame;
    qud_diag_cursor.crc = ~crc;
    qud_diag_cursor.active = 1;
    qud_diag_cursor.is_phys = phys ? 1u : 0u;
    return 1;
}

/* Hands out the next chunk. Returns 0 when the cursor is exhausted. */
static size_t
qud_diag_read_next(uint8_t *out, size_t capacity, uint32_t *offset, uint32_t *frame)
{
    uint32_t take;
    size_t got;

    if (!qud_diag_cursor.active || !capacity) {
        return 0;
    }
    if (qud_diag_cursor.offset >= qud_diag_cursor.length) {
        qud_diag_cursor.active = 0;
        return 0;
    }
    take = qud_diag_cursor.length - qud_diag_cursor.offset;
    if (take > capacity) {
        take = (uint32_t)capacity;
    }
    got = qud_diag_cursor.is_phys
        ? qud_diag_read_phys(qud_diag_cursor.va + qud_diag_cursor.offset, out, take)
        : qud_diag_read(qud_diag_cursor.va + qud_diag_cursor.offset, out, take);
    if (got != take) {
        qud_diag_cursor.active = 0;
        return 0;
    }
    if (offset) *offset = qud_diag_cursor.offset;
    if (frame) *frame = qud_diag_cursor.frame;
    qud_diag_cursor.offset += take;
    return got;
}

static int
qud_diag_cursor_active(void)
{
    return qud_diag_cursor.active;
}

static uint32_t
qud_diag_cursor_crc(void)
{
    return qud_diag_cursor.crc;
}

static uint32_t
qud_diag_cursor_length(void)
{
    return qud_diag_cursor.length;
}

#endif /* QCOM_USB_DIAG_C */

/* ==== NABU-PROBE-L808 BEGIN (temporary) ==== */
static int qud_diag_cursor_is_phys(void)
{
    return qud_diag_cursor.is_phys;
}
/* ==== NABU-PROBE-L808 END ==== */
