/* SPDX-License-Identifier: MIT
 * Read-only bus primitives for the nabu USB console: TLMM GPIO state and a
 * GIC snapshot.
 *
 * Included by QcomXnuUsbDebug.c (same translation unit as QcomUsbDiag.c), so
 * it inherits the XNU headers and the writer helpers already in scope. It is
 * also compilable standalone on a host with NABU_BUS_HOST_TEST for the
 * offline selftest, which injects the MMIO reader and the PA policy.
 *
 * WHY THIS EXISTS
 * T132 section 2.5 found the bus layer empty: the console could read MMIO but
 * could not ask a question of a bus. This file adds the two read-only probes
 * that need no controller driver and therefore carry no bring-up dependency:
 *
 *   GPIOGET / GPIODIR  -- TLMM GPIO block words, straight MMIO.
 *   GICSNAP           -- GICv3 distributor / redistributor status words.
 *
 * Both are gated behind the boot-arg "nabu-verbs-bus=1" and answer
 * "ERR disabled" while it is off, byte-for-byte identical to a build without
 * this feature.
 *
 * T149 -- THE TWO RULES THIS FILE NOW OBEYS
 * 1. No address table of its own. Every physical read goes through
 *    qud_read_pa_allowed(), the whitelist READPA already enforces. T141 had a
 *    private static window table {0x03000000,0x03100000,"tlmm"} and treated a
 *    whole 1 MiB as readable because the dts declares it; reading 0x03001000
 *    through that table wedged the CPU on the t140 board round (no panic, USB
 *    dropped, bootloader watchdog reset ~20 s later). "The address exists" and
 *    "reading it right now is safe" are different statements, exactly as
 *    QcomUsbDiag.c:375-380 warns. See README-PATCH.md sections R1/R2/R3.
 * 2. Every read is announced on the console log before it happens, naming the
 *    PA. The wedge above left no trace of 0x03001000 anywhere, which is the
 *    failure mode the diag whitelist's pa-rd line was built to prevent.
 *
 * ADDRESS EVIDENCE (all from dts/nabu-full.dts, /root/merge/l738-device):
 *   interrupt-controller@17a00000  reg = <0x17a00000 0x10000>,
 *     redistributor-stride = <0x0 0x20000>, second reg = <0x17a60000 0x100000>
 *   pinctrl@03000000               reg = <0x3000000 0xdc2000>, alias "tlmm"
 *
 * GPIO register semantics inside a block are NOT confirmed on this board.
 * Mainline Linux spells the SM8150 layout as ctl_reg=+0x00, io_reg=+0x04
 * (in_bit=0/out_bit=1), intr_cfg_reg=+0x08, intr_status_reg=+0x0c
 * (drivers/pinctrl/qcom/pinctrl-sm8150.c, PINGROUP macro, v5.15 and v6.6).
 * T148 section 4 claimed +0x04 is INTR_CFG and the level lives at +0x0c; the
 * SoC's own driver source disagrees, so both words are reported under neutral
 * names and marked layout=unverified rather than guessing.
 */
#ifndef QCOM_USB_BUS_PRIMS_C
#define QCOM_USB_BUS_PRIMS_C

#include <stddef.h>
#include <stdint.h>

#ifdef NABU_BUS_HOST_TEST
/* The host test installs its own PA policy, reader and log sink, so neither
 * the XNU mapping nor the console writer is used. */
#include <string.h>
extern int nabu_bus_host_pa_allowed(uint64_t pa, size_t len, const char **why);
extern int nabu_bus_host_read_bytes(uint64_t pa, void *dst, size_t len);
extern int nabu_bus_host_log(const char *text, size_t len);
#else
#include <pexpert/pexpert.h>
#endif

/* ---- physical blocks this file asks about --------------------------------
 * These are request constants only. Whether a given PA may be read is decided
 * by qud_read_pa_allowed() at call time, never by a table here. */
#define NABU_BUS_TLMM_BASE      UINT64_C(0x03000000)
#define NABU_BUS_TLMM_STRIDE    UINT64_C(0x1000)
/* Argument bound for the verb. The gate below is stricter than this. */
#define NABU_BUS_GPIO_MAX       256u

#define NABU_BUS_GICD_BASE      UINT64_C(0x17a00000)
#define NABU_BUS_GICR_BASE      UINT64_C(0x17a60000)
#define NABU_BUS_GICR_STRIDE    UINT64_C(0x20000)
#define NABU_BUS_GICR_COUNT     8u

/* GICv3 register offsets used here. Every one is a read that does not acknowledge
 * an interrupt: ICC/IAR* and GICD_ISPENDR* are deliberately absent, because a
 * read of those changes interrupt state (T49 risk R1). */
#define NABU_GICD_CTLR          0x0000u
#define NABU_GICD_TYPER         0x0004u
#define NABU_GICD_IIDR          0x0008u
#define NABU_GICD_STATUSR       0x0010u
#define NABU_GICR_CTLR          0x0000u
#define NABU_GICR_TYPER         0x0008u
#define NABU_GICR_STATUSR       0x0010u
#define NABU_GICR_WAKER         0x0014u

/* ---- TLMM block gate (T149 R3) ------------------------------------------ */
/* Exactly these block indices may be read. An exact match, never a range: an
 * unverified neighbour must not ride along on an approved base address.
 * Grow this one entry at a time, each backed by a board round in which
 * `READPA <block base> 4` returned and the device stayed alive. */
static const unsigned nabu_bus_tlmm_verified[] = { 0u };

static int
nabu_bus_tlmm_index_allowed(unsigned index)
{
    for (unsigned i = 0; i < sizeof(nabu_bus_tlmm_verified)/sizeof(nabu_bus_tlmm_verified[0]); i++) {
        if (nabu_bus_tlmm_verified[i] == index) return 1;
    }
    return 0;
}

/* ---- PA policy seam (T149 R1) ------------------------------------------- */
#ifdef NABU_BUS_HOST_TEST
#define NABU_BUS_PA_ALLOWED(pa, len, why) nabu_bus_host_pa_allowed((pa), (len), (why))
#define NABU_BUS_LOG(text, len)           nabu_bus_host_log((text), (len))
#else
#define NABU_BUS_PA_ALLOWED(pa, len, why) qud_read_pa_allowed((pa), (len), (why))
#define NABU_BUS_LOG(text, len)           ((void)qcom_usb_debug_write((text), (len)))
#endif

/* ---- access announcement (T149 R2) -------------------------------------- */
/* Hand-rolled text for the same reason pa-rd/pa-wr in QcomUsbDiag.c are: this
 * runs from the USB worker and must not take kprintf's lock (the module
 * deliberately never enters kprintf; see the re-entry notes in
 * QcomXnuUsbDebug.c). IOLog/kprintf from here would reintroduce that hazard,
 * so the console log line is emitted through the same non-blocking writer the
 * diagnostics verbs already use. */
struct nabu_bus_text {
    char body[96];
    size_t at;
};

static void
nabu_bus_text_put(struct nabu_bus_text *t, const char *s)
{
    while (*s && t->at + 1 < sizeof(t->body)) t->body[t->at++] = *s++;
}

static void
nabu_bus_text_hex(struct nabu_bus_text *t, uint64_t v)
{
    static const char hexd[] = "0123456789abcdef";
    char digits[16];
    unsigned n = 0;

    do { digits[n++] = hexd[v & 0xfu]; v >>= 4; } while (v && n < sizeof(digits));
    /* Round up to whole 4-digit groups so an address prints the way the dts
     * spells it: 0x03001000 -> "03001000", not "0000000003001000". */
    while ((n & 3u) && n < sizeof(digits)) digits[n++] = '0';
    while (n && t->at + 1 < sizeof(t->body)) t->body[t->at++] = digits[--n];
}

static void
nabu_bus_text_dec(struct nabu_bus_text *t, size_t v)
{
    char digits[20];
    unsigned n = 0;

    do { digits[n++] = (char)('0' + (v % 10u)); v /= 10u; } while (v && n < sizeof(digits));
    while (n && t->at + 1 < sizeof(t->body)) t->body[t->at++] = digits[--n];
}

static void
nabu_bus_announce(const char *verb, uint64_t pa, size_t len, const char *why)
{
    struct nabu_bus_text t;

    t.at = 0;
    nabu_bus_text_put(&t, "nabu-bus: ");
    nabu_bus_text_put(&t, verb);
    nabu_bus_text_put(&t, " pa=0x");
    nabu_bus_text_hex(&t, pa);
    nabu_bus_text_put(&t, " len=");
    nabu_bus_text_dec(&t, len);
    nabu_bus_text_put(&t, " why=");
    nabu_bus_text_put(&t, why ? why : "outside");
    if (t.at + 1 < sizeof(t.body)) t.body[t.at++] = '\n';
    NABU_BUS_LOG(t.body, t.at);
}

/* One bounded read out of a whitelisted PA. Returns bytes read, 0 on a policy
 * refusal or a mapping failure. Nothing here writes: the only stores are into
 * the caller's buffer. */
#ifndef NABU_BUS_HOST_TEST
static size_t
nabu_bus_read(uint64_t pa, void *dst, size_t len)
{
    const char *why = "outside";
    uintptr_t va;
    size_t left;
    uintptr_t src, dsta;

    if (!len || !dst) return 0;
    /* Policy first: a refused address is announced and never mapped. */
    if (!NABU_BUS_PA_ALLOWED(pa, len, &why)) {
        nabu_bus_announce("deny", pa, len, why);
        return 0;
    }
    nabu_bus_announce("read", pa, len, why);
    /* One page-aligned mapping per call; the console asks for a handful of
     * words at a time, so a cache would be complexity without payoff. XNU
     * keeps IO mappings permanent and never unmaps them (see the note in
     * qud_diag_read_phys_anno). */
    va = ml_io_map(pa & ~UINT64_C(0xfff),
                   ((pa & UINT64_C(0xfff)) + len + 0xfffu) & ~UINT64_C(0xfff));
    if (!va) return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    src = va + (uintptr_t)(pa & UINT64_C(0xfff));
    dsta = (uintptr_t)dst;
    left = len;
    while (left) {
        /* Same volatile, alignment-checked copy discipline as
         * qud_diag_read_phys_anno: a wide access would splice neighbouring
         * registers, and an unaligned store faults on arm64. */
        if (!(src & 3u) && !(dsta & 3u) && left >= 4u) {
            *(volatile uint32_t *)dsta = *(volatile uint32_t *)src;
            src += 4u; dsta += 4u; left -= 4u;
        } else {
            *(volatile uint8_t *)dsta = *(volatile uint8_t *)src;
            src += 1u; dsta += 1u; left -= 1u;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    return len;
}
#else
static size_t
nabu_bus_read(uint64_t pa, void *dst, size_t len)
{
    const char *why = "outside";

    if (!len || !dst) return 0;
    if (!NABU_BUS_PA_ALLOWED(pa, len, &why)) {
        nabu_bus_announce("deny", pa, len, why);
        return 0;
    }
    nabu_bus_announce("read", pa, len, why);
    if (nabu_bus_host_read_bytes(pa, dst, len) != (int)len) return 0;
    return len;
}
#endif /* NABU_BUS_HOST_TEST */

/* Read one 32-bit word. Returns 1 when the whole word came back. */
static int
nabu_bus_read_word(uint64_t pa, uint32_t *out)
{
    uint8_t bytes[4];

    if (!out) return 0;
    if (nabu_bus_read(pa, bytes, sizeof(bytes)) != sizeof(bytes)) return 0;
    *out = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    return 1;
}

/* ---- boot-arg gate ------------------------------------------------------- */
static int nabu_bus_gate_state = -1;

static uint32_t
nabu_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int
nabu_bus_enabled(void)
{
#ifdef NABU_BUS_HOST_TEST
    return 1;   /* the host test exercises the logic, not the gate */
#else
    int v = 0;
    if (nabu_bus_gate_state < 0) {
        PE_parse_boot_argn("nabu-verbs-bus", &v, sizeof(v));
        nabu_bus_gate_state = v ? 1 : 0;
    }
    return nabu_bus_gate_state;
#endif
}

/* ---- GPIO ---------------------------------------------------------------- */
/* The two raw words of one TLMM block. Offsets follow the SM8150 layout in
 * mainline Linux (see the file header); the names stay neutral because the
 * layout is unverified on this board. Reading +0x08/+0x0c is deliberately not
 * done: intr_status_reg is an ack register, and a read can change interrupt
 * state (same class of hazard as ICC_IAR*, T49 risk R1). */
struct nabu_gpio_words {
    uint32_t w0;     /* block + 0x00 (mainline: ctl_reg / cfg) */
    uint32_t w1;     /* block + 0x04 (mainline: io_reg, bit0 = input level) */
};

/* Distinct answers matter to the verb reply: DENIED means the block gate or
 * the PA whitelist said no and nothing was touched; READFAIL means a permitted
 * address did not come back. */
#define NABU_GPIO_OK        1
#define NABU_GPIO_DENIED    0
#define NABU_GPIO_READFAIL  (-1)

/* Read the two words of one TLMM GPIO block.
 * `index` is bounded by NABU_BUS_GPIO_MAX before any address is formed. */
static int
nabu_bus_gpio_read(unsigned index, struct nabu_gpio_words *out)
{
    uint64_t pa;
    uint32_t w0 = 0, w1 = 0;

    if (!out) return NABU_GPIO_DENIED;
    if (index >= NABU_BUS_GPIO_MAX) return NABU_GPIO_DENIED;
    pa = (uint64_t)NABU_BUS_TLMM_BASE + (uint64_t)index * NABU_BUS_TLMM_STRIDE;
    /* R3: the block gate fires before the PA is ever mapped, and it announces
     * the address it refused so the log still names the culprit. */
    if (!nabu_bus_tlmm_index_allowed(index)) {
        nabu_bus_announce("deny", pa, 4u, "tlmm-index-unverified");
        return NABU_GPIO_DENIED;
    }
    if (!nabu_bus_read_word(pa, &w0)) return NABU_GPIO_READFAIL;
    if (!nabu_bus_read_word(pa + 4u, &w1)) return NABU_GPIO_READFAIL;
    out->w0 = w0;
    out->w1 = w1;
    return NABU_GPIO_OK;
}

/* ---- GIC snapshot -------------------------------------------------------- */
struct nabu_gic_dist {
    uint32_t ctlr, typer, iidr, statusr;
};

struct nabu_gic_redist {
    unsigned cpu;
    uint32_t ctlr, typer, statusr, waker;
};

struct nabu_gic_snap {
    struct nabu_gic_dist dist;
    struct nabu_gic_redist rd[NABU_BUS_GICR_COUNT];
    unsigned rd_ok;      /* bitmask of redistributors with any word read */
};

/* Read four distributor words and four words per redistributor.
 * Returns 1 when the distributor block came back, 0 otherwise. Each block is
 * one bounded read, so "this redistributor answered" means exactly that -- a
 * failing block does not leave a half-true snapshot behind. A failing
 * redistributor only clears its bit in rd_ok; it is never fatal. */
static int
nabu_bus_gic_snapshot(struct nabu_gic_snap *snap)
{
    uint8_t d[0x14];
    unsigned i;

    if (!snap) return 0;
    memset(snap, 0, sizeof(*snap));

    if (nabu_bus_read((uint64_t)NABU_BUS_GICD_BASE, d, sizeof(d)) != sizeof(d))
        return 0;
    snap->dist.ctlr    = nabu_le32(d + NABU_GICD_CTLR);
    snap->dist.typer   = nabu_le32(d + NABU_GICD_TYPER);
    snap->dist.iidr    = nabu_le32(d + NABU_GICD_IIDR);
    snap->dist.statusr = nabu_le32(d + NABU_GICD_STATUSR);

    for (i = 0; i < NABU_BUS_GICR_COUNT; i++) {
        const uint64_t base = (uint64_t)NABU_BUS_GICR_BASE +
                              (uint64_t)i * NABU_BUS_GICR_STRIDE;
        uint8_t r[0x18];
        struct nabu_gic_redist *rd = &snap->rd[i];

        rd->cpu = i;
        if (nabu_bus_read(base, r, sizeof(r)) != sizeof(r)) continue;
        rd->ctlr    = nabu_le32(r + NABU_GICR_CTLR);
        rd->typer   = nabu_le32(r + NABU_GICR_TYPER);
        rd->statusr = nabu_le32(r + NABU_GICR_STATUSR);
        rd->waker   = nabu_le32(r + NABU_GICR_WAKER);
        snap->rd_ok |= (1u << i);
    }
    return 1;
}

#endif /* QCOM_USB_BUS_PRIMS_C */
