/* SPDX-License-Identifier: MIT
 * SYSREGSCAN core: bounded, non-fatal batch reads of system registers.
 *
 * Included by QcomXnuUsbDebug.c. The row table and the per-row MRS stubs come
 * from nabu_sysreg_scan_table.h (generated; see tools/gen_sysreg_scan_table.py).
 *
 * HOW A BATCH CANNOT KILL THE MACHINE
 * An MRS encoding is an immediate, so every candidate needs its own stub at a
 * unique PC. XNU provides the claim mechanism -- ml_expect_fault_pc_begin()
 * stores an expected PC and osfmk/arm64/sleh.c:821 consults it on the
 * UNCATEGORIZED (EC=0x00) path for kernel code as well:
 *
 *     if (!is_user && (expected_fault_handler != NULL) && expected_fault_handler(state))
 *         break;
 *
 * This build defines CONFIG_XNUPOST (checked in the pe_kprintf.o.json compile
 * record) and HAS_ADD_SAVED_STATE_PC is unconditionally 1 in
 * osfmk/mach/arm/thread_status.h, so the handler advances the PC past the
 * .inst and drops a sentinel into x0. An undefined encoding therefore returns
 * "faulted" instead of panicking.
 *
 * THE LIMIT, STATED PLAINLY
 * Only rows in the generated table can be scanned. The default set (group 0)
 * is 43 encodings carried verbatim from qud_sysregs[] plus two that UPTIME
 * already executes; every one of them is readable at EL1 on this port. Row
 * group 1 holds a single probe encoding expected to be UNDEFINED, and it is
 * reachable only when nabu-sysregscan-trap=1.
 *
 * Why the probe is gated: an encoding that IS allocated but trapped (EL2/EL3
 * registers behind trap control) raises EC=0x18, which sleh.c routes to
 * handle_msr_trap() and panics rather than consulting the expected-fault
 * handler (T49 section 4.3). S3_6_C0_C0_0 is chosen because CRn=CRm=op2=0 with
 * op1=6 has no architectural MRS assignment. That is an inference from the
 * encoding space, not a board measurement: the first on-device run must prove
 * the box survives it, exactly as T49 section 6 criterion 2 asks.
 */
#ifndef QCOM_USB_SYSREG_SCAN_C
#define QCOM_USB_SYSREG_SCAN_C

#include <stddef.h>
#include <stdint.h>

#ifdef NABU_SCAN_HOST_TEST
extern uint64_t nabu_scan_host_probe(uint32_t enc);
#else
#include <pexpert/pexpert.h>
#include <machine/machine_routines.h>
#endif

#include "nabu_sysreg_scan_table.h"

#define NABU_MRS_SENTINEL   0xdeadc0dedeadc0deULL
#define NABU_SCAN_MAX       16u   /* reply frame ceiling: 16 * 24 hex + header */

struct nabu_scan_result {
    uint32_t enc;
    uint64_t value;
    uint8_t faulted;
};

/* Reader callback: returns the register value and sets *faulted. The console
 * passes the real stub executor; the offline selftest passes an injection. */
typedef uint64_t (*nabu_scan_reader)(void *ctx, unsigned index, int *faulted);

#ifndef NABU_BUS_HOST_TEST
static bool
nabu_scan_fault_handler(arm_saved_state_t *state)
{
    add_saved_state_pc(state, 4);
    saved_state64(state)->x[0] = NABU_MRS_SENTINEL;
    return true;
}
#endif

static int nabu_scan_gate_state = -1;
static int nabu_scan_trap_state = -1;

static int
nabu_scan_enabled(void)
{
#ifdef NABU_SCAN_HOST_TEST
    return 1;
#else
    int v = 0;
    if (nabu_scan_gate_state < 0) {
        PE_parse_boot_argn("nabu-verbs-bus", &v, sizeof(v));
        nabu_scan_gate_state = v ? 1 : 0;
    }
    return nabu_scan_gate_state;
#endif
}

static int
nabu_scan_trap_allowed(void)
{
#ifdef NABU_SCAN_HOST_TEST
    return nabu_scan_trap_state == 1;
#else
    int v = 0;
    if (nabu_scan_trap_state < 0) {
        PE_parse_boot_argn("nabu-sysregscan-trap", &v, sizeof(v));
        nabu_scan_trap_state = v ? 1 : 0;
    }
    return nabu_scan_trap_state;
#endif
}

#ifndef NABU_SCAN_HOST_TEST
/* Execute one stub under the expected-fault claim. Returns the value read (or
 * the sentinel) and reports whether the fault handler fired. Interrupts are
 * masked around the armed window: ml_expect_fault_* does not mask them, and a
 * taken interrupt inside the two-instruction stub would leave the claim armed
 * while some unrelated handler runs (T49 risk R4). */
static uint64_t
nabu_scan_exec(void *ctx, unsigned index, int *faulted)
{
    const struct nabu_scan_row *row = &nabu_scan_rows[index];
    uint64_t v;

    (void)ctx;
    if (!nabu_scan_trap_allowed()) {
        v = row->fn();
        *faulted = 0;
        return v;
    }
    /* control_handle_frame() runs with interrupts masked, so the previous
     * state is captured and restored rather than blindly re-enabled. */
    {
        const boolean_t was = ml_set_interrupts_enabled(0);
        ml_expect_fault_pc_begin(nabu_scan_fault_handler, (uintptr_t)row->fn);
        v = row->fn();
        ml_expect_fault_end();
        ml_set_interrupts_enabled(was);
    }
    *faulted = (v == NABU_MRS_SENTINEL);
    return v;
}
#endif /* !NABU_SCAN_HOST_TEST */

/* Scan rows [start, start+count) of the table.
 * Returns the number of results written (>= 0), or -1 disabled, -2 range,
 * -3 arguments. `trap_allowed` comes from the caller so the host test can force
 * it; the production path reads the boot-arg once. */
static int
nabu_sysreg_scan(unsigned start, unsigned count, struct nabu_scan_result *out,
                 unsigned max, int trap_allowed, nabu_scan_reader reader, void *ctx)
{
    unsigned i;

    if (!nabu_scan_enabled()) return -1;
    if (!out || !reader || !max || max > NABU_SCAN_MAX) return -3;
    if (count > max) return -3;
    if (start >= NABU_SCAN_ROWS || count > NABU_SCAN_ROWS - start) return -2;
    if (!trap_allowed && (start + count) > NABU_SCAN_SAFE_ROWS) return -2;

    for (i = 0; i < count; i++) {
        const unsigned idx = start + i;
        int faulted = 0;
        uint64_t v = reader(ctx, idx, &faulted);
        out[i].enc = nabu_scan_rows[idx].enc;
        out[i].value = v;
        out[i].faulted = (uint8_t)(faulted ? 1 : 0);
    }
    return (int)count;
}

#endif /* QCOM_USB_SYSREG_SCAN_C */
