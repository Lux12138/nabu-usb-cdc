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

/* RAM bounds, defined in osfmk/arm64/arm_vm_init.c and exported so the linear
 * map can be bounds-checked before phystokv() -- that function panics on an
 * illegal PA ("illegal PA: 0x...; phys base 0x..., size ..."), which inside the
 * USB worker would take the machine down. Page-table pages always live in RAM,
 * so a bounds check turns a bad table pointer into an ERR reply instead. */
extern unsigned long gPhysBase;
extern unsigned long real_phys_size;

/* Declared here because QcomXnuUsbDebug.h is pulled in after this file; the
 * definition lives in QcomXnuUsbDebug.c and this is only used for the one
 * pre-access log line. */
size_t qcom_usb_debug_write(const void *bytes, size_t count);

/* The kernel's own copy of the TTBR1 base address.
 *
 * arm_vm_init.c:1962/2151/2162 is the whole story:
 *
 *     cpu_ttep = kvtophys((vm_offset_t)cpu_tte);          // PA of the L1 table
 *     set_mmu_ttb_alternate(cpu_ttep & TTBR_BADDR_MASK);  // -> TTBR1_EL1
 *     cpu_tte  = (tt_entry_t *)(phystokv(cpu_ttep));      // VA of that table
 *
 * so cpu_ttep *is* the walk root, and reading it is what XNU itself does
 * (pmap.c:7772 uses pmap->ttep, the same value).
 *
 * This variable exists because of a measured failure, not by preference: on
 * kb-42 `mrs TTBR1_EL1` produced a value that failed the walk's first check, so
 * every address -- including kernel text that READ reads happily -- came back
 * "unmapped". The variable was verified live before being trusted: `var
 * _cpu_ttep` = 0x00000000cae35000, and a manual walk started from that PA
 * (readpa 0xcae35600 -> L1 descriptor 0x20000000cae36003 -> L2 entry at
 * 0xcae361c0 = 0xcae3f003 -> L3 entry at 0xcae3fe48 = 0x0060000000c2fc9683 ->
 * page 0xc2fc9108, whose contents equal `var _gPhysBase`) reproduces the
 * translation byte for byte. So the table is right and the register read is
 * what was wrong.
 *
 * The mrs value is still taken and reported so a mismatch stays visible. */
extern uint64_t cpu_ttep;

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
 * internally rounds to pages.
 *
 * HAZARD, measured on kb-46 (2026-11-23): a PA that does not exist on the SoC
 * wedges the whole USB worker. `readpa 0x3c0fc000 32` (a Raspberry Pi address
 * that BCM2837.h defines, so it looked plausible) never returned: the CDC port
 * stayed open but HELLO stopped answering, and with nabu-wdt-off=1 the board
 * needed a power cycle.
 *
 * The reason is that ml_io_map() cannot be used as an existence test. Its
 * kmem_alloc() runs with KMA_NOFAIL (osfmk/arm/io_map.c:119-135), so it
 * essentially never returns 0; for a nonexistent PA it hands back a valid VA
 * whose translation points at nothing, and the memcpy below then takes a
 * synchronous external abort. mmu_kvtop() (the READ path) does not have this
 * problem because the hardware AT instruction reports a fault instead.
 *
 * => Only call this for a PA already known to exist: an MMIO window in
 *    qud_write_whitelist[], the XNU RAM range, or an address the caller has
 *    read successfully before. A READPA whitelist mirroring WRITEPA's is the
 *    intended hardening; until then the host side must not be fed guesses. */
/* One cached device window. The console reads a few register blocks repeatedly
 * (MDSS, GPUCC, SMMU); reusing one permanent mapping avoids leaking a VM entry
 * per command. Different windows evict the previous one -- the mapping stays
 * valid in the kernel (XNU never unmaps IO space), we just stop tracking it. */
static uint64_t phys_cache_base;
static uint64_t phys_cache_span;
static uintptr_t phys_cache_va;

static size_t
qud_diag_read_phys_anno(uint64_t pa, void *dst, size_t len, int anno)
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
    /* Announce host-supplied accesses before touching them. A read that wedges
     * the machine never returns, so this line is the only way to learn
     * afterwards which address did it -- kb-47 was diagnosed by guessing, at
     * one power cycle per attempt.
     *
     * Hand-rolled instead of printf: this runs from the USB worker and must not
     * take kprintf's lock (the module deliberately never enters kprintf; see
     * the re-entry notes in QcomXnuUsbDebug.c). PTDUMP's own reads pass
     * anno=0 -- those addresses come from the page tables and logging every
     * level would flood the console. */
    if (anno) {
        char text[40];
        size_t at = 0;
        static const char hexd[] = "0123456789abcdef";
        text[at++] = 'p'; text[at++] = 'a'; text[at++] = '-';
        text[at++] = 'r'; text[at++] = 'd'; text[at++] = ':';
        text[at++] = ' '; text[at++] = '0'; text[at++] = 'x';
        for (int shift = 60; shift >= 0; shift -= 4)
            text[at++] = hexd[(pa >> shift) & 0xfu];
        text[at++] = ' '; text[at++] = 'l'; text[at++] = '=';
        {
            char digits[8];
            unsigned n = 0, v = (unsigned)len;
            do { digits[n++] = (char)('0' + (v % 10u)); v /= 10u; } while (v && n < sizeof(digits));
            while (n) text[at++] = digits[--n];
        }
        text[at++] = '\n';
        (void)qcom_usb_debug_write(text, at);
    }
    memcpy(dst, (const void *)(va + off), len);
    __asm__ volatile("dsb sy" ::: "memory");
    /* XNU never calls ml_io_unmap: device mappings from ml_io_map are
       permanent (all XNU callers keep them). Unmapping panics vm_map_delete
       on a permanent entry. */
    return len;
}

/* Existing callers keep the two-argument spelling; only the two paths that
 * take an address from the host pass anno=1 (see the log note above). */
static size_t
qud_diag_read_phys(uint64_t pa, void *dst, size_t len)
{
    return qud_diag_read_phys_anno(pa, dst, len, 0);
}

/* ---- WRITEPA: MMIO-only physical write ---------------------------------- */
/* Whitelist of MMIO windows a WRITEPA may touch. DDR physical addresses are
 * deliberately excluded: a stray write to RAM would corrupt a running system
 * without any fault. Only the SoC register blocks the console needs are listed
 * here; anything else returns "denied". Ranges are [lo, hi) in bytes. */
struct qud_mmio_range { uint64_t lo, hi; };
static const struct qud_mmio_range qud_write_whitelist[] = {
    /* WDT. Added for the WRITEPA positive test, and justified by the driver
     * itself: nabu_wdt_arm()/nabu_wdt_pet() write +0x08 (EN) on every pet, so
     * that register is already known writable on this board from a path that
     * runs thousands of times per boot. Note the difference from SMMU: here a
     * WRITE is proven, there only configuration was; and a write to EN is not
     * what hangs the machine. */
    { UINT64_C(0x17c10000), UINT64_C(0x17c11000) }, /* WDT */
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

/* ---- READPA: which physical addresses may be read ------------------------
 *
 * History, because this went wrong twice in opposite directions on kb-47:
 *
 *   readpa 0x3c0fc000  -> wedged the worker for good (address does not exist;
 *                         ml_io_map's KMA_NOFAIL allocation returns a valid VA
 *                         whose pages map to nothing, so the memcpy never
 *                         returns)
 *   readpa 0x17c10008  -> refused, but that is the WDT EN register this port
 *                         has read successfully for many rounds
 *   RDBLK smmu         -> CRASHED the kernel, although SMMU is in the write
 *                         whitelist because writing it is explicitly supported
 *
 * The last one is the important lesson: "the address exists" and "reading it
 * right now is safe" are different statements. A static list cannot decide the
 * second one -- SMMU needs its clock/power domain up, and that is not knowable
 * from here. So this list is deliberately NOT the write list: it holds only
 * blocks that have actually been read safely on this board, and it is expected
 * to grow one entry at a time, each backed by a board round.
 *
 * Anything outside it is refused without touching ml_io_map, and every accepted
 * read is announced on the console log first (see qud_diag_read_phys) so that if
 * a read ever does wedge the machine, the last log line names the culprit. */
struct qud_read_range { uint64_t lo, hi; const char *why; };
static const struct qud_read_range qud_read_whitelist[] = {
    /* Register blocks this console has read on hardware, repeatedly. */
    { UINT64_C(0x17c10000), UINT64_C(0x17c11000), "wdt"    },
    { UINT64_C(0x0ae00000), UINT64_C(0x0af00000), "mdss"   },
    { UINT64_C(0x0ae01000), UINT64_C(0x0ae02000), "mdp"    },
    { UINT64_C(0x0ae6b000), UINT64_C(0x0ae6c000), "intf1"  },
    { UINT64_C(0x0ae6c000), UINT64_C(0x0ae6d000), "intf2"  },
    { UINT64_C(0x02c90000), UINT64_C(0x02ca0000), "gpucc"  },
    { UINT64_C(0x01050000), UINT64_C(0x01090000), "gcc"    },
    { UINT64_C(0x0af00000), UINT64_C(0x0af01000), "discc"  },
    { UINT64_C(0x0a600000), UINT64_C(0x0a700000), "dwc3"   },
    /* SMMU is NOT here on purpose. It is in the write whitelist because
     * configuring it is supported, but reading it crashed the kernel on kb-47
     * (its clock domain is not guaranteed up). Add it back only with a board
     * round that proves otherwise. */
};

static int
qud_read_pa_allowed(uint64_t pa, size_t len, const char **why)
{
    if (why) *why = "outside";
    if (!len) return 0;

    /* RAM first: the common case (page tables, kernel data). Checked at runtime
     * because gPhysBase moves with the physmap slide. */
    if (real_phys_size) {
        const uint64_t base = (uint64_t)gPhysBase;
        const uint64_t end = base + (uint64_t)real_phys_size;
        if (pa >= base && pa <= end && (uint64_t)len <= end - pa) {
            if (why) *why = "ram";
            return 1;
        }
    }
    for (unsigned i = 0; i < sizeof(qud_read_whitelist)/sizeof(qud_read_whitelist[0]); i++) {
        const struct qud_read_range *r = &qud_read_whitelist[i];
        if (pa >= r->lo && pa <= r->hi && (uint64_t)len <= r->hi - pa) {
            if (why) *why = r->why;
            return 1;
        }
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
    /* Announce before writing, for the same reason qud_diag_read_phys_anno()
     * announces reads: a write to a register whose clock domain is down can hang
     * the worker exactly as a read can (kb-46 and kb-47 each cost a power
     * cycle), and this log line is the only way to learn afterwards which
     * address did it. */
    {
        char text[44];
        size_t at = 0;
        static const char hexd[] = "0123456789abcdef";
        text[at++] = 'p'; text[at++] = 'a'; text[at++] = '-';
        text[at++] = 'w'; text[at++] = 'r'; text[at++] = ':';
        text[at++] = ' '; text[at++] = '0'; text[at++] = 'x';
        for (int shift = 60; shift >= 0; shift -= 4)
            text[at++] = hexd[(pa >> shift) & 0xfu];
        text[at++] = ' '; text[at++] = 'l'; text[at++] = '=';
        {
            char digits[8];
            unsigned n = 0, v = (unsigned)len;
            do { digits[n++] = (char)('0' + (v % 10u)); v /= 10u; } while (v && n < sizeof(digits));
            while (n) text[at++] = digits[--n];
        }
        text[at++] = '\n';
        (void)qcom_usb_debug_write(text, at);
    }
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
extern int native_ufs_log_open(uint64_t *first_lba, uint64_t *blocks, const char **why)
    __attribute__((weak_import));
extern int native_ufs_export_write(uint64_t rel, uint32_t blocks, const void *src)
    __attribute__((weak_import));
extern uint64_t native_ufs_export_blocks(void) __attribute__((weak_import));
extern uint64_t native_ufs_export_first(void) __attribute__((weak_import));
extern int native_ufs_log_sync(void) __attribute__((weak_import));

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
    if (&native_ufs_export_write == 0 || &native_ufs_export_blocks == 0 ||
        &native_ufs_log_open == 0) return 0;
    /* The export window is opened lazily by the log writer (APFS mount).
     * Try once here so a dump issued early in boot still works; retry on
     * the next command if the UFS driver is not ready yet. */
    if (native_ufs_export_blocks() == 0) {
        uint64_t first = 0, blocks = 0; const char *why = "";
        (void)native_ufs_log_open(&first, &blocks, &why);
    }
    return native_ufs_export_blocks() != 0;
}

/* Enqueue a dump. Returns slot index 0..N-1, or -1 full, -2 unsupported,
 * -3 bad args, -4 PA not readable.
 *
 * The source range goes through the same physical whitelist READPA uses: the
 * worker reads it with qud_diag_read_phys(), and a nonexistent PA there wedges
 * the USB worker permanently (see the hazard note above that function). A dump
 * of a mistyped address must be refused at enqueue time, not five ticks into
 * the job. */
int qud_dump_ufs_enqueue(uint64_t pa, uint32_t len, const uint8_t *tag, uint32_t taglen)
{
    if (!qud_dump_ufs_available()) return -2;
    if (!len || (len & 0xfffu) || native_ufs_export_blocks() < (len / 4096u)) return -3;
    if (!qud_read_pa_allowed(pa, len, NULL)) return -4;
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

/* Synchronous first block (T35-s3 Q3 criterion): write one 4 KiB block from
 * the slot's source PA before the reply goes out. Lets the host tell a broken
 * read path from a stalled worker in a single command.
 * Returns 1 on success (block landed, optionally synced), 0 on read failure,
 * -1 on export_write failure. *synced reports whether log_sync succeeded. */
int qud_dump_ufs_write_first(uint32_t slot_index, uint32_t *blocks_written,
                             int *write_err, int *synced)
{
    struct qud_dump_slot *s;
    size_t got;
    int ok;
    if (blocks_written) *blocks_written = 0;
    if (write_err) *write_err = 0;
    if (synced) *synced = 0;
    if (slot_index >= QUD_DUMP_SLOTS) return -1;
    s = &qud_dump_slots[slot_index];
    if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == 0 || !s->len) return -1;

    got = qud_diag_read_phys_anno(s->pa, qud_dump_scratch, 4096u, 1);
    if (got != 4096u) {
        if (write_err) *write_err = -1;   /* read path failed */
        __atomic_store_n(&s->state, 4u, __ATOMIC_RELEASE);
        return 0;
    }
    ok = native_ufs_export_write(s->rel, 1u, qud_dump_scratch);
    if (!ok) {
        if (write_err) *write_err = -2;   /* export_write failed */
        __atomic_store_n(&s->state, 4u, __ATOMIC_RELEASE);
        return -1;
    }
    s->done = 4096u;
    s->rel = 1u;
    if (blocks_written) *blocks_written = 1u;
    /* Land it: without a sync (or FUA) a PSCI reset can leave NAND stale. */
    if (&native_ufs_log_sync != 0) {
        if (native_ufs_log_sync()) {
            if (synced) *synced = 1;
        } else {
            if (write_err) *write_err = -3;   /* sync failed */
        }
    }
    __atomic_store_n(&s->state, 2u, __ATOMIC_RELEASE);  /* running: worker finishes the rest */
    return 1;
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
            size_t got = qud_diag_read_phys_anno(s->pa + s->done, qud_dump_scratch, 4096u, 1);
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
        /* phys=1 means the address came from the host (READPA / RDBLK), so
         * announce it: if this read wedges the worker, the log names it. */
        const size_t got = phys ? qud_diag_read_phys_anno(va + at, scratch, take, 1)
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
        ? qud_diag_read_phys_anno(qud_diag_cursor.va + qud_diag_cursor.offset, out, take, 1)
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

/* ---- PTDUMP: stage-1 page-table walk ------------------------------------
 *
 * Everything below is derived from registers and from RAM-resident tables; no
 * page-table geometry is hardcoded. The two inputs are TTBR1_EL1 (kernel VA
 * space) and TCR_EL1 (T1SZ = VA width, TG1 = granule), both read with mrs.
 *
 * Why TTBR1 and not TTBR0: kernel virtual addresses live in the high half, and
 * XNU programs the kernel's root table into TTBR1_EL1 (osfmk/arm/pmap/pmap.c:
 *7772 uses set_mmu_ttb() for user pmaps, which writes TTBR0_EL1; the comment at
 * :738 states the kernel pmap's root page table "is always the same as TTBR1".
 * Measured on kb-40: get_mmu_ttb() reads TTBR0_EL1, so a walker built on it
 * would descend the *user* table and report unmapped for every 0xfffffff0…
 * address.
 *
 * Why it is safe from the USB worker: it takes no pmap lock and never calls
 * pmap_vtophys(). It reads raw 8-byte entries out of RAM through phystokv(),
 * which is the same technique pmap_find_pa_nofault() uses for kernel_pmap
 * (:6701, via mmu_kvtop) and KDP relies on. A bad table pointer cannot fault:
 * every PA is bounds-checked against [gPhysBase, gPhysBase+real_phys_size)
 * first, because phystokv() panics on an out-of-range PA.
 *
 * Known limitation, deliberate: without the pmap lock a concurrent unmap can
 * make the walk see a half-updated hierarchy. The worst case is a stale or
 * "unmapped" answer, never a crash, so the verb stays safe to call at any time.
 *
 * Geometry measured out of the kb-40 kernel image (pmap_table_level_info_4k at
 * 0xfffffff00700ae00, pmap_pt_attr_4k at 0xfffffff0071c7af0) and cross-checked
 * against TCR_EL1 at runtime:
 *   pta_root_level = 1   (T0SZ_BOOT 26 -> (26-16)/9 = 1: the L0 table is skipped)
 *   pta_max_level  = 3   pta_page_shift = 12   L1 index_mask = 0x3fc0000000
 * The truncated L1 index (8 bits, not 9) is __ARM64_PMAP_SUBPAGE_L1__ + 4K:
 * one TTBR covers 2^38 = 256GB, which is exactly
 * VM_MIN_KERNEL_ADDRESS = 0xffffffe000000000. Deriving the root index width
 * from T1SZ reproduces that mask, so the truncation needs no special case. */

struct qud_pt_geom {
    unsigned shift[4];      /* bit position of each level's index */
    unsigned width[4];      /* index bits at each level */
    unsigned root;          /* first level present in the hierarchy */
    unsigned last;          /* leaf level (3 for both granules here) */
    unsigned page_shift;    /* 12 for 4K, 14 for 16K */
    unsigned t1sz;          /* raw TCR_EL1 T1SZ, for reporting */
    unsigned tg1;           /* raw TCR_EL1 TG1, for reporting */
};

/* Fill geom from TCR_EL1 using the same formulas pmap.c uses to build its own
 * tables (pmap_pt_attr_4k/16k: .pta_root_level = (T0SZ - 16) / per_level).
 *
 * Verified against the kb-40 kernel image: with T0SZ_BOOT = 26 the formula
 * yields root = 1, and the resulting masks (L1 = bits 30..37, L2 = 21..29,
 * L3 = 12..20) are byte-identical to the index_mask fields read out of
 * pmap_table_level_info_4k at 0xfffffff00700ae00:
 *   L1 0x0000003fc0000000   L2 0x000000003fe00000   L3 0x00000000001ff000
 * The truncated 8-bit L1 index is __ARM64_PMAP_SUBPAGE_L1__ with a 4K granule
 * (one TTBR covers 2^38 = 256GB, exactly VM_MIN_KERNEL_ADDRESS). Deriving it
 * from T1SZ reproduces that without a special case.
 *
 * Returns 1 on success, 0 for a granule this walker does not model. */
static int
qud_pt_geometry(struct qud_pt_geom *g)
{
    uint64_t tcr = 0;
    unsigned t1sz, va_bits, tg1, n;

    __asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr) : : "memory");
    t1sz = (unsigned)((tcr >> 16) & 0x3fu);     /* TCR_T1SZ_SHIFT == 16 */
    tg1 = (unsigned)((tcr >> 30) & 0x3u);       /* TCR_TG1_GRANULE_SHIFT == 30 */
    if (t1sz < 16u || t1sz > 40u) return 0;
    g->t1sz = t1sz;
    g->tg1 = tg1;

    /* TG1 encoding -- READ THIS BEFORE "FIXING" IT.
     *
     * XNU does NOT use the architectural AArch64 values here. osfmk/arm64/
     * proc_reg.h:850-856 defines, for the TTBR1 region:
     *
     *     TCR_TG1_GRANULE_16KB = 1 << 30   (field value 1)
     *     TCR_TG1_GRANULE_4KB  = 2 << 30   (field value 2)
     *     TCR_TG1_GRANULE_64KB = 3 << 30   (field value 3)
     *
     * which is the *inverted* mapping relative to the ARM ARM's 00=4K, 01=16K,
     * 10=64K. This cost a full board round on kb-44: the register reports TG1=2,
     * so the architectural reading says 4K, and an earlier revision of this
     * function decoded it as 16K. Every downstream number then went wrong in a
     * way that looked like a page-table problem: page_shift became 14, the root
     * mask cleared 14 bits (0xcae35000 -> 0xcae34000), shift[] became
     * {47,36,25,14}, the L1 index came out as 3 instead of 192, and the walk
     * read an empty entry and reported stage=5.
     *
     * Decoded per XNU's own table: TG1==2 -> 4KB granule. */
    if (tg1 == 2u) {            /* TCR_TG1_GRANULE_4KB */
        g->page_shift = 12u;
        g->shift[0] = 39u; g->shift[1] = 30u; g->shift[2] = 21u; g->shift[3] = 12u;
    } else if (tg1 == 1u) {     /* TCR_TG1_GRANULE_16KB */
        g->page_shift = 14u;
        g->shift[0] = 47u; g->shift[1] = 36u; g->shift[2] = 25u; g->shift[3] = 14u;
    } else {
        return 0;               /* 64KB (3) or reserved (0): not modelled */
    }

    g->last = 3u;

    /* Root level: the lowest level whose table is still smaller than the VA
     * width, i.e. the first level where shift[n] < va_bits. This reproduces
     * both of XNU's attrs without a per-granule special case:
     *
     *   4K,  T1SZ 26 -> va_bits 38: shift[0]=39 is NOT < 38, shift[1]=30 is
     *                   -> root = 1, width = 38-30 = 8 bits  (0x3fc0000000)
     *   16K, T1SZ 25 -> va_bits 39: shift[0]=47 is NOT < 39, shift[1]=36 is
     *                   -> root = 1, width = 39-36 = 3 bits  (0x7000000000)
     *
     * The naive (T1SZ-16)/per_level formula happens to give 1 for 4K but 0 for
     * 16K, which would send the walk through an L0 table the kernel never
     * builds -- checked against pmap_pt_attr_16k, which hardcodes
     * PMAP_TT_L1_LEVEL. The shift comparison is the rule that actually holds. */
    va_bits = 64u - t1sz;
    g->root = g->last;
    for (n = 0; n <= g->last; n++) {
        if ((unsigned)g->shift[n] < va_bits) { g->root = n; break; }
    }
    if ((unsigned)g->shift[g->root] >= va_bits) return 0;

    for (n = 0; n <= g->last; n++) {
        if (n < g->root) {
            g->width[n] = 0;
        } else if (n == g->root) {
            g->width[n] = va_bits - (unsigned)g->shift[n];
        } else {
            g->width[n] = (unsigned)(g->shift[n - 1] - g->shift[n]);
        }
        if (g->width[n] > 16u) return 0;
    }
    return 1;
}

struct qud_pt_result {
    uint64_t entry[4];      /* raw descriptor at each level visited */
    unsigned level;         /* level at which the walk stopped      */
    uint64_t pa;            /* translation of the queried VA        */
    uint64_t root;          /* PA the walk actually started from    */
    uint64_t ttbr1_mrs;     /* what mrs TTBR1_EL1 said, for reporting */
    uint64_t last_entry_pa; /* PA of the last entry read            */
    unsigned t1sz;          /* copy of geom, for the reply           */
    unsigned tg1;
    unsigned page_shift;
    uint8_t valid;
    uint8_t block;          /* leaf was a block descriptor, not L3  */
    uint8_t source;         /* 1 = cpu_ttep variable, 2 = mrs fallback */
    uint8_t stage;          /* where a failure happened:
                             * 0 none, 1 geometry, 2 root empty,
                             * 3 bad width, 4 entry read failed,
                             * 5 entry invalid, 6 leaf not valid */
};

/* Read one 8-byte table entry from a RAM physical address.
 *
 * Uses qud_diag_read_phys() -- the ml_io_map window -- rather than
 * phystokv(). Reason, measured on kb-42/kb-43: the same table page is readable
 * through the READPA verb (readpa 0xcae35600 returned the L1 descriptor
 * 0x20000000cae36003, and the whole manual walk produced gPhysBase's value),
 * so the data is reachable; but a walk built on phystokv() answered "unmapped"
 * for every address including kernel text. phystokv() derives a V=P address
 * from gVirtBase while the kernel's linear window is actually built from the
 * slid physmap_base, so for a page that sits only in the slid window the
 * derived address is not the mapped one. ml_io_map() asks the pmap for a fresh
 * mapping instead of assuming an offset, and it is the path READPA already
 * exercises on every round.
 *
 * The bounds check stays: ml_io_map() on an absurd PA is still a bad idea, and
 * a table pointer is only ever inside RAM. */
static int
qud_pt_read_entry(uint64_t table_pa, unsigned index, uint64_t *out)
{
    uint64_t pa;

    if (!real_phys_size) return 0;
    pa = table_pa + (uint64_t)index * 8u;
    if (pa < (uint64_t)gPhysBase) return 0;
    if ((pa - (uint64_t)gPhysBase) + 8u > (uint64_t)real_phys_size) return 0;
    if (qud_diag_read_phys(pa, out, 8) != 8) return 0;
    return 1;
}

/* Walk the stage-1 kernel hierarchy for va.
 * Returns 1 for a translation (r->valid=1, r->pa filled), 0 when the VA is
 * unmapped, -1 for a geometry this walker does not model. */
static int
qud_pt_walk(uint64_t va, struct qud_pt_result *r)
{
    struct qud_pt_geom g;
    uint64_t table_pa, entry = 0;
    unsigned level;

    if (!r) return 0;
    if (!qud_pt_geometry(&g)) { r->stage = 1; return -1; }

    /* Root table: prefer the kernel's own cpu_ttep (arm_vm_init.c:1962), fall
     * back to `mrs TTBR1_EL1` only when the variable is unset. Measured on
     * kb-42: cpu_ttep = 0x00000000cae35000 walked correctly by hand, while the
     * register read failed the first check for every address including kernel
     * text. Both values are reported so a disagreement is never silent. */
    __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(r->ttbr1_mrs) : : "memory");
    if (cpu_ttep) {
        table_pa = cpu_ttep;
        r->source = 1;
    } else {
        table_pa = r->ttbr1_mrs;
        r->source = 2;
    }
    table_pa &= UINT64_C(0x0000ffffffffffff);          /* TTBR_BADDR_MASK */
    table_pa &= ~((UINT64_C(1) << g.page_shift) - 1u);
    r->root = table_pa;
    r->t1sz = g.t1sz;
    r->tg1 = g.tg1;
    r->page_shift = g.page_shift;
    if (!table_pa) { r->stage = 2; return 0; }         /* TTBR1 disabled */

    for (level = g.root; level <= g.last; level++) {
        const unsigned width = g.width[level];
        const unsigned shift = g.shift[level];
        unsigned index;

        if (!width) { r->stage = 3; return 0; }
        index = (unsigned)((va >> shift) & ((UINT64_C(1) << width) - 1u));
        r->last_entry_pa = table_pa + (uint64_t)index * 8u;
        if (!qud_pt_read_entry(table_pa, index, &entry)) { r->stage = 4; return 0; }
        r->entry[level] = entry;
        r->level = level;

        if (level == g.last) {
            /* L3 leaf: ARM_PTE_TYPE_VALID == 3, both low bits set. */
            if ((entry & 3u) != 3u) { r->stage = 6; return 0; }
            r->valid = 1;
            r->pa = (entry & UINT64_C(0x0000fffffffff000)) |
                    (va & ((UINT64_C(1) << g.page_shift) - 1u));
            return 1;
        }
        if (!(entry & 1u)) { r->stage = 5; return 0; } /* ARM_TTE_VALID   */
        if (entry & 2u) {                             /* table: descend  */
            table_pa = entry & UINT64_C(0x0000fffffffff000);
            continue;
        }
        /* Block descriptor: a leaf one level early -- 1GB at L1 and 2MB at L2
         * for 4K, 64GB at L1 and 32MB at L2 for 16K. The PA comes from the
         * entry's bits above this level's shift, the offset from the VA. */
        {
            const uint64_t block_mask = ~((UINT64_C(1) << shift) - 1u);
            r->valid = 1;
            r->block = 1;
            r->pa = (entry & block_mask & UINT64_C(0x0000fffffffff000)) |
                    (va & ~block_mask);
            return 1;
        }
    }
    return 0;
}

/* ---- PANICDUMP: the embedded panic region -------------------------------
 *
 * Layout comes from osfmk/kern/debug.h:356-383 (struct embedded_panic_header),
 * whose offsets are fixed by __attribute__((packed)) -- verified by compiling
 * the struct and printing offsetof():
 *
 *     magic       +0x00   u32   EMBEDDED_PANIC_MAGIC ('FUNK')
 *     crc         +0x04   u32
 *     version     +0x08   u32
 *     flags       +0x0c   u64   (eph_panic_flags_t is uint64_t, debug.h:324)
 *     log_offset  +0x14   u32   from the start of the header
 *     log_len     +0x18   u32
 *     ss_offset   +0x1c   u32
 *     ss_len      +0x20   u32
 *     other_off   +0x24   u32
 *     other_len   +0x28   u32
 *     os_version  +0x34   32 bytes, NUL padded
 *     sizeof      = 169
 *
 * gPanicBase is a *virtual* address: pe_init.c:112/159 assigns it from
 * ml_io_map_wcomb(PLATFORM_PANIC_LOG_PADDR, PLATFORM_PANIC_LOG_SIZE), and
 * pe_init.c:4-5 defaults those to 0x9b000000 / 0x80000 before the device-tree
 * path can override them. So the region is read through qud_diag_read() (the
 * hardware-AT path), never through a PA derived by arithmetic -- the exact
 * mistake that cost kb-42/43 on PTDUMP.
 *
 * The magic test is the point of "info" mode: anything other than 'FUNK' means
 * there is no panic to dump, and reporting which magic was found distinguishes
 * "iBoot already cleared the region" from "we read the wrong place". */

struct qud_panic_summary {
    uint64_t base;          /* gPanicBase, a kernel VA */
    uint32_t size;          /* gPanicSize */
    uint32_t magic;
    uint32_t version;
    uint64_t flags;
    uint32_t log_offset;
    uint32_t log_len;
    uint32_t ss_offset;
    uint32_t ss_len;
    uint32_t other_offset;
    uint32_t other_len;
    char     os_version[33];
};

extern vm_offset_t gPanicBase;
extern unsigned int gPanicSize;

static uint32_t
qud_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Read and validate the panic header.
 * 1 = valid panic ('FUNK' + sane bounds), 0 = magic absent,
 * -1 = header unreadable / region not mapped, -2 = magic ok but bounds absurd. */
static int
qud_panic_header(struct qud_panic_summary *out)
{
    uint8_t raw[0x60];
    size_t got;

    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->base = (uint64_t)gPanicBase;
    out->size = (uint32_t)gPanicSize;
    if (!gPanicBase || !gPanicSize) return -1;

    got = qud_diag_read((uint64_t)gPanicBase, raw, sizeof(raw));
    if (got < 0x34) return -1;

    out->magic = qud_le32(raw + 0x00);
    out->version = qud_le32(raw + 0x08);
    for (unsigned i = 0; i < 8u; i++)
        out->flags |= (uint64_t)raw[0x0c + i] << (8u * i);
    out->log_offset = qud_le32(raw + 0x14);
    out->log_len = qud_le32(raw + 0x18);
    out->ss_offset = qud_le32(raw + 0x1c);
    out->ss_len = qud_le32(raw + 0x20);
    out->other_offset = qud_le32(raw + 0x24);
    out->other_len = qud_le32(raw + 0x28);

    /* eph_os_version at +0x34: 32 bytes, NUL padded. Copy at most 32 and
     * terminate unconditionally -- a region of garbage must not produce a
     * non-terminated string in the reply. */
    for (unsigned i = 0; i < 32u && (0x34u + i) < got; i++)
        out->os_version[i] = (char)raw[0x34u + i];
    out->os_version[32] = 0;

    if (out->magic != UINT32_C(0x46554e4b))      /* 'FUNK' */
        return 0;
    /* Bounds must stay inside the region; a stale header can carry absurd
     * lengths and a later chunk read would walk off the mapping. */
    if (out->log_len && (out->log_offset > gPanicSize ||
                         out->log_len > gPanicSize - out->log_offset))
        return -2;
    if (out->ss_len && (out->ss_offset > gPanicSize ||
                        out->ss_len > gPanicSize - out->ss_offset))
        return -2;
    if (out->other_len && (out->other_offset > gPanicSize ||
                           out->other_len > gPanicSize - out->other_offset))
        return -2;
    return 1;
}

#endif /* QCOM_USB_DIAG_C */

/* ==== NABU-PROBE-L808 BEGIN (temporary) ==== */
static int qud_diag_cursor_is_phys(void)
{
    return qud_diag_cursor.is_phys;
}
/* ==== NABU-PROBE-L808 END ==== */
