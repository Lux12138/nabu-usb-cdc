/* SPDX-License-Identifier: MIT */
#ifndef QCOM_FUEL_GAUGE_H
#define QCOM_FUEL_GAUGE_H
#include <stddef.h>
#include <stdint.h>

/* nabu PM8150B Gen4 fuel gauge (SID2) read-only probe for the USB console.
 *
 * Scope of this unit (T144 / T91 P1): read state, convert it, print it.
 * It never writes a PMIC register.  Temperature and learned capacity need the
 * Gen4 MEMIF grant (0x4340/0x4350), which requires a write channel the SPMI
 * layer does not have yet; they stay "na" here and land with T91 P3.
 *
 * APID: the SPMI arbiter maps each register bank (address >> 8) to its own
 * channel.  The measured 283 belongs to SID2 bank 0x40 (PPID 0x240) only; the
 * voltage (0x41), insert/status (0x12/0x13) and enable/phase (0x10) banks are
 * resolved separately with nabu_spmi_apid_for().  A bank with no mapping stops
 * the read (NO_APID) -- no apid is ever guessed (T91 section 5 risk 1).
 *
 * Register semantics follow donor drivers/NabuPower/NabuPowerCore.cpp and
 * Gen4Sram.cpp (qcom/pm8150b.dtsi + Android commit 7d30ec43), NOT the word
 * numbers printed in some design notes: T91 section 5.3 records that the
 * "word265 / 2xword264 / word275 / 3xword282" notation could not be closed
 * against byte addresses, so this file uses the donor byte addresses only and
 * marks the word notation as unmapped. */

enum nabu_fg_status {
    NABU_FG_OK = 1,
    NABU_FG_DISABLED = -1,   /* nabu-fg=1 absent */
    NABU_FG_NO_SPMI = -2,    /* SPMI transport absent or its own gate is off */
    NABU_FG_NO_APID = -3,    /* an arbiter bank has no APID_MAP entry */
    NABU_FG_IDENTIFY = -4,   /* 0x4005 / 0x4105 do not name a Gen4 FG */
    NABU_FG_IO = -5,         /* a register read failed (timeout / bus error) */
    NABU_FG_INCOHERENT = -6, /* primary and shadow words disagree 3 times */
    NABU_FG_RANGE = -7       /* conversion landed outside the plausibility box */
};

enum nabu_fg_why {
    NABU_FG_WHY_NONE = 0,
    NABU_FG_WHY_NOSYM = 1,   /* nabu_spmi_* not linked into this kernel */
    NABU_FG_WHY_NOGATE = 2,  /* nabu_spmi_init() refused: needs nabu-spmi=1 */
    NABU_FG_WHY_NOAPID = 3
};

struct nabu_fg_sample {
    int status;
    uint8_t why;
    /* Per-bank APID actually used (0xffff = unresolved). */
    uint16_t apid40;
    uint16_t apid41;
    uint16_t apid12;
    uint16_t apid13;
    uint16_t apid10;
    uint16_t missing_bank;
    /* SOC */
    uint8_t soc_raw;
    unsigned soc_pct;
    /* VBATT, 122.07 uV per LSB (donor: raw*122070/1000 uV) */
    uint16_t voltage_raw;
    uint32_t voltage_uv;
    /* IBATT, 488.281 uA per LSB signed; FG sign: positive = discharge */
    uint16_t current_raw;
    int32_t current_ua;
    /* status inputs */
    uint8_t batif;
    uint8_t usb;
    uint8_t enable;
    uint8_t status_reg;
    unsigned installed;
    unsigned external;
    unsigned charge_enabled;
    unsigned phase;
    unsigned charging;
    /* capacity basis (mAh).  Learned capacity is P3; P1 uses the design value
     * from nabu-fg-design-mah (default 8720, donor constant). */
    uint32_t design_mah;
    uint32_t soc_mah;
    /* P3 fields, decoded but not wired until the SRAM grant exists. */
    int temp_valid;
    int32_t temp_centi_c;
    uint32_t learned_mah;
};

/* Pure converters.  Host-tested; no I/O, no globals. */
unsigned    nabu_fg_capacity(uint8_t raw);
uint32_t    nabu_fg_voltage_uv(uint16_t raw);
int32_t     nabu_fg_current_ua(uint16_t raw);
int32_t     nabu_fg_temp_centic(uint16_t raw);
uint32_t    nabu_fg_soc_mah(uint32_t full_mah, unsigned pct);
/* Plausibility box used by the reader before it trusts a conversion.  The
 * windows are wider than T91's "3.0-4.5 V / +/-10 A" acceptance bands so a
 * single bad sample is flagged without being clipped. */
int         nabu_fg_in_range(const struct nabu_fg_sample *s);

int         nabu_fg_enabled(void);
/* Per-call capacity basis override (mAh, 1000..20000).  Zero restores the
 * boot-arg / default value. */
void        nabu_fg_set_design_mah(uint32_t mah);
int         nabu_fg_read(struct nabu_fg_sample *out);
size_t      nabu_fg_format(char *buf, size_t cap, const struct nabu_fg_sample *s);

#endif /* QCOM_FUEL_GAUGE_H */
