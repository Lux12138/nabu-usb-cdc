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
