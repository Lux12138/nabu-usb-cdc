/* SPDX-License-Identifier: MIT
 * Argument-carrying verbs: READ / READNEXT / WRITE / SYM / SYMS.
 *
 * The framing layer used to accept only argument-free verbs
 * (SXNU1 <session> <seq> <VERB> <crc>). Diagnosing the GPU and memory paths
 * from the serial console needs addresses, so the grammar now allows text
 * between the verb and the CRC:
 *
 *     SXNU1 <16hex> <8hex> READ 0xfffffff007004000 100 <8hex crc>
 *
 * This test pins both halves of that change: every legacy verb must still
 * parse with an empty argument span, and the new verbs must hand their text
 * through untouched.
 */
#include "kernel/QcomUsbControl.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int failures;

/* Build a wire frame with a correct CRC and parse it back. */
static void
check(const char *body, int expect_ok, enum sxnu_control_verb expect_verb,
      const char *expect_args)
{
    char frame[256];
    char head[192];
    uint32_t crc;
    int head_bytes = snprintf(head, sizeof(head), "SXNU1 0123456789abcdef 00000001 %s", body);
    struct sxnu_control_frame parsed;
    int ok;

    assert(head_bytes > 0 && (size_t)head_bytes < sizeof(head));
    crc = sxnu_control_crc32(head, (size_t)head_bytes);
    assert(snprintf(frame, sizeof(frame), "%s %08x", head, crc) > 0);

    memset(&parsed, 0, sizeof(parsed));
    ok = sxnu_control_parse(frame, strlen(frame), &parsed);
    if (ok != expect_ok) {
        printf("FAIL parse(%s): got %d want %d\n", body, ok, expect_ok);
        failures++;
        return;
    }
    if (!ok) return;
    if (parsed.verb != expect_verb) {
        printf("FAIL verb(%s): got %d want %d\n", body, parsed.verb, expect_verb);
        failures++;
        return;
    }
    if (parsed.arg_bytes != strlen(expect_args) ||
        (parsed.arg_bytes && memcmp(parsed.args, expect_args, parsed.arg_bytes))) {
        printf("FAIL args(%s): got %u bytes\n", body, (unsigned)parsed.arg_bytes);
        failures++;
        return;
    }
    printf("PASS %-58s args=%u\n", body, (unsigned)parsed.arg_bytes);
}

/* Drive parse -> accept for a verb in an established session. The previous
 * bug was an accept() upper bound of SHOTNEXT, which silently rejected every
 * diagnostics verb even though parse() accepted them. */
static void
check_accept(enum sxnu_control_verb verb, const char *token, int expect)
{
    char frame[256];
    char head[192];
    uint32_t crc;
    int head_bytes = snprintf(head, sizeof(head),
        "SXNU1 0123456789abcdef 00000001 %s", token);
    struct sxnu_control_state state;
    struct sxnu_control_frame parsed;
    enum sxnu_control_verb got;

    assert(head_bytes > 0 && (size_t)head_bytes < sizeof(head));
    crc = sxnu_control_crc32(head, (size_t)head_bytes);
    assert(snprintf(frame, sizeof(frame), "%s %08x", head, crc) > 0);

    memset(&state, 0, sizeof(state));
    state.session = UINT64_C(0x0123456789abcdef);
    state.last_seq = 0;                    /* HELLO already established */
    memset(&parsed, 0, sizeof(parsed));
    if (!sxnu_control_parse(frame, strlen(frame), &parsed)) {
        printf("FAIL accept(%s): parse failed\n", token);
        failures++;
        return;
    }
    got = sxnu_control_accept(&state, &parsed);
    if ((got != SXNU_CONTROL_VERB_INVALID) != expect) {
        printf("FAIL accept(%s): got %d (INVALID=%d) expected_accept=%d\n",
               token, got, SXNU_CONTROL_VERB_INVALID, expect);
        failures++;
        return;
    }
    if (expect && got != verb) {
        printf("FAIL accept(%s): verb %d want %d\n", token, got, verb);
        failures++;
        return;
    }
    printf("PASS accept %-12s -> %s\n", token, expect ? "OK" : "INVALID");
}

int
main(void)
{
    /* Full accept-path coverage: these would all have silently failed with
     * the old SHOTNEXT upper bound. */
    check_accept(SXNU_CONTROL_VERB_STATUS, "STATUS", 1);
    check_accept(SXNU_CONTROL_VERB_SHOTNEXT, "SHOTNEXT", 1);
    check_accept(SXNU_CONTROL_VERB_READ, "READ 0xfffffff007004000 40", 1);
    check_accept(SXNU_CONTROL_VERB_READNEXT, "READNEXT 80", 1);
    check_accept(SXNU_CONTROL_VERB_WRITE, "WRITE 0xfffffff007004000 41", 1);
    check_accept(SXNU_CONTROL_VERB_SYM, "SYM gPhysBase", 1);
    check_accept(SXNU_CONTROL_VERB_SYMS, "SYMS nabu_", 1);
    check_accept(SXNU_CONTROL_VERB_READPA, "READPA 0xae00000 40", 1);
    check_accept(SXNU_CONTROL_VERB_READPANEXT, "READPANEXT 80", 1);
    check_accept(SXNU_CONTROL_VERB_WRITEPA, "WRITEPA 0xae02000 08000003", 1);
    check_accept(SXNU_CONTROL_VERB_BOOTARGS, "BOOTARGS", 1);
    check_accept(SXNU_CONTROL_VERB_VERSION, "VERSION", 1);
    check_accept(SXNU_CONTROL_VERB_DUMPUFS, "DUMPUFS 0x1008000 0x1000 smmu", 1);
    check_accept(SXNU_CONTROL_VERB_READUFS, "READUFS 0 1", 1);
    check_accept(SXNU_CONTROL_VERB_READUFS, "READUFS 0x10", 1);
    check_accept(SXNU_CONTROL_VERB_DUMLIST, "DUMLIST", 1);
    check_accept(SXNU_CONTROL_VERB_UPTIME, "UPTIME", 1);
    check_accept(SXNU_CONTROL_VERB_RDBLK, "RDBLK mdp 0 40", 1);
    check_accept(SXNU_CONTROL_VERB_PERFSTART, "PERFSTART bb8", 1);
    check_accept(SXNU_CONTROL_VERB_PERFSTOP, "PERFSTOP", 1);
    check_accept(SXNU_CONTROL_VERB_PERFREAD, "PERFREAD", 1);
    check_accept(SXNU_CONTROL_VERB_PERFCLEAR, "PERFCLEAR", 1);
    check_accept(SXNU_CONTROL_VERB_GPUFREQ, "GPUFREQ", 1);
    check_accept(SXNU_CONTROL_VERB_GPUHWCG, "GPUHWCG on", 1);
    check_accept(SXNU_CONTROL_VERB_GPUPROFILE, "GPUPROFILE", 1);
    check_accept(SXNU_CONTROL_VERB_GPUPROFILE, "GPUPROFILE status", 1);
    check_accept(SXNU_CONTROL_VERB_GMUSTATE, "GMUSTATE", 1);
    check_accept(SXNU_CONTROL_VERB_DISPREGS, "DISPREGS", 1);
    check_accept(SXNU_CONTROL_VERB_FLIPNOW, "FLIPNOW", 1);
    check_accept(SXNU_CONTROL_VERB_SMMUDUMP, "SMMUDUMP", 1);
    check_accept(SXNU_CONTROL_VERB_DEBUGGER, "DEBUGGER", 1);
    check_accept(SXNU_CONTROL_VERB_KDPPUSH, "KDPPUSH fa0102", 1);
    check_accept(SXNU_CONTROL_VERB_PTDUMP, "PTDUMP 0xfffffff007004000", 1);
    check_accept(SXNU_CONTROL_VERB_PANICDUMP, "PANICDUMP", 1);
    check_accept(SXNU_CONTROL_VERB_SYSCTL, "SYSCTL kern.maxproc", 1);
    check_accept(SXNU_CONTROL_VERB_TASKDUMP, "TASKDUMP", 1);
    check_accept(SXNU_CONTROL_VERB_THREADDUMP, "THREADDUMP 0xfffffff007004000", 1);
    check_accept(SXNU_CONTROL_VERB_VSYNCON, "VSYNCON", 1);
    check_accept(SXNU_CONTROL_VERB_BKPADD, "BKPADD 0xfffffff007004000", 1);
    check_accept(SXNU_CONTROL_VERB_BKPDEL, "BKPDEL 0xfffffff007004000", 1);
    check_accept(SXNU_CONTROL_VERB_BKPLIST, "BKPLIST", 1);
    check_accept(SXNU_CONTROL_VERB_SYSREG, "SYSREG TCR_EL1", 1);
    check_accept(SXNU_CONTROL_VERB_SYSREGW, "SYSREGW TTBR1_EL1 0xcae35000", 1);
    check_accept(SXNU_CONTROL_VERB_PANICDUMP, "PANICDUMP info", 1);
    check_accept(SXNU_CONTROL_VERB_PANICDUMP, "PANICDUMP log 0 100", 1);

    /* Legacy verbs keep the argument-free form. */
    check("HELLO", 1, SXNU_CONTROL_VERB_HELLO, "");
    check("PING", 1, SXNU_CONTROL_VERB_PING, "");
    check("STATUS", 1, SXNU_CONTROL_VERB_STATUS, "");
    check("REBOOT", 1, SXNU_CONTROL_VERB_REBOOT, "");
    check("SCREENSHOT", 1, SXNU_CONTROL_VERB_SCREENSHOT, "");
    check("SHOTNEXT", 1, SXNU_CONTROL_VERB_SHOTNEXT, "");

    /* New verbs carry their arguments verbatim. */
    check("READ 0xfffffff007004000 100", 1, SXNU_CONTROL_VERB_READ,
          "0xfffffff007004000 100");
    check("READ fffffff007004000 40", 1, SXNU_CONTROL_VERB_READ,
          "fffffff007004000 40");
    check("READNEXT", 1, SXNU_CONTROL_VERB_READNEXT, "");
    check("READNEXT 80", 1, SXNU_CONTROL_VERB_READNEXT, "80");
    check("WRITE 0xfffffff007004000 00000000", 1, SXNU_CONTROL_VERB_WRITE,
          "0xfffffff007004000 00000000");
    check("SYM gPhysBase", 1, SXNU_CONTROL_VERB_SYM, "gPhysBase");
    check("SYMS nabu_", 1, SXNU_CONTROL_VERB_SYMS, "nabu_");
    check("READPA 0xae00000 40", 1, SXNU_CONTROL_VERB_READPA, "0xae00000 40");
    check("READPANEXT 80", 1, SXNU_CONTROL_VERB_READPANEXT, "80");
    check("WRITEPA 0xae02000 08000003", 1, SXNU_CONTROL_VERB_WRITEPA,
          "0xae02000 08000003");
    check("BOOTARGS", 1, SXNU_CONTROL_VERB_BOOTARGS, "");
    check("VERSION", 1, SXNU_CONTROL_VERB_VERSION, "");
    check("DUMPUFS 0x1008000 0x1000 smmu", 1, SXNU_CONTROL_VERB_DUMPUFS, "0x1008000 0x1000 smmu");
    check("READUFS 0x10 1", 1, SXNU_CONTROL_VERB_READUFS, "0x10 1");
    check("READUFS 0x10", 1, SXNU_CONTROL_VERB_READUFS, "0x10");
    check("DUMLIST", 1, SXNU_CONTROL_VERB_DUMLIST, "");
    /* kb-40: both spellings must resolve. The console upper-cases what the user
     * types and sends it verbatim, and every other dump verb is DUMP*, so
     * "dumplist" is the natural thing to try -- it used to parse to nothing,
     * which produced no reply at all and left last_seq behind, poisoning every
     * later command on the same session. */
    check("DUMPLIST", 1, SXNU_CONTROL_VERB_DUMLIST, "");
    check("UPTIME", 1, SXNU_CONTROL_VERB_UPTIME, "");
    check("RDBLK mdp 0 40", 1, SXNU_CONTROL_VERB_RDBLK, "mdp 0 40");
    check("PERFSTART bb8", 1, SXNU_CONTROL_VERB_PERFSTART, "bb8");
    check("PERFSTOP", 1, SXNU_CONTROL_VERB_PERFSTOP, "");
    check("PERFREAD", 1, SXNU_CONTROL_VERB_PERFREAD, "");
    check("PERFCLEAR", 1, SXNU_CONTROL_VERB_PERFCLEAR, "");
    check("GPUFREQ", 1, SXNU_CONTROL_VERB_GPUFREQ, "");
    check("GPUHWCG off", 1, SXNU_CONTROL_VERB_GPUHWCG, "off");
    check("GPUPROFILE", 1, SXNU_CONTROL_VERB_GPUPROFILE, "");
    check("GPUPROFILE status", 1, SXNU_CONTROL_VERB_GPUPROFILE, "status");
    check("GMUSTATE", 1, SXNU_CONTROL_VERB_GMUSTATE, "");
    check("DISPREGS", 1, SXNU_CONTROL_VERB_DISPREGS, "");
    check("FLIPNOW", 1, SXNU_CONTROL_VERB_FLIPNOW, "");
    check("SMMUDUMP", 1, SXNU_CONTROL_VERB_SMMUDUMP, "");
    check("DEBUGGER", 1, SXNU_CONTROL_VERB_DEBUGGER, "");
    check("KDPPUSH fa01", 1, SXNU_CONTROL_VERB_KDPPUSH, "fa01");
    check("PTDUMP 0xfffffff007004000", 1, SXNU_CONTROL_VERB_PTDUMP,
          "0xfffffff007004000");
    check("PTDUMP", 1, SXNU_CONTROL_VERB_PTDUMP, "");
    check("PANICDUMP", 1, SXNU_CONTROL_VERB_PANICDUMP, "");
    check("SYSCTL kern.maxproc", 1, SXNU_CONTROL_VERB_SYSCTL, "kern.maxproc");
    check("SYSCTL kern.maxproc 4", 1, SXNU_CONTROL_VERB_SYSCTL, "kern.maxproc 4");
    check("TASKDUMP", 1, SXNU_CONTROL_VERB_TASKDUMP, "");
    check("TASKDUMP 0xfffffff007004000", 1, SXNU_CONTROL_VERB_TASKDUMP, "0xfffffff007004000");
    check("THREADDUMP 0xfffffff007004000", 1, SXNU_CONTROL_VERB_THREADDUMP, "0xfffffff007004000");
    check("VSYNCON", 1, SXNU_CONTROL_VERB_VSYNCON, "");
    check("VSYNCON 60", 1, SXNU_CONTROL_VERB_VSYNCON, "60");
    check("BKPADD 0xfffffff007004000", 1, SXNU_CONTROL_VERB_BKPADD, "0xfffffff007004000");
    check("BKPADD 0xfffffff007004000 1", 1, SXNU_CONTROL_VERB_BKPADD, "0xfffffff007004000 1");
    check("BKPDEL 0xfffffff007004000", 1, SXNU_CONTROL_VERB_BKPDEL, "0xfffffff007004000");
    check("BKPLIST", 1, SXNU_CONTROL_VERB_BKPLIST, "");
    check("SYSREG TCR_EL1", 1, SXNU_CONTROL_VERB_SYSREG, "TCR_EL1");
    check("SYSREG MIDR_EL1", 1, SXNU_CONTROL_VERB_SYSREG, "MIDR_EL1");
    check("SYSREGW TTBR1_EL1 0xcae35000", 1, SXNU_CONTROL_VERB_SYSREGW,
          "TTBR1_EL1 0xcae35000");
    check("PANICDUMP info", 1, SXNU_CONTROL_VERB_PANICDUMP, "info");
    check("PANICDUMP log 0 100", 1, SXNU_CONTROL_VERB_PANICDUMP, "log 0 100");

    /* Unknown verbs stay inert. */
    check("BOGUS", 0, SXNU_CONTROL_VERB_INVALID, "");
    check("READX 1 2", 0, SXNU_CONTROL_VERB_INVALID, "");

    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
