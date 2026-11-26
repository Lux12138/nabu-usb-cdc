/* SPDX-License-Identifier: MIT
 * Offline self-check for the T140 forensics verbs (DEVTREE / FBRAW /
 * NABULOGRING). Runs on the host with no device: it exercises the pure parsing
 * and planning layer (QcomUsbForensics.h) plus the SXNU1 verb registration,
 * and models the frame boundaries the wire format imposes.
 */
#include "kernel/QcomUsbControl.h"
#include "kernel/QcomUsbForensics.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void
test_nabulog_header(void)
{
    uint8_t hdr[QUD_NABULOG_HEADER_BYTES];
    struct qud_nabulog_header h;
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, QUD_NABULOG_MAGIC, QUD_NABULOG_MAGIC_BYTES);
    hdr[8] = 2;                    /* version = 2 (le) */
    hdr[12] = 0;                   /* is_panic = 0 */
    hdr[16] = 0x58, hdr[17] = 0x14; /* available = 0x1458 = 5208 */
    hdr[20] = 1;                   /* dropped = 1 */
    hdr[24] = 7; hdr[28] = 0;      /* total = 7 */
    assert(qud_nabulog_header_parse(hdr, sizeof(hdr), &h) == 1);
    assert(h.version == 2 && h.is_panic == 0);
    assert(h.available == 5208 && h.dropped == 1);
    assert(h.total_lo == 7 && h.total_hi == 0);

    /* Short buffer is a length error, not a magic error. */
    assert(qud_nabulog_header_parse(hdr, QUD_NABULOG_HEADER_BYTES - 1u, &h) == 0);
    /* Corrupt magic. */
    hdr[0] = 'X';
    assert(qud_nabulog_header_parse(hdr, sizeof(hdr), &h) == -1);
}

static void
test_nabulog_window(void)
{
    uint64_t want = 999;
    /* Normal clamp to available. */
    assert(qud_nabulog_body_window(32, 1000, 5208, &want) == 1 && want == 1000);
    assert(qud_nabulog_body_window(5000, 1000, 5208, &want) == 1 && want == 208);
    /* Empty window at the exact end is legal. */
    assert(qud_nabulog_body_window(5208, 64, 5208, &want) == 1 && want == 0);
    /* Beyond available is a hard error. */
    assert(qud_nabulog_body_window(5209, 64, 5208, &want) == 0);
    /* NULL out is refused. */
    assert(qud_nabulog_body_window(0, 1, 1, NULL) == 0);
}

static void
test_nabulog_geometry(void)
{
    uint64_t rel = 999;
    assert(qud_nabulog_slot_rel(8192, 0, &rel) == 1 && rel == 0);
    assert(qud_nabulog_slot_rel(8192, 1, &rel) == 1 && rel == 8190);
    assert(qud_nabulog_slot_rel(1, 1, &rel) == 0);
    /* Live anchors measured on device: first=121718715, blocks=8192. */
    assert(qud_nabulog_slot_rel(8192, 1, &rel) == 1 &&
           121718715u + rel == 121726905u);
}

static void
test_fbraw_plan(void)
{
    uint32_t chunk = 0;
    int last = 0;
    assert(qud_fbraw_plan(0, 1000, 256, &chunk, &last) == 1 &&
           chunk == 256 && last == 0);
    assert(qud_fbraw_plan(744, 1000, 256, &chunk, &last) == 1 &&
           chunk == 256 && last == 1);
    assert(qud_fbraw_plan(1000, 1000, 256, &chunk, &last) == 1 &&
           chunk == 0 && last == 1);
    assert(qud_fbraw_plan(1001, 1000, 256, &chunk, &last) == 0);
    assert(qud_fbraw_plan(0, 1000, 0, &chunk, &last) == 0);
    /* The default chunk keeps an SXNU1R reply inside the 512-byte cap:
     * header + 2 hex chars per byte must stay below SXNU_CONTROL_REPLY_BYTES. */
    assert(2u * 128u + 160u < SXNU_CONTROL_REPLY_BYTES);
}

static void
test_devtree_args(void)
{
    assert(qud_devtree_path_valid("/", 64));
    assert(qud_devtree_path_valid("/arm-io/intc", 64));
    assert(!qud_devtree_path_valid("", 64));
    assert(!qud_devtree_path_valid("arm-io", 64));
    assert(!qud_devtree_path_valid("/a b", 64));
    assert(!qud_devtree_path_valid(NULL, 64));
    assert(qud_path_or_prefix_match("/arm-io", "/"));
    assert(qud_path_or_prefix_match("/arm-io/intc", "/arm-io"));
    assert(qud_path_or_prefix_match("/arm-io", "/arm-io"));
    assert(!qud_path_or_prefix_match("/arm-io2", "/arm-io"));
    assert(!qud_path_or_prefix_match("/arm", "/arm-io"));
}

static void
test_verb_registration(void)
{
    const uint8_t *t = (const uint8_t *)"DEVTREE";
    assert(sxnu_control_parse_verb(t, 7) == SXNU_CONTROL_VERB_DEVTREE);
    /* The wire token is the whole word; a 6-byte prefix must NOT match. */
    assert(sxnu_control_parse_verb(t, 6) == SXNU_CONTROL_VERB_INVALID);
    assert(sxnu_control_parse_verb((const uint8_t *)"FBRAW", 5) ==
           SXNU_CONTROL_VERB_FBRAW);
    assert(sxnu_control_parse_verb((const uint8_t *)"NABULOGRING", 11) ==
           SXNU_CONTROL_VERB_NABULOGRING);
    /* Neighbouring verbs are not captured by a prefix match. */
    assert(sxnu_control_parse_verb((const uint8_t *)"DEV", 3) ==
           SXNU_CONTROL_VERB_INVALID);
    assert(sxnu_control_parse_verb((const uint8_t *)"NABULOG", 7) ==
           SXNU_CONTROL_VERB_INVALID);
    assert(strcmp(sxnu_control_verb_name(SXNU_CONTROL_VERB_DEVTREE),
                  "DEVTREE") == 0);
    assert(strcmp(sxnu_control_verb_name(SXNU_CONTROL_VERB_FBRAW), "FBRAW") == 0);
    assert(strcmp(sxnu_control_verb_name(SXNU_CONTROL_VERB_NABULOGRING),
                  "NABULOGRING") == 0);
    /* The accept range covers the new verbs. */
    assert(SXNU_CONTROL_VERB_DEVTREE > SXNU_CONTROL_VERB_HELLO);
    assert(SXNU_CONTROL_VERB_NABULOGRING < SXNU_CONTROL_VERB_LAST);
}

int main(void)
{
    test_nabulog_header();
    test_nabulog_window();
    test_nabulog_geometry();
    test_fbraw_plan();
    test_devtree_args();
    test_verb_registration();
    puts("forensics-verbs: PASS");
    return 0;
}
