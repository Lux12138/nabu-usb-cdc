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
#define QUD_DIAG_CHUNK 128u
/* Largest range a single READ may name; the host walks it with READNEXT. */
#define QUD_DIAG_MAX_LEN (1u << 20)

struct qud_diag_cursor {
    uint64_t va;        /* base address of the pending read          */
    uint32_t length;    /* total bytes requested                     */
    uint32_t offset;    /* next byte to hand out                     */
    uint32_t frame;     /* command sequence that created the cursor  */
    uint32_t crc;       /* CRC32 over the whole range                */
    uint8_t  active;
    uint8_t  reserved[3];
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

/* ---- READ / READNEXT ---------------------------------------------------- */

/* Starts a buffered read so the host can pull large ranges 128 bytes at a
 * time, mirroring the SHOTNEXT cursor the screenshot path uses. Returns 1 on
 * success, -2 when a page inside the range is unmapped, 0 on bad arguments. */
static int
qud_diag_read_begin(uint64_t va, uint32_t len, uint32_t frame)
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
        const size_t got = qud_diag_read(va + at, scratch, take);
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
    got = qud_diag_read(qud_diag_cursor.va + qud_diag_cursor.offset, out, take);
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
