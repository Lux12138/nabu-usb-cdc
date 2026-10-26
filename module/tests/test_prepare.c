/* SPDX-License-Identifier: MIT
 * Host model for QcomUsbPrepare: a fake GCC/DWC3/PHY that follows the board's
 * measured behaviour (GDSC needs SW_COLLAPSE cleared, clocks need enabling,
 * BCR releases only on an explicit 0, CORESOFTRESET/CSFTRST latch until they
 * are written back to 0, DSTS leaves halt after RUN_STOP while CONNECTSPD still
 * carries the previous connection's value, GSTS reports the current mode).
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "include/QcomUsbPrepare.h"

struct reg { uint64_t address; uint32_t value; };
struct model {
    struct reg regs[64];
    unsigned int count;
    uint32_t gdscr;
    uint32_t bcr;
    uint32_t gsts;
    uint32_t dsts_after_run;
    int gdsc_ready;
    int gdsc_stuck_off;
    int reset_stuck;
    unsigned int core_writes;
};

static struct reg *find(struct model *m, uint64_t address, int create)
{
    unsigned int i;
    for (i = 0; i < m->count; ++i) {
        if (m->regs[i].address == address) return &m->regs[i];
    }
    if (!create) return NULL;
    assert(m->count < 64);
    m->regs[m->count].address = address;
    m->regs[m->count].value = 0;
    return &m->regs[m->count++];
}

static void set(struct model *m, uint64_t address, uint32_t value)
{
    find(m, address, 1)->value = value;
}

static uint32_t get(struct model *m, uint64_t address)
{
    return find(m, address, 1)->value;
}

static int is_cbcr(uint64_t address, unsigned int *index)
{
    static const uint32_t cbcr[] = QUD_GCC_CBCR_OFFSETS;
    unsigned int i;
    for (i = 0; i < sizeof(cbcr) / sizeof(cbcr[0]); ++i) {
        if (address == QUD_GCC_BASE + cbcr[i]) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

static int read32(void *cookie, uint64_t address, uint32_t *value)
{
    struct model *m = cookie;

    if (address == QUD_GCC_BASE + QUD_GCC_GDSCR_OFFSET) {
        *value = m->gdscr | (m->gdsc_ready ? (1u << 31) : 0u);
        return 1;
    }
    if (address == QUD_GCC_BASE + QUD_GCC_BCR_OFFSET) {
        *value = m->bcr;
        return 1;
    }
    if (address == QUD_DWC3_BASE + 0xc118u /* GSTS */) {
        *value = m->gsts;
        return 1;
    }
    if (address == QUD_DWC3_BASE + 0xc110u /* GCTL */ && m->reset_stuck) {
        *value = get(m, address) | (1u << 11);
        return 1;
    }
    *value = get(m, address);
    return 1;
}

static int write32(void *cookie, uint64_t address, uint32_t value)
{
    struct model *m = cookie;
    unsigned int index;

    if (address == QUD_GCC_BASE + QUD_GCC_GDSCR_OFFSET) {
        m->gdscr = value & ~(1u << 31);
        m->gdsc_ready = !(value & 1u) && !m->gdsc_stuck_off;
        return 1;
    }
    if (address == QUD_GCC_BASE + QUD_GCC_BCR_OFFSET) {
        m->bcr = value;
        return 1;
    }
    if (is_cbcr(address, &index)) {
        /* Enabling a branch clears CLK_OFF; gating it sets it again. */
        uint32_t next = value & ~(1u << 31);
        if (!(value & 1u)) next |= (1u << 31);
        set(m, address, next);
        return 1;
    }
    if (address >= QUD_DWC3_BASE) {
        ++m->core_writes;
    }
    set(m, address, value);
    if (address == QUD_DWC3_BASE + 0xc704u /* DCTL */ && (value & (1u << 31))) {
        set(m, QUD_DWC3_BASE + 0xc70cu /* DSTS */, m->dsts_after_run);
    }
    return 1;
}

static void delay_us(void *cookie, uint32_t usec)
{
    (void)cookie;
    (void)usec;
}

static void init(struct model *m)
{
    static const uint32_t cbcr[] = QUD_GCC_CBCR_OFFSETS;
    unsigned int i;

    memset(m, 0, sizeof(*m));
    for (i = 0; i < sizeof(cbcr) / sizeof(cbcr[0]); ++i) {
        set(m, QUD_GCC_BASE + cbcr[i], 0x80000000u);   /* CLK_OFF set */
    }
    m->gdscr = 1u;                                     /* SW_COLLAPSE asserted */
    m->gdsc_ready = 0;
    m->bcr = 1u;                                       /* reset still asserted */
    m->gsts = 0;                                       /* CURMOD: device */
    /* The exact value the board read after RUN on 2026-10-26: link up, speed
     * field 0. It must not affect acceptance any more. */
    m->dsts_after_run = 0x00102800u;
    set(m, QUD_DWC3_BASE + 0xc120u, 0x33313130u);      /* GSNPSID */
    set(m, QUD_DWC3_BASE + 0xc110u, 0x00001001u);      /* GCTL: host, halted */
    set(m, QUD_DWC3_BASE + 0xc704u, 0x00000000u);      /* DCTL */
    set(m, QUD_DWC3_BASE + 0xc70cu, 0x00400000u);      /* DSTS halted */
    set(m, QUD_PHY_BASE + 0x58u, 0);                   /* COMMON1 */
    set(m, QUD_PHY_BASE + 0x60u, 0);                   /* CTRL1 */
}

int main(void)
{
    struct model m;
    QCOM_PREPARE_PLATFORM p;
    QCOM_PREPARE_RESULT r;
    uint32_t stage = 0xffffffffu, last = 0;

    p.read32 = read32;
    p.write32 = write32;
    p.cookie = &m;
    p.delay_us = delay_us;
    p.deadline = 0;

    init(&m);
    r = QcomUsbPrepare(&p, &stage, &last);
    assert(r == QPREP_OK);
    assert(stage == QPREP_STAGE_CORE);
    assert(!(m.gdscr & 1u));                       /* SW_COLLAPSE written as 0 */
    assert(m.gdsc_ready);
    assert(m.bcr == 0);                            /* released explicitly */
    assert(get(&m, QUD_DWC3_BASE + 0xc700u) == 0u);   /* DCFG.DEVSPD = 0 = HS */
    assert(get(&m, QUD_DWC3_BASE + 0xc70cu) == 0x00102800u);
    assert((get(&m, QUD_DWC3_BASE + 0xc110u) & (3u << 12)) == (2u << 12));
    assert(get(&m, QUD_DWC3_BASE + 0xc2c0u) & (1u << 17));   /* SS pipe suspended */
    assert(!(get(&m, QUD_DWC3_BASE + 0xc200u) & ((1u << 6) | (1u << 8))));
    assert(last == 0x00102800u);                      /* success signature: DSTS */
    assert(get(&m, QUD_PHY_BASE + 0x58u) & (1u << 4));   /* VBUSVLDEXTSEL0 */
    assert(get(&m, QUD_PHY_BASE + 0x60u) & 1u);          /* VBUSVLDEXT0 */

    /* A reset bit that never falls is a hard failure, not a 20 ms delay. */
    init(&m);
    m.reset_stuck = 1;
    r = QcomUsbPrepare(&p, &stage, &last);
    assert(r == QPREP_CORE);
    assert(stage == QPREP_STAGE_CORE);
    assert(last & (1u << 11));                        /* rb shows CORESOFTRESET */

    /* Acceptance is GSTS.CURMOD, so a core that reports host mode must fail
     * even though GCTL reads back the device value that was written. */
    init(&m);
    m.gsts = 1;
    r = QcomUsbPrepare(&p, &stage, &last);
    assert(r == QPREP_CORE);
    assert(last == 1u);

    /* RUN_STOP that leaves the core halted is also a failure. */
    init(&m);
    m.dsts_after_run = 0x00400000u;
    r = QcomUsbPrepare(&p, &stage, &last);
    assert(r == QPREP_CORE);
    assert(last & 0x00400000u);

    /* Power stuck off must stop at the GDSC stage, before any core write. */
    init(&m);
    m.gdsc_stuck_off = 1;
    r = QcomUsbPrepare(&p, &stage, &last);
    assert(r == QPREP_GDSC);
    assert(stage == QPREP_STAGE_CLOCKS);
    assert(m.core_writes == 0);

    /* Unknown core family must be refused after clocks, before the PHY. */
    init(&m);
    set(&m, QUD_DWC3_BASE + 0xc120u, 0x12345678u);
    r = QcomUsbPrepare(&p, &stage, &last);
    assert(r == QPREP_ID);
    assert(stage == QPREP_STAGE_ID);

    puts("QcomUsbPrepare: power, clocks, reset, id, PHY and device mode modelled");
    return 0;
}
