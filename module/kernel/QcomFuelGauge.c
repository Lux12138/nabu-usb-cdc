/* SPDX-License-Identifier: MIT */
/* T144: read-only PM8150B Gen4 fuel gauge probe for the nabu USB console.
 *
 * Included by QcomXnuUsbDebug.c (same translation unit as QcomUsbDiag.c and
 * QcomUsbBusPrims.c), so it inherits the XNU headers, the writer helpers and
 * the weak-import discipline already in scope.  It is also compiled standalone
 * on a host with NABU_FUEL_GAUGE_HOST_TEST for the offline selftest, which
 * injects a fake SPMI register file instead of the arbiter.
 *
 * WHY THIS EXISTS
 * T91 section 4 P1 asks for the first readable SOC on this board.  The SPMI
 * transport (drivers/NabuSpmi, boot-arg nabu-spmi=1) is already measured: FG
 * SID2 ppid 0x240 resolves to apid 283.  This unit adds the register semantics
 * and a console verb; it is read-only and gated behind nabu-fg=1.
 *
 * READ SEQUENCE (donor NabuPowerCore.cpp / Gen4Sram.cpp):
 *   identify   0x4005 == 0x10 (bank 0x40) and 0x4105 == 0x11 (bank 0x41)
 *   SOC        coherent 0x4009 / shadow 0x400a          (1 byte)
 *   VBATT      coherent 0x41a0 / shadow 0x41a6          (2 bytes, LE)
 *   IBATT      coherent 0x41a2 / shadow 0x41a8          (2 bytes, signed LE)
 *   singles    0x1210 batif, 0x1310 usb, 0x1042 enable, 0x1006 phase
 *
 * APID DISCIPLINE: one apid addresses one register bank.  283 is the measured
 * channel for PPID 0x240 (bank 0x40); every other bank used here is resolved
 * through nabu_spmi_apid_for() and printed.  An unresolved bank aborts the read
 * instead of reusing another bank's channel -- guessing an apid is how a value
 * from an unrelated peripheral gets published as a battery reading.
 *
 * CONVERSIONS (donor exact, qcom/fg-util.c lineage):
 *   SOC      raw 255 -> 100, raw 0 -> 0, else ((raw-1)*98+126)/253 + 1
 *   VBATT    raw * 122070 / 1000 uV   (i.e. 122.07 uV/LSB)
 *   IBATT    signed_raw * 488281 / 1000 uA (488.281 uA/LSB; positive = discharge)
 *   phase    status & 7; charging when installed && external && enabled && 1..4
 *
 * NOT HERE (later batches, see README-PATCH.md): IOPMPowerSource publishing,
 * learned-capacity correction, MEMIF grant writes (temperature / word287),
 * and any charger control register.  Temperature therefore reports "na".
 *
 * UNIT CAVEAT: the donor comments say "122070 uV/LSB" and "488281 uA/LSB",
 * but the same lines divide by 1000, so the effective LSB is 122.07 uV /
 * 488.281 uA.  The larger numbers are nanounits.  Marked INFERRED: the
 * factors were never measured on this board.
 */
#ifndef QCOM_FUEL_GAUGE_C
#define QCOM_FUEL_GAUGE_C

#include "QcomFuelGauge.h"

#include <stddef.h>
#include <stdint.h>

#ifdef NABU_FUEL_GAUGE_HOST_TEST
#include <string.h>
/* Host selftest: plain externs, implemented by the harness. */
extern int  nabu_spmi_init(void);
extern int  nabu_spmi_enabled(void);
extern int  nabu_spmi_apid_for(uint8_t sid, uint16_t reg);
extern int  nabu_spmi_ext_readl(uint16_t apid, uint16_t reg, uint8_t *out);
extern int  nabu_spmi_ext_read(uint16_t apid, uint16_t reg, uint8_t *out, size_t len);
#else
#include <stdbool.h>
#include <string.h>
#include <pexpert/pexpert.h>
/* Same layer the buttons use.  Weak imports: a kernel built without
 * drivers/NabuSpmi links, and FGREAD answers ERR nosp mi instead of trapping
 * through a null call target. */
extern bool nabu_spmi_init(void) __attribute__((weak_import));
extern bool nabu_spmi_enabled(void) __attribute__((weak_import));
extern int  nabu_spmi_apid_for(uint8_t sid, uint16_t reg) __attribute__((weak_import));
extern bool nabu_spmi_ext_readl(uint16_t apid, uint16_t reg, uint8_t *out) __attribute__((weak_import));
extern bool nabu_spmi_ext_read(uint16_t apid, uint16_t reg, uint8_t *out, size_t len) __attribute__((weak_import));
#endif

/* Peripheral identity: PM8150B SID2.  Registers are addressed by their own
 * bank (address >> 8); the arbiter maps each bank to a channel. */
#define NABU_FG_SID             2u

#define NABU_FG_REG_ID_LO       0x4005u
#define NABU_FG_REG_ID_HI       0x4105u
#define NABU_FG_ID_LO_VALUE     0x10u
#define NABU_FG_ID_HI_VALUE     0x11u

#define NABU_FG_REG_SOC         0x4009u
#define NABU_FG_REG_SOC_SHADOW  0x400au
#define NABU_FG_REG_VOLT        0x41a0u
#define NABU_FG_REG_VOLT_SHADOW 0x41a6u
#define NABU_FG_REG_CURR        0x41a2u
#define NABU_FG_REG_CURR_SHADOW 0x41a8u

#define NABU_FG_REG_BATIF       0x1210u
#define NABU_FG_REG_USB         0x1310u
#define NABU_FG_REG_ENABLE      0x1042u
#define NABU_FG_REG_STATUS      0x1006u

/* Plausibility box.  Deliberately wider than the T91 acceptance band so one
 * bad sample is reported, not clipped. */
#define NABU_FG_VOLT_MIN_UV     1500000u
#define NABU_FG_VOLT_MAX_UV     5000000u
#define NABU_FG_CURR_MAX_UA     12000000

#define NABU_FG_DEFAULT_MAH     8720u   /* donor design capacity */

/* ---- pure converters ----------------------------------------------------- */

unsigned nabu_fg_capacity(uint8_t raw)
{
    if (raw == 255u) return 100u;
    if (raw == 0u) return 0u;
    return (unsigned)(((unsigned)(raw - 1u) * 98u + 126u) / 253u) + 1u;
}

uint32_t nabu_fg_voltage_uv(uint16_t raw)
{
    return (uint32_t)((uint64_t)raw * 122070u / 1000u);
}

int32_t nabu_fg_current_ua(uint16_t raw)
{
    int32_t signed_raw = (raw & 0x8000u) ? (int32_t)raw - 65536 : (int32_t)raw;
    return (int32_t)((int64_t)signed_raw * 488281 / 1000);
}

/* Gen4 BATT_TEMP word: 10-bit signed, 0.25 C per LSB, donor returns centi-C. */
int32_t nabu_fg_temp_centic(uint16_t raw)
{
    int32_t v = (int32_t)(raw & 1023u);
    if (v & 512) v -= 1024;
    return v * 25;
}

uint32_t nabu_fg_soc_mah(uint32_t full_mah, unsigned pct)
{
    if (pct > 100u) pct = 100u;
    return full_mah * (uint32_t)pct / 100u;
}

int nabu_fg_in_range(const struct nabu_fg_sample *s)
{
    if (!s) return 0;
    if (s->voltage_uv < NABU_FG_VOLT_MIN_UV || s->voltage_uv > NABU_FG_VOLT_MAX_UV)
        return 0;
    if (s->current_ua > NABU_FG_CURR_MAX_UA || s->current_ua < -NABU_FG_CURR_MAX_UA)
        return 0;
    return 1;
}

/* ---- boot-arg gates ------------------------------------------------------ */

#ifndef NABU_FUEL_GAUGE_HOST_TEST
static int nabu_fg_gate_state = -1;
#endif

int nabu_fg_enabled(void)
{
#ifdef NABU_FUEL_GAUGE_HOST_TEST
    return 1;   /* the host test exercises the logic, not the gate */
#else
    int v = 0;
    if (nabu_fg_gate_state < 0) {
        (void)PE_parse_boot_argn("nabu-fg", &v, sizeof(v));
        nabu_fg_gate_state = (v == 1) ? 1 : 0;
    }
    return nabu_fg_gate_state;
#endif
}

static uint32_t nabu_fg_design_override;

void nabu_fg_set_design_mah(uint32_t mah)
{
    nabu_fg_design_override = (mah >= 1000u && mah <= 20000u) ? mah : 0u;
}

static uint32_t nabu_fg_design_mah(void)
{
    if (nabu_fg_design_override) return nabu_fg_design_override;
#ifdef NABU_FUEL_GAUGE_HOST_TEST
    return NABU_FG_DEFAULT_MAH;
#else
    static uint32_t cached;
    static int have;
    int v = 0;
    if (!have) {
        have = 1;
        cached = NABU_FG_DEFAULT_MAH;
        if (PE_parse_boot_argn("nabu-fg-design-mah", &v, sizeof(v)) &&
            v >= 1000 && v <= 20000)
            cached = (uint32_t)v;
    }
    return cached;
#endif
}

/* ---- per-bank APID resolution -------------------------------------------- */

enum {
    NABU_FG_BANK_40 = 0,
    NABU_FG_BANK_41,
    NABU_FG_BANK_12,
    NABU_FG_BANK_13,
    NABU_FG_BANK_10,
    NABU_FG_BANK_COUNT
};

static const uint16_t nabu_fg_bank_base[NABU_FG_BANK_COUNT] = {
    0x4000u, 0x4100u, 0x1200u, 0x1300u, 0x1000u
};
static int nabu_fg_bank_apid[NABU_FG_BANK_COUNT];
static int nabu_fg_bank_ready;
static uint16_t nabu_fg_missing_bank;

static int nabu_fg_bank_index(uint16_t reg)
{
    unsigned i;
    uint16_t base = (uint16_t)(reg & 0xff00u);
    for (i = 0; i < NABU_FG_BANK_COUNT; i++)
        if (nabu_fg_bank_base[i] == base) return (int)i;
    return -1;
}

/* Resolve the channel for a register's bank.  A failure is retried on the
 * next call; only a real APID is latched. */
static int nabu_fg_bank_channel(uint16_t reg)
{
    int i = nabu_fg_bank_index(reg);
    int apid;
    if (!nabu_fg_bank_ready) {
        unsigned j;
        for (j = 0; j < NABU_FG_BANK_COUNT; j++) nabu_fg_bank_apid[j] = -1;
        nabu_fg_bank_ready = 1;
    }
    if (i < 0) {
        nabu_fg_missing_bank = (uint16_t)(reg & 0xff00u);
        return -1;
    }
    apid = nabu_fg_bank_apid[i];
    if (apid < 0) {
        apid = nabu_spmi_apid_for(NABU_FG_SID, nabu_fg_bank_base[i]);
        if (apid >= 0) nabu_fg_bank_apid[i] = apid;
    }
    if (apid < 0) nabu_fg_missing_bank = nabu_fg_bank_base[i];
    return apid;
}

/* ---- register access ----------------------------------------------------- */

/* Returns 1 on success, 0 on transfer failure, -1 when the bank has no APID. */
static int nabu_fg_bus_read(uint16_t reg, uint8_t *out, size_t len)
{
    int apid;

    if (!out || !len || len > 8u) return 0;
    apid = nabu_fg_bank_channel(reg);
    if (apid < 0) return -1;
#ifdef NABU_FUEL_GAUGE_HOST_TEST
    if (len == 1u) return nabu_spmi_ext_readl((uint16_t)apid, reg, out) ? 1 : 0;
    return nabu_spmi_ext_read((uint16_t)apid, reg, out, len) ? 1 : 0;
#else
    if (len == 1u) return nabu_spmi_ext_readl((uint16_t)apid, reg, out) ? 1 : 0;
    return nabu_spmi_ext_read((uint16_t)apid, reg, out, len) ? 1 : 0;
#endif
}

/* Read a primary register and its shadow up to three times; accept only an
 * exact match (donor Core::coherent).  Returns 0, or an nabu_fg_status code. */
static int nabu_fg_coherent(uint16_t reg, uint16_t shadow, uint8_t *out, size_t len)
{
    unsigned attempt;
    uint8_t first[2], second[2];

    if (len != 1u && len != 2u) return NABU_FG_IO;
    for (attempt = 0; attempt < 3u; attempt++) {
        int r = nabu_fg_bus_read(reg, first, len);
        if (r < 0) return NABU_FG_NO_APID;
        if (!r) return NABU_FG_IO;
        r = nabu_fg_bus_read(shadow, second, len);
        if (r < 0) return NABU_FG_NO_APID;
        if (!r) return NABU_FG_IO;
        if (first[0] != second[0]) continue;
        if (len == 2u && first[1] != second[1]) continue;
        out[0] = first[0];
        if (len == 2u) out[1] = first[1];
        return 0;
    }
    return NABU_FG_INCOHERENT;
}

/* ---- reader -------------------------------------------------------------- */

int nabu_fg_read(struct nabu_fg_sample *s)
{
    uint8_t id_lo = 0, id_hi = 0, soc = 0, volt[2] = { 0, 0 }, curr[2] = { 0, 0 };
    int rc;

    if (!s) return NABU_FG_IO;
    memset(s, 0, sizeof(*s));
    s->status = NABU_FG_DISABLED;
    s->why = NABU_FG_WHY_NONE;
    s->apid40 = s->apid41 = s->apid12 = s->apid13 = s->apid10 = 0xffffu;
    s->design_mah = nabu_fg_design_mah();

    if (!nabu_fg_enabled()) return NABU_FG_DISABLED;

#ifndef NABU_FUEL_GAUGE_HOST_TEST
    /* Weak-import presence test, same idiom as native_ufs_tel_sync above. */
    if (&nabu_spmi_init == 0) {
        s->status = NABU_FG_NO_SPMI;
        s->why = NABU_FG_WHY_NOSYM;
        return s->status;
    }
#endif
    if (!nabu_spmi_init() || !nabu_spmi_enabled()) {
        s->status = NABU_FG_NO_SPMI;
        s->why = NABU_FG_WHY_NOGATE;
        return s->status;
    }

    /* Identify spans two banks; resolve both before trusting anything. */
    rc = nabu_fg_bus_read(NABU_FG_REG_ID_LO, &id_lo, 1u);
    if (rc < 0) { s->status = NABU_FG_NO_APID; s->why = NABU_FG_WHY_NOAPID;
                  s->missing_bank = nabu_fg_missing_bank; return s->status; }
    if (!rc) { s->status = NABU_FG_IO; return s->status; }
    rc = nabu_fg_bus_read(NABU_FG_REG_ID_HI, &id_hi, 1u);
    if (rc < 0) { s->status = NABU_FG_NO_APID; s->why = NABU_FG_WHY_NOAPID;
                  s->missing_bank = nabu_fg_missing_bank; return s->status; }
    if (!rc) { s->status = NABU_FG_IO; return s->status; }

    s->apid40 = (uint16_t)nabu_fg_bank_channel(NABU_FG_REG_ID_LO);
    s->apid41 = (uint16_t)nabu_fg_bank_channel(NABU_FG_REG_ID_HI);

    if (id_lo != NABU_FG_ID_LO_VALUE || id_hi != NABU_FG_ID_HI_VALUE) {
        s->status = NABU_FG_IDENTIFY;
        return s->status;
    }

    rc = nabu_fg_coherent(NABU_FG_REG_SOC, NABU_FG_REG_SOC_SHADOW, &soc, 1u);
    if (rc) { s->status = rc; s->why = (rc == NABU_FG_NO_APID) ? NABU_FG_WHY_NOAPID : 0;
              s->missing_bank = nabu_fg_missing_bank; return s->status; }
    rc = nabu_fg_coherent(NABU_FG_REG_VOLT, NABU_FG_REG_VOLT_SHADOW, volt, 2u);
    if (rc) { s->status = rc; s->why = (rc == NABU_FG_NO_APID) ? NABU_FG_WHY_NOAPID : 0;
              s->missing_bank = nabu_fg_missing_bank; return s->status; }
    rc = nabu_fg_coherent(NABU_FG_REG_CURR, NABU_FG_REG_CURR_SHADOW, curr, 2u);
    if (rc) { s->status = rc; s->why = (rc == NABU_FG_NO_APID) ? NABU_FG_WHY_NOAPID : 0;
              s->missing_bank = nabu_fg_missing_bank; return s->status; }

    rc = nabu_fg_bus_read(NABU_FG_REG_BATIF, &s->batif, 1u);
    if (rc < 0) { s->status = NABU_FG_NO_APID; s->why = NABU_FG_WHY_NOAPID;
                  s->missing_bank = nabu_fg_missing_bank; return s->status; }
    if (!rc) { s->status = NABU_FG_IO; return s->status; }
    rc = nabu_fg_bus_read(NABU_FG_REG_USB, &s->usb, 1u);
    if (rc < 0) { s->status = NABU_FG_NO_APID; s->why = NABU_FG_WHY_NOAPID;
                  s->missing_bank = nabu_fg_missing_bank; return s->status; }
    if (!rc) { s->status = NABU_FG_IO; return s->status; }
    rc = nabu_fg_bus_read(NABU_FG_REG_ENABLE, &s->enable, 1u);
    if (rc < 0) { s->status = NABU_FG_NO_APID; s->why = NABU_FG_WHY_NOAPID;
                  s->missing_bank = nabu_fg_missing_bank; return s->status; }
    if (!rc) { s->status = NABU_FG_IO; return s->status; }
    rc = nabu_fg_bus_read(NABU_FG_REG_STATUS, &s->status_reg, 1u);
    if (rc < 0) { s->status = NABU_FG_NO_APID; s->why = NABU_FG_WHY_NOAPID;
                  s->missing_bank = nabu_fg_missing_bank; return s->status; }
    if (!rc) { s->status = NABU_FG_IO; return s->status; }

    s->apid12 = (uint16_t)nabu_fg_bank_channel(NABU_FG_REG_BATIF);
    s->apid13 = (uint16_t)nabu_fg_bank_channel(NABU_FG_REG_USB);
    s->apid10 = (uint16_t)nabu_fg_bank_channel(NABU_FG_REG_ENABLE);

    s->soc_raw = soc;
    s->soc_pct = nabu_fg_capacity(soc);
    s->voltage_raw = (uint16_t)((unsigned)volt[0] | ((unsigned)volt[1] << 8));
    s->voltage_uv = nabu_fg_voltage_uv(s->voltage_raw);
    s->current_raw = (uint16_t)((unsigned)curr[0] | ((unsigned)curr[1] << 8));
    s->current_ua = nabu_fg_current_ua(s->current_raw);
    s->installed = (s->batif & 0x30u) ? 0u : 1u;
    s->external = (s->usb & 0x10u) ? 1u : 0u;
    s->charge_enabled = (s->enable & 1u) ? 1u : 0u;
    s->phase = s->status_reg & 7u;
    s->charging = (s->installed && s->external && s->charge_enabled &&
                   s->phase >= 1u && s->phase <= 4u) ? 1u : 0u;
    s->soc_mah = nabu_fg_soc_mah(s->design_mah, s->soc_pct);

    if (!nabu_fg_in_range(s)) {
        s->status = NABU_FG_RANGE;
        return s->status;
    }
    s->status = NABU_FG_OK;
    return s->status;
}

/* ---- formatter ----------------------------------------------------------- */

struct nabu_fg_text {
    char *buf;
    size_t cap;
    size_t used;
};

static void nabu_fg_put(struct nabu_fg_text *w, const char *text)
{
    if (!w || !w->buf || !w->cap) return;
    while (*text && w->used + 1u < w->cap) w->buf[w->used++] = *text++;
    w->buf[w->used] = '\0';
}

static void nabu_fg_put_u(struct nabu_fg_text *w, uint64_t v)
{
    char tmp[24];
    unsigned n = 0;
    if (!v) { nabu_fg_put(w, "0"); return; }
    while (v && n < sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n--) {
        if (w->used + 1u < w->cap) w->buf[w->used++] = tmp[n];
    }
    w->buf[w->used] = '\0';
}

static void nabu_fg_put_i(struct nabu_fg_text *w, int64_t v)
{
    if (v < 0) {
        nabu_fg_put(w, "-");
        nabu_fg_put_u(w, (uint64_t)(-v));
    } else {
        nabu_fg_put_u(w, (uint64_t)v);
    }
}

static void nabu_fg_put_hex2(struct nabu_fg_text *w, uint8_t v)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[2];
    tmp[0] = digits[(v >> 4) & 0xf];
    tmp[1] = digits[v & 0xf];
    nabu_fg_put(w, tmp);
}

static void nabu_fg_put_apid(struct nabu_fg_text *w, const char *name, uint16_t apid)
{
    nabu_fg_put(w, " ");
    nabu_fg_put(w, name);
    nabu_fg_put(w, "=");
    if (apid == 0xffffu) nabu_fg_put(w, "na");
    else nabu_fg_put_u(w, (uint64_t)apid);
}

size_t nabu_fg_format(char *buf, size_t cap, const struct nabu_fg_sample *s)
{
    struct nabu_fg_text w;

    if (!buf || !cap) return 0;
    buf[0] = '\0';
    if (!s) return 0;
    w.buf = buf;
    w.cap = cap;
    w.used = 0;

    nabu_fg_put(&w, "[fg] st=");
    nabu_fg_put_i(&w, (int64_t)s->status);
    if (s->why != NABU_FG_WHY_NONE) {
        nabu_fg_put(&w, " why=");
        nabu_fg_put_u(&w, (uint64_t)s->why);
    }
    nabu_fg_put_apid(&w, "apid", s->apid40);
    nabu_fg_put_apid(&w, "ap41", s->apid41);
    nabu_fg_put_apid(&w, "ap12", s->apid12);
    nabu_fg_put_apid(&w, "ap13", s->apid13);
    nabu_fg_put_apid(&w, "ap10", s->apid10);
    if (s->status == NABU_FG_NO_APID) {
        nabu_fg_put(&w, " miss=0x");
        nabu_fg_put_hex2(&w, (uint8_t)(s->missing_bank >> 8));
        nabu_fg_put_hex2(&w, (uint8_t)(s->missing_bank & 0xff));
    }

    if (s->status == NABU_FG_OK || s->status == NABU_FG_RANGE) {
        nabu_fg_put(&w, " soc=");
        nabu_fg_put_u(&w, (uint64_t)s->soc_pct);
        nabu_fg_put(&w, " raw=");
        nabu_fg_put_hex2(&w, s->soc_raw);
        nabu_fg_put(&w, " v=");
        nabu_fg_put_u(&w, (uint64_t)(s->voltage_uv / 1000u));
        nabu_fg_put(&w, ".");
        nabu_fg_put_u(&w, (uint64_t)(s->voltage_uv % 1000u));
        nabu_fg_put(&w, " mv=");
        nabu_fg_put_u(&w, (uint64_t)(s->voltage_uv / 1000u));
        nabu_fg_put(&w, " ma=");
        nabu_fg_put_i(&w, (int64_t)(s->current_ua / 1000));
        nabu_fg_put(&w, " t=");
        if (s->temp_valid) nabu_fg_put_i(&w, (int64_t)s->temp_centi_c);
        else nabu_fg_put(&w, "na");
        nabu_fg_put(&w, " cap=");
        nabu_fg_put_u(&w, (uint64_t)s->design_mah);
        nabu_fg_put(&w, " socmah=");
        nabu_fg_put_u(&w, (uint64_t)s->soc_mah);
        nabu_fg_put(&w, " ext=");
        nabu_fg_put_u(&w, (uint64_t)s->external);
        nabu_fg_put(&w, " en=");
        nabu_fg_put_u(&w, (uint64_t)s->charge_enabled);
        nabu_fg_put(&w, " ph=");
        nabu_fg_put_u(&w, (uint64_t)s->phase);
        nabu_fg_put(&w, " chg=");
        nabu_fg_put_u(&w, (uint64_t)s->charging);
    } else {
        switch (s->status) {
        case NABU_FG_DISABLED: nabu_fg_put(&w, " state=disabled"); break;
        case NABU_FG_NO_SPMI:
            nabu_fg_put(&w, s->why == NABU_FG_WHY_NOSYM ?
                        " state=nospmi need=drivers/NabuSpmi" :
                        " state=nospmi need=nabu-spmi=1");
            break;
        case NABU_FG_NO_APID: nabu_fg_put(&w, " state=noapid"); break;
        case NABU_FG_IDENTIFY: nabu_fg_put(&w, " state=identify"); break;
        case NABU_FG_IO: nabu_fg_put(&w, " state=io"); break;
        case NABU_FG_INCOHERENT: nabu_fg_put(&w, " state=incoherent"); break;
        default: break;
        }
    }
    return w.used;
}

#endif /* QCOM_FUEL_GAUGE_C */
