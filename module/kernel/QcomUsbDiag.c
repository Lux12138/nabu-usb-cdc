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
    /* nabu 2026-11-25 (§43): volatile element-wise copy, not memcpy.
     *
     * memcpy on device memory has no volatile semantics: the compiler is free
     * to vectorise it (ldp/ldnp/ldr q), split it, or hoist it above the dsb, and
     * GPU registers require strictly 32-bit accesses -- a wide load reads the
     * neighbouring register and splices a value that was never in the hardware.
     * Measured: `readpa 0x02c000e0 4` reported 0xc2e3d013 while the driver's own
     * mapping returned the correct 0x13d0e3c2 for the same physical address, and
     * four rounds of kernel-side reasoning were built on that bogus value.
     *
     * Copy by the widest naturally aligned type that does not straddle the
     * request, reading each unit through a volatile pointer so the compiler
     * cannot merge or reorder them. `len` is at most QUD_DIAG_CHUNK and the
     * whitelist keeps ranges inside one device window, so this stays cheap.
     *
     * ⚠️ BOTH pointers must be alignment-checked. The first version only checked
     * `src` and then stored through `*(uint32_t *)out`; `dst` is a byte array on
     * the caller's stack (QcomUsbDiag.c:694 `uint8_t scratch[QUD_DIAG_CHUNK]`),
     * which has no alignment guarantee, so an unaligned 32-bit store took an
     * alignment fault on arm64 and wedged the USB worker mid-boot (kdpr12).
     * Fall back to byte access unless both sides are aligned. */
    {
        uintptr_t src = va + off;
        uintptr_t dsta = (uintptr_t)dst;
        size_t left = len;
        __asm__ volatile("dsb sy" ::: "memory");
        while (left) {
            if (!(src & 3u) && !(dsta & 3u) && left >= 4u) {
                *(volatile uint32_t *)dsta = *(volatile uint32_t *)src;
                src += 4u; dsta += 4u; left -= 4u;
            } else {
                *(volatile uint8_t *)dsta = *(volatile uint8_t *)src;
                src += 1u; dsta += 1u; left -= 1u;
            }
        }
    }
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
    /* SMMU deliberately absent, on the same evidence that removed it from the
     * read list: `writepa 0x01000000 00000000` wedged the USB worker on kb-49
     * exactly as `RDBLK smmu` did on kb-47. The pa-wr line named it directly:
     *
     *     pa-wr: 0x0000000001000000 l=4     <- last action before the silence
     *
     * Writing SMMU registers is a supported operation in the driver, but doing
     * it through this console is not the same thing: the clock/power domain has
     * to be up, and that cannot be guaranteed from here. Restore only with a
     * board round that proves the domain is up at the moment of the write. */
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
    /* The GPU core window. The driver accesses exactly this range through
     * ml_io_map (A640State::read -> maps_[NativeAdrenoA6xxGpu]
     * -> getVirtualAddress() + offset), so a read here is the same operation
     * the driver performs on every fence and every counter sample -- not a new
     * kind of access.
     *
     * Why it is needed: the GPU's RBBM interrupt status and mask are the only
     * way to answer "is the CP fence interrupt enabled at all". Every other
     * route is closed. KDP's readmem cannot reach it (the window maps into the
     * 0xfffffff8... IO region, which has no pmap backing for copyin, and
     * `memory read` there returns "kdp read memory failed"); the 19.2 MHz job
     * rows carry GPU-clock ticks but not the IRQ state; and a probe built into
     * the driver would need exactly the rebuild this console is meant to
     * avoid.
     *
     * The window is 256 KiB (windows_[0].bytes = 262144, verified against
     * windows_[0].physical = 0x02c00000 on the board). Reading only. */
    { UINT64_C(0x02c00000), UINT64_C(0x02c40000), "gpu"    },
    { UINT64_C(0x01050000), UINT64_C(0x01090000), "gcc"    },
    { UINT64_C(0x0af00000), UINT64_C(0x0af01000), "discc"  },
    { UINT64_C(0x0a600000), UINT64_C(0x0a700000), "dwc3"   },
    /* 0x9b000000 is deliberately NOT here. kb-46 measured `readpa 0x9b000000`
     * as returning zeros and it was whitelisted on that basis, but two later
     * board rounds showed the address has no usable backing at all:
     *
     *   kb-51: mapping it before Mach VM (PE_create_console) builds a block
     *          descriptor and the machine panics in machine_idle.
     *   kb-52: mapping it after (STARTUP(KMEM)) succeeds, then bzero()ing
     *          512 KiB wedges the USB worker.
     *
     * "Reads as zero" turned out to mean "the transaction completes without
     * fault", not "there is memory there". Whitelisting an address because a
     * read survived is not evidence it is safe to touch.
     *
     * Panic text now goes to the 4 MiB BSS ring in osfmk/console/serial_general.c
     * and lands on UFS via nabu_log_land_now(); PANICDUMP has nothing to inspect
     * on this board and correctly reports nobase. */
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
/* READUFS: read the diagnostic log window back off the medium. Separate export so a
 * kernel built before it still links (the verb answers unsupported). */
extern int native_ufs_log_read(uint64_t rel, uint32_t blocks, void *dst)
    __attribute__((weak_import));

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

/* One-block read of the diagnostic log window. Returns 1 on success. Kept
 * synchronous on purpose: the write side already runs one 4 KiB block inside the
 * command handler (DUMPUFS blocks_written=1), so a single read block is the same
 * order of work. Bounds are validated in the driver as well. */
int qud_log_ufs_read_block(uint64_t rel, void *dst, int *why)
{
    if (why) *why = 0;
    if (&native_ufs_log_read == 0) { if (why) *why = 1; return 0; }
    if (!qud_dump_ufs_available()) { if (why) *why = 2; return 0; }
    if (native_ufs_log_read(rel, 1u, dst) != 1) { if (why) *why = 3; return 0; }
    return 1;
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

/* ---- SYSCTL -------------------------------------------------------------
 *
 * sysctlbyname() is exported (nm type S), so the console can ask the kernel the
 * same questions a user-space tool would. Read only: a stray sysctl write can
 * change scheduler or VM behaviour mid-boot, so the write side is not exposed.
 *
 * oldlenp is in/out -- pass the buffer size in, get the real length back -- and
 * the kernel answers ENOMEM (12) with the required size filled in when the
 * buffer is too small, which is what makes the two-step probe below work. */
extern int sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
                        void *newp, size_t newlen) __attribute__((weak_import));

/* Read a sysctl into out.
 *   >= 0  bytes produced
 *   -1    sysctlbyname not exported in this kernel
 *   -2    name not found or other error
 *   -3    the value does not fit in cap (the real size is written to *needed) */
int
qud_sysctl_read(const char *name, void *out, size_t cap, size_t *needed)
{
    size_t len = cap;
    int rc;

    if (needed) *needed = 0;
    if (!name || !*name || !out || !cap) return -2;
    if (&sysctlbyname == 0) return -1;

    rc = sysctlbyname(name, out, &len, NULL, 0);
    if (needed) *needed = len;
    if (rc == 0) return (int)len;
    if (len > cap) return -3;
    return -2;
}

/* ---- TASKDUMP / THREADDUMP ---------------------------------------------
 *
 * Design note, because this is the one place where the obvious approach was
 * deliberately rejected.
 *
 * The obvious approach is to walk the exported `tasks` queue and decode each
 * struct task. It does not work reliably here:
 *
 *   - allproc is not exported (nm type s), so the process side is out.
 *   - `tasks` IS exported, but its chain entries are `&task->tasks`, so
 *     recovering the struct base needs offsetof(struct task, tasks) -- and
 *     struct task is full of #if DEVELOPMENT / MACH_ASSERT /
 *     CONFIG_SCHED_MULTIQ fields, so that offset cannot be mirrored from the
 *     headers without reproducing the whole build configuration.
 *   - _proc_task() does not read a fixed p_task offset either: it computes
 *     global_base + 0x1a8 + proc_off, i.e. the field lives behind a per-zone
 *     layout. Hardcoding anything from that would be a guess.
 *
 * A wrong offset here does not fail cleanly -- it reports plausible garbage, and
 * in the worst case the walk dereferences a wild pointer. Three bricks already
 * taught what guessing costs.
 *
 * So these verbs report what is true without decoding: the exported anchors, the
 * raw queue words, and whether a caller-supplied pointer is mapped. A caller who
 * wants a field can PTDUMP the address, confirm the translation, then READ the
 * offset it cares about -- with the walk itself staying inside the validated
 * read path. Nothing here dereferences a pointer it has not bounds-checked. */

/* `tasks` is a queue_head_t ({next, prev}); the type is not in this module's
 * header set, so it is declared through its address instead -- the two words are
 * read explicitly below. */
extern char tasks;                   /* osfmk/kern/task.c, exported (nm type S) */
extern void *kernproc;               /* bsd/kern/kern_proc.c, exported as _kernproc */

/* Is this a kernel pointer safe to dereference? Only alignment and "the page
 * translates" are checked -- that is enough to stop a garbage word from being
 * followed, and it costs nothing on the happy path. */
static int
qud_kptr_ok(uint64_t p)
{
    if (!p) return 0;
    if (p & 3u) return 0;                          /* pointers are at least 4-aligned */
    return mmu_kvtop(p & ~UINT64_C(0xfff)) != 0;
}

/* The queue head's two words. Returns 1 when the head itself is readable. */
int
qud_task_qhead(uint64_t *next, uint64_t *prev)
{
    const uint64_t head = (uint64_t)(uintptr_t)&tasks;
    uint64_t words[2] = { 0, 0 };

    if (!qud_kptr_ok(head)) return 0;
    if (qud_diag_read(head, words, sizeof(words)) != sizeof(words)) return 0;
    if (next) *next = words[0];
    if (prev) *prev = words[1];
    return 1;
}

/* ---- kdebug control structure (T50: read without the sysctl lock) ----------
 *
 * bsd/kern/kdebug_common.c:28 declares `extern struct kd_control kd_control_trace;`
 * and it IS exported (D _kd_control_trace), so the field can be read directly
 * instead of through sysctl -- which fails from this context because kern.kdbg.*
 * carries CTLFLAG_LOCKED and sysctlbyname cannot take that lock here (measured:
 * kb-68 returned state=notfound for both kern.kdbg.oldest_time and kern.kdbg.debug
 * while hw.ncpu worked).
 *
 * The struct below mirrors the HEAD of bsd/sys/kdebug_common.h:110 up to
 * kdc_oldest_time only. Everything after it is omitted on purpose: we never touch
 * those fields, and mirroring less means less to keep in sync. The leading members
 * are here solely so the compiler computes the offset -- hardcoding "+48" would
 * break silently if the layout ever changed.
 *
 * Locking: kdebug_storage_lock() is a lck_spin with interrupts disabled
 * (kdebug_common.c:31-36), NOT a mutex, so taking it briefly from this context
 * cannot sleep and cannot wedge the CDC the way ktrace_lock would. kdc_oldest_time
 * is also a naturally aligned uint64_t, so even an unlocked read is a single
 * instruction; we still note both options in the reply. */
/* Offsets below are not hand-derived: they were read out of the LINKED kernel's
 * own code. `llvm-objdump -d --disassemble-symbols=_qud_kdebug_state` against
 * kb-69 shows exactly what the compiler emitted against kd_control_trace, so
 * these numbers are the compiler's, not ours:
 *
 *   adr  x8, _kd_control_trace
 *   ldr  x9, [x8, #0x30]   -> kdc_oldest_time
 *   ldr  w9, [x8, #0x08]   -> enabled:1 / mode:3
 *   ldr  w9, [x8, #0x2c]   -> kdc_live_flags
 *   ldr  w9, [x8, #0x1c]   -> kdebug_cpus
 *   ldr  w9, [x8, #0x24]   -> kdc_flags
 *
 * A mirrored struct was the first attempt and it was wrong: union kds_ptr is a
 * 21+11 bitfield over uint32_t (kdebug_common.h:65-71), not a pointer, and
 * declaring it as uint64_t shifted oldest_time to +0x38 while the real field is
 * at +0x30. Hard offsets with the disassembly as the source of truth cannot drift
 * silently in the same way -- if the layout changes, this verb reads the wrong
 * u32 and the caller sees an implausible value, which is a loud failure. */
#define QUD_KDC_ENABLED_MODE  0x08u
#define QUD_KDC_CPUS          0x1cu
#define QUD_KDC_FLAGS         0x24u
#define QUD_KDC_LIVE_FLAGS    0x2cu
#define QUD_KDC_OLDEST_TIME   0x30u

extern char kd_control_trace;   /* D _kd_control_trace, opaque on purpose */

/* 1 on success. Reports the oldest timestamp still in the trace buffer, plus the
 * header fields that say whether tracing is on and how the buffer is behaving.
 * Every access is a single naturally-aligned load: no lock, nothing to sleep on. */
int
qud_kdebug_state(uint64_t *oldest_time, uint32_t *enabled, uint32_t *mode,
                 uint32_t *live_flags, uint32_t *cpus, uint32_t *flags)
{
    const uint8_t *base = (const uint8_t *)(const void *)&kd_control_trace;
    uint32_t em;

    if (!base) return 0;
    em = *(const uint32_t *)(const void *)(base + QUD_KDC_ENABLED_MODE);
    if (oldest_time) *oldest_time = *(const uint64_t *)(const void *)(base + QUD_KDC_OLDEST_TIME);
    if (enabled) *enabled = em & 1u;
    if (mode) *mode = (em >> 1) & 7u;
    if (live_flags) *live_flags = *(const uint32_t *)(const void *)(base + QUD_KDC_LIVE_FLAGS);
    if (cpus) *cpus = *(const uint32_t *)(const void *)(base + QUD_KDC_CPUS);
    if (flags) *flags = *(const uint32_t *)(const void *)(base + QUD_KDC_FLAGS);
    return 1;
}

/* ---- struct proc/task decoding (T47-verified offsets) -----------------------
 *
 * Every offset below was verified against this tree AND against the running
 * kernel (kb-64):
 *   _proc_struct_size @ 0xfffffff007b051a8 == 0x790 (1936)      <- self-check gate
 *   kernproc -> p_pid  @ +0x60  == 0        (kernel_task)       <- matches
 *   kernproc -> p_comm @ +0x5bc == "kernel_task"                 <- matches
 *   tasks      @ 0xfffffff00c11d6d0         (exported queue head)
 *
 * T47's corrections, all adopted here:
 *   - struct task has NO name/pid/proc field; those live on the proc side.
 *   - _proc_task() is just proc + _proc_struct_size, not a per-zone walk.
 *   - arm64 thread has NO ->task; it is thread->t_tro(+0x438) -> *(u64*)(tro+0x28).
 *   - the task list is `tasks` (exported) with the node at +0x58; `allproc` is
 *     local so it cannot be imported -- kernproc is the usable anchor.
 *
 * The size gate matters: if _proc_struct_size ever disagrees with the compiled-in
 * offsets, the offsets are stale and decoding would emit plausible garbage. We
 * refuse instead. */

#define QUD_TASK_MAP            0x50u
#define QUD_TASK_TASKS_NODE     0x58u
#define QUD_TASK_THREADS        0x80u
#define QUD_TASK_THREAD_COUNT   0xa8u
#define QUD_TASK_PRIORITY       0xbcu
#define QUD_PROC_PID            0x60u
#define QUD_PROC_COMM           0x5bcu
#define QUD_PROC_COMM_LEN       17u
#define QUD_EXPECT_PROC_SIZE    0x790u

extern uint32_t proc_struct_size;      /* D _proc_struct_size, 32-bit */

/* 1 if the running kernel's proc size matches the offsets compiled in above. */
int
qud_task_layout_ok(uint32_t *actual)
{
    uint32_t got = 0;

    if (qud_diag_read((uint64_t)(uintptr_t)&proc_struct_size, &got, sizeof(got))
        != sizeof(got)) {
        return 0;
    }
    if (actual) *actual = got;
    return got == QUD_EXPECT_PROC_SIZE;
}

/* Resolve a task pointer to its owning proc via map->pm_task? No: the reliable
 * route for THIS board is proc = task - 0 for the embedded case only, which is
 * not true in general. Instead go through the exported `kernproc` anchor for the
 * first task, and walk `tasks`+0x58 for the rest. Callers pass the task pointer
 * they got from the list, so pid/comm are looked up on the *proc* side only when
 * the caller also supplies it. Keeping the two separate is deliberate. */
int
qud_task_info(uint64_t task, uint64_t *map, uint64_t *threads, uint32_t *nthreads,
              uint32_t *priority)
{
    uint8_t raw[8];
    uint64_t v = 0;

    if (!qud_kptr_ok(task)) return 0;
    if (qud_diag_read(task + QUD_TASK_MAP, raw, 8) != 8) return 0;
    v = *(uint64_t *)raw;
    if (map) *map = v;
    if (qud_diag_read(task + QUD_TASK_THREADS, raw, 8) != 8) return 0;
    v = *(uint64_t *)raw;
    if (threads) *threads = v;
    if (qud_diag_read(task + QUD_TASK_THREAD_COUNT, raw, 4) != 4) return 0;
    if (nthreads) *nthreads = *(uint32_t *)raw;
    if (qud_diag_read(task + QUD_TASK_PRIORITY, raw, 4) != 4) return 0;
    if (priority) *priority = *(uint32_t *)raw;
    return 1;
}

/* proc side: pid and a NUL-trimmed command name. */
int
qud_proc_info(uint64_t proc, uint32_t *pid, char *comm, size_t comm_cap)
{
    uint8_t raw[QUD_PROC_COMM_LEN];
    size_t i;

    if (!qud_kptr_ok(proc) || !comm || comm_cap == 0) return 0;
    if (qud_diag_read(proc + QUD_PROC_PID, raw, 4) != 4) return 0;
    if (pid) *pid = *(uint32_t *)raw;
    if (qud_diag_read(proc + QUD_PROC_COMM, raw, QUD_PROC_COMM_LEN) != QUD_PROC_COMM_LEN) {
        return 0;
    }
    for (i = 0; i + 1 < comm_cap && i < QUD_PROC_COMM_LEN && raw[i] != 0; i++) {
        comm[i] = (char)raw[i];
    }
    comm[i] = 0;
    return 1;
}

/* thread -> task on arm64: no direct field. t_tro at +0x438, then *(u64*)(tro+0x28). */
int
qud_thread_task(uint64_t thread, uint64_t *task)
{
    uint8_t raw[8];
    uint64_t tro;

    if (!qud_kptr_ok(thread)) return 0;
    if (qud_diag_read(thread + 0x438u, raw, 8) != 8) return 0;
    tro = *(uint64_t *)raw;
    if (!qud_kptr_ok(tro)) return 0;
    if (qud_diag_read(tro + 0x28u, raw, 8) != 8) return 0;
    if (task) *task = *(uint64_t *)raw;
    return 1;
}

/* A caller-supplied pointer: is it mapped, and what are its first `words`
 * 8-byte values? This is the honest answer when the struct layout is unknown --
 * the caller decides what the words mean. */
int
qud_ptr_probe(uint64_t va, uint64_t *words, unsigned count)
{
    if (!words || !count || count > 8u) return 0;
    if (!qud_kptr_ok(va)) return 0;
    if (qud_diag_read(va, words, (size_t)count * sizeof(uint64_t))
        != (size_t)count * sizeof(uint64_t)) return 0;
    return 1;
}

/* ---- BKP*: software breakpoint table ------------------------------------
 *
 * Pure module state: a list of kernel virtual addresses the operator wants to
 * remember, with an optional 32-bit word to compare. Nothing is written to the
 * kernel, no hardware breakpoint registers are programmed, and no exception
 * handler is installed -- those need CPU debug registers per core, which this
 * module has no business touching from a USB worker.
 *
 * What it is good for: pairing with PTDUMP. Register an address, let PTDUMP walk
 * it, then dump the word -- a poor man's watchpoint that survives the rebuild
 * cycle because the addresses live in the console, not in the kernel. */
#define QUD_BKP_MAX 8u

struct qud_bkp_entry {
    uint64_t va;
    uint32_t kind;      /* caller-defined tag; 0 = unset */
    uint32_t used;
};

static struct qud_bkp_entry qud_bkp_table[QUD_BKP_MAX];

/* Returns slot index, -1 full, -2 bad address, -3 duplicate. */
int
qud_bkp_add(uint64_t va, uint32_t kind)
{
    if (!qud_kptr_ok(va)) return -2;
    for (unsigned i = 0; i < QUD_BKP_MAX; i++)
        if (qud_bkp_table[i].used && qud_bkp_table[i].va == va) return -3;
    for (unsigned i = 0; i < QUD_BKP_MAX; i++) {
        if (!qud_bkp_table[i].used) {
            qud_bkp_table[i].va = va;
            qud_bkp_table[i].kind = kind;
            qud_bkp_table[i].used = 1;
            return (int)i;
        }
    }
    return -1;
}

/* Returns 1 when removed, 0 when the address was not registered. */
int
qud_bkp_del(uint64_t va)
{
    for (unsigned i = 0; i < QUD_BKP_MAX; i++) {
        if (qud_bkp_table[i].used && qud_bkp_table[i].va == va) {
            qud_bkp_table[i].used = 0;
            qud_bkp_table[i].va = 0;
            qud_bkp_table[i].kind = 0;
            return 1;
        }
    }
    return 0;
}

/* Snapshot for BKPLIST. Returns how many slots are in use. */
int
qud_bkp_list(struct qud_bkp_entry *out, unsigned max)
{
    unsigned n = 0;
    if (!out) return 0;
    for (unsigned i = 0; i < QUD_BKP_MAX && n < max; i++)
        if (qud_bkp_table[i].used) out[n++] = qud_bkp_table[i];
    return (int)n;
}

/* ---- SYSREG / SYSREGW: system register access ----------------------------
 *
 * This is the console's only blank spot that mattered: everything else reads
 * memory, page tables or MMIO, but nothing could read a system register, and
 * TCR/TTBR/SCTLR are exactly what PTDUMP needs to be cross-checked against.
 * (PTDUMP currently derives its geometry from TCR_EL1 on its own; being able to
 * read TCR_EL1 independently is what turns that from "the code computes this"
 * into "here is the register".)
 *
 * Why a table of 44 instead of all 2137 names: MRS/MSR take the register
 * encoding as an *immediate*, so a runtime-computed encoding cannot be used --
 * the instruction has to exist at compile time. The m1n1 approach encodes the
 * instruction word and executes it, which needs a writable-executable page; this
 * module maps device memory through ml_io_map, not code pages, and creating an
 * RX mapping from a USB worker is not something to do on a running kernel.
 *
 * So the table covers the registers that matter for bring-up -- identity, the
 * translation regime, fault status, EL0 thread pointers, the debug registers --
 * and anything else answers "unsupported" together with the encoding it would
 * need, so extending the table is a copy-paste rather than an investigation.
 *
 * Encodings come from m1n1's proxyclient/m1n1/arm_regs.json (the ARM standard
 * name table), field order (op0, op1, CRn, CRm, op2). Only _EL1/_EL0 registers
 * are listed: an EL2/EL3 register would trap at EL1, and a trap here is a panic.
 *
 * Writes are gated behind nabu-sysreg-write=1 for the same reason WRITEPA is
 * gated: SCTLR_EL1 and TCR_EL1 are one store away from an unrecoverable machine.
 * Reads need no gate -- MRS has no side effects. */
struct qud_sysreg {
    const char *name;
    uint8_t op0, op1, crn, crm, op2;
    uint8_t readable, writable;
};

static const struct qud_sysreg qud_sysregs[] = {
    { "MIDR_EL1",        3,0, 0,0,0,  1,0 },
    { "MPIDR_EL1",       3,0, 0,0,5,  1,0 },
    { "REVIDR_EL1",      3,0, 0,0,6,  1,0 },
    { "CTR_EL0",         3,3, 0,0,1,  1,0 },
    { "DCZID_EL0",       3,3, 0,0,7,  1,0 },
    { "ID_AA64PFR0_EL1", 3,0, 0,4,0,  1,0 },
    { "ID_AA64PFR1_EL1", 3,0, 0,4,1,  1,0 },
    { "ID_AA64ISAR0_EL1",3,0, 0,6,0,  1,0 },
    { "ID_AA64ISAR1_EL1",3,0, 0,6,1,  1,0 },
    { "ID_AA64MMFR0_EL1",3,0, 0,7,0,  1,0 },
    { "ID_AA64MMFR1_EL1",3,0, 0,7,1,  1,0 },
    { "ID_AA64MMFR2_EL1",3,0, 0,7,2,  1,0 },
    { "ID_AA64DFR0_EL1", 3,0, 0,5,0,  1,0 },
    { "ID_AA64DFR1_EL1", 3,0, 0,5,1,  1,0 },
    { "SCTLR_EL1",       3,0, 1,0,0,  1,1 },
    { "TCR_EL1",         3,0, 2,0,2,  1,1 },
    { "TTBR0_EL1",       3,0, 2,0,0,  1,1 },
    { "TTBR1_EL1",       3,0, 2,0,1,  1,1 },
    { "MAIR_EL1",        3,0,10,2,0,  1,1 },
    { "VBAR_EL1",        3,0,12,0,0,  1,1 },
    { "ESR_EL1",         3,0, 5,2,0,  1,1 },
    { "FAR_EL1",         3,0, 6,0,0,  1,1 },
    { "ELR_EL1",         3,0, 4,0,1,  1,1 },
    { "SPSR_EL1",        3,0, 4,0,0,  1,1 },
    { "SP_EL0",          3,0, 4,1,0,  1,1 },
    { "SP_EL1",          3,4, 4,1,0,  1,1 },
    { "TPIDR_EL0",       3,3,13,0,2,  1,1 },
    { "TPIDR_EL1",       3,0,13,0,4,  1,1 },
    { "TPIDRRO_EL0",     3,3,13,0,3,  1,1 },
    { "CNTKCTL_EL1",     3,0,14,1,0,  1,1 },
    { "CPACR_EL1",       3,0, 1,0,2,  1,1 },
    { "CONTEXTIDR_EL1",  3,0,13,0,1,  1,1 },
    { "PAR_EL1",         3,0, 7,4,0,  1,1 },
    { "MDSCR_EL1",       2,0, 0,2,2,  1,1 },
    { "OSLAR_EL1",       2,0, 1,0,4,  0,1 },
    { "OSDLR_EL1",       2,0, 1,3,4,  1,1 },
    { "DBGBVR0_EL1",     2,0, 0,0,4,  1,1 },
    { "DBGBCR0_EL1",     2,0, 0,0,5,  1,1 },
    { "DBGWVR0_EL1",     2,0, 0,0,6,  1,1 },
    { "DBGWCR0_EL1",     2,0, 0,0,7,  1,1 },
    { "PMCR_EL0",        3,3, 9,12,0, 1,1 },
    { "PMCCNTR_EL0",     3,3, 9,13,0, 1,1 },
    { "PMUSERENR_EL0",   3,3, 9,14,0, 1,1 },
};

static int qud_sysreg_write_enabled = -1;

static int
qud_sysreg_writes_allowed(void)
{
    if (qud_sysreg_write_enabled < 0) {
        int v = 0;
        PE_parse_boot_argn("nabu-sysreg-write", &v, sizeof(v));
        qud_sysreg_write_enabled = v ? 1 : 0;
    }
    return qud_sysreg_write_enabled;
}

/* Compare a token against a table name. Returns 1 on exact match. */
static int
qud_name_eq(const uint8_t *tok, size_t len, const char *name)
{
    size_t i = 0;
    while (i < len && name[i] && (char)tok[i] == name[i]) i++;
    return i == len && name[i] == 0;
}

/* Find a table entry by name. Returns NULL when unknown. */
static const struct qud_sysreg *
qud_sysreg_find(const uint8_t *tok, size_t len)
{
    for (unsigned i = 0; i < sizeof(qud_sysregs)/sizeof(qud_sysregs[0]); i++)
        if (qud_name_eq(tok, len, qud_sysregs[i].name)) return &qud_sysregs[i];
    return NULL;
}

/* The read side. Every case below is a compile-time constant encoding, which is
 * what MRS needs. Keep this list in the same order as the table above. */
#define QUD_MRS(r)  __asm__ volatile("mrs %0, " #r : "=r"(v))
#define QUD_MSR(r)  __asm__ volatile("msr " #r ", %0" :: "r"(value) : "memory")

int
qud_sysreg_read(const uint8_t *tok, size_t len, uint64_t *out)
{
    uint64_t v = 0;

    if (!out) return 0;
    if (qud_name_eq(tok, len, "MIDR_EL1"))         { QUD_MRS(midr_el1); }
    else if (qud_name_eq(tok, len, "MPIDR_EL1"))   { QUD_MRS(mpidr_el1); }
    else if (qud_name_eq(tok, len, "REVIDR_EL1"))  { QUD_MRS(revidr_el1); }
    else if (qud_name_eq(tok, len, "CTR_EL0"))     { QUD_MRS(ctr_el0); }
    else if (qud_name_eq(tok, len, "DCZID_EL0"))   { QUD_MRS(dczid_el0); }
    else if (qud_name_eq(tok, len, "ID_AA64PFR0_EL1"))  { QUD_MRS(id_aa64pfr0_el1); }
    else if (qud_name_eq(tok, len, "ID_AA64PFR1_EL1"))  { QUD_MRS(id_aa64pfr1_el1); }
    else if (qud_name_eq(tok, len, "ID_AA64ISAR0_EL1")) { QUD_MRS(id_aa64isar0_el1); }
    else if (qud_name_eq(tok, len, "ID_AA64ISAR1_EL1")) { QUD_MRS(id_aa64isar1_el1); }
    else if (qud_name_eq(tok, len, "ID_AA64MMFR0_EL1")) { QUD_MRS(id_aa64mmfr0_el1); }
    else if (qud_name_eq(tok, len, "ID_AA64MMFR1_EL1")) { QUD_MRS(id_aa64mmfr1_el1); }
    else if (qud_name_eq(tok, len, "ID_AA64MMFR2_EL1")) { QUD_MRS(id_aa64mmfr2_el1); }
    else if (qud_name_eq(tok, len, "ID_AA64DFR0_EL1"))  { QUD_MRS(id_aa64dfr0_el1); }
    else if (qud_name_eq(tok, len, "ID_AA64DFR1_EL1"))  { QUD_MRS(id_aa64dfr1_el1); }
    else if (qud_name_eq(tok, len, "SCTLR_EL1"))   { QUD_MRS(sctlr_el1); }
    else if (qud_name_eq(tok, len, "TCR_EL1"))     { QUD_MRS(tcr_el1); }
    else if (qud_name_eq(tok, len, "TTBR0_EL1"))   { QUD_MRS(ttbr0_el1); }
    else if (qud_name_eq(tok, len, "TTBR1_EL1"))   { QUD_MRS(ttbr1_el1); }
    else if (qud_name_eq(tok, len, "MAIR_EL1"))    { QUD_MRS(mair_el1); }
    else if (qud_name_eq(tok, len, "VBAR_EL1"))    { QUD_MRS(vbar_el1); }
    else if (qud_name_eq(tok, len, "ESR_EL1"))     { QUD_MRS(esr_el1); }
    else if (qud_name_eq(tok, len, "FAR_EL1"))     { QUD_MRS(far_el1); }
    else if (qud_name_eq(tok, len, "ELR_EL1"))     { QUD_MRS(elr_el1); }
    else if (qud_name_eq(tok, len, "SPSR_EL1"))    { QUD_MRS(spsr_el1); }
    else if (qud_name_eq(tok, len, "SP_EL0"))      { QUD_MRS(sp_el0); }
    else if (qud_name_eq(tok, len, "TPIDR_EL0"))   { QUD_MRS(tpidr_el0); }
    else if (qud_name_eq(tok, len, "TPIDR_EL1"))   { QUD_MRS(tpidr_el1); }
    else if (qud_name_eq(tok, len, "TPIDRRO_EL0")) { QUD_MRS(tpidrro_el0); }
    else if (qud_name_eq(tok, len, "CNTKCTL_EL1")) { QUD_MRS(cntkctl_el1); }
    else if (qud_name_eq(tok, len, "CPACR_EL1"))   { QUD_MRS(cpacr_el1); }
    else if (qud_name_eq(tok, len, "CONTEXTIDR_EL1")) { QUD_MRS(contextidr_el1); }
    else if (qud_name_eq(tok, len, "PAR_EL1"))     { QUD_MRS(par_el1); }
    else if (qud_name_eq(tok, len, "MDSCR_EL1"))   { QUD_MRS(mdscr_el1); }
    else if (qud_name_eq(tok, len, "OSDLR_EL1"))   { QUD_MRS(osdlr_el1); }
    else if (qud_name_eq(tok, len, "DBGBVR0_EL1")) { QUD_MRS(dbgbvr0_el1); }
    else if (qud_name_eq(tok, len, "DBGBCR0_EL1")) { QUD_MRS(dbgbcr0_el1); }
    else if (qud_name_eq(tok, len, "DBGWVR0_EL1")) { QUD_MRS(dbgwvr0_el1); }
    else if (qud_name_eq(tok, len, "DBGWCR0_EL1")) { QUD_MRS(dbgwcr0_el1); }
    else if (qud_name_eq(tok, len, "PMCR_EL0"))    { QUD_MRS(pmcr_el0); }
    else if (qud_name_eq(tok, len, "PMCCNTR_EL0")) { QUD_MRS(pmccntr_el0); }
    else if (qud_name_eq(tok, len, "PMUSERENR_EL0")) { QUD_MRS(pmuserenr_el0); }
    else return 0;                     /* unknown or not readable */
    *out = v;
    return 1;
}

/* The write side. Returns 1 written, 0 unknown/not-writable, -1 gated.
 *
 * Reads are safe by construction; writes are not, so the boot-arg gate is
 * checked before anything else. The dangerous ones (SCTLR/TCR/TTBR) are marked
 * writable in the table only because a bring-up console eventually needs them --
 * the gate is what keeps that from happening by accident. */
int
qud_sysreg_write(const uint8_t *tok, size_t len, uint64_t value)
{
    const struct qud_sysreg *e = qud_sysreg_find(tok, len);

    if (!e || !e->writable) return 0;
    if (!qud_sysreg_writes_allowed()) return -1;
    if (qud_name_eq(tok, len, "SCTLR_EL1"))   { QUD_MSR(sctlr_el1); }
    else if (qud_name_eq(tok, len, "TCR_EL1")){ QUD_MSR(tcr_el1); }
    else if (qud_name_eq(tok, len, "TTBR0_EL1")) { QUD_MSR(ttbr0_el1); }
    else if (qud_name_eq(tok, len, "TTBR1_EL1")) { QUD_MSR(ttbr1_el1); }
    else if (qud_name_eq(tok, len, "MAIR_EL1"))  { QUD_MSR(mair_el1); }
    else if (qud_name_eq(tok, len, "VBAR_EL1"))  { QUD_MSR(vbar_el1); }
    else if (qud_name_eq(tok, len, "ESR_EL1"))   { QUD_MSR(esr_el1); }
    else if (qud_name_eq(tok, len, "FAR_EL1"))   { QUD_MSR(far_el1); }
    else if (qud_name_eq(tok, len, "ELR_EL1"))   { QUD_MSR(elr_el1); }
    else if (qud_name_eq(tok, len, "SPSR_EL1"))  { QUD_MSR(spsr_el1); }
    else if (qud_name_eq(tok, len, "SP_EL0"))    { QUD_MSR(sp_el0); }
    else if (qud_name_eq(tok, len, "TPIDR_EL0")) { QUD_MSR(tpidr_el0); }
    else if (qud_name_eq(tok, len, "TPIDR_EL1")) { QUD_MSR(tpidr_el1); }
    else if (qud_name_eq(tok, len, "CNTKCTL_EL1")) { QUD_MSR(cntkctl_el1); }
    else if (qud_name_eq(tok, len, "CPACR_EL1")) { QUD_MSR(cpacr_el1); }
    else if (qud_name_eq(tok, len, "CONTEXTIDR_EL1")) { QUD_MSR(contextidr_el1); }
    else if (qud_name_eq(tok, len, "PAR_EL1"))   { QUD_MSR(par_el1); }
    else if (qud_name_eq(tok, len, "MDSCR_EL1")) { QUD_MSR(mdscr_el1); }
    else if (qud_name_eq(tok, len, "OSLAR_EL1")) { QUD_MSR(oslar_el1); }
    else if (qud_name_eq(tok, len, "OSDLR_EL1")) { QUD_MSR(osdlr_el1); }
    else if (qud_name_eq(tok, len, "DBGBVR0_EL1")) { QUD_MSR(dbgbvr0_el1); }
    else if (qud_name_eq(tok, len, "DBGBCR0_EL1")) { QUD_MSR(dbgbcr0_el1); }
    else if (qud_name_eq(tok, len, "DBGWVR0_EL1")) { QUD_MSR(dbgwvr0_el1); }
    else if (qud_name_eq(tok, len, "DBGWCR0_EL1")) { QUD_MSR(dbgwcr0_el1); }
    else if (qud_name_eq(tok, len, "PMCR_EL0"))  { QUD_MSR(pmcr_el0); }
    else if (qud_name_eq(tok, len, "PMCCNTR_EL0")) { QUD_MSR(pmccntr_el0); }
    else if (qud_name_eq(tok, len, "PMUSERENR_EL0")) { QUD_MSR(pmuserenr_el0); }
    else return 0;
    return 1;
}

/* Encoding of a name we do not support, so the caller can extend the table
 * without going back to arm_regs.json. Returns 1 when the name is known to the
 * table but has no instruction hooked up. */
int
qud_sysreg_encoding(const uint8_t *tok, size_t len, unsigned *op0, unsigned *op1,
                    unsigned *crn, unsigned *crm, unsigned *op2)
{
    const struct qud_sysreg *e = qud_sysreg_find(tok, len);
    if (!e) return 0;
    if (op0) *op0 = e->op0;
    if (op1) *op1 = e->op1;
    if (crn) *crn = e->crn;
    if (crm) *crm = e->crm;
    if (op2) *op2 = e->op2;
    return 1;
}

#endif /* QCOM_USB_DIAG_C */

/* ==== NABU-PROBE-L808 BEGIN (temporary) ==== */
static int qud_diag_cursor_is_phys(void)
{
    return qud_diag_cursor.is_phys;
}
/* ==== NABU-PROBE-L808 END ==== */
