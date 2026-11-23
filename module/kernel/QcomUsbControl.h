/* SPDX-License-Identifier: MIT
 * Bounded, allocation-free control framing shared by the kernel control
 * worker and its host-side conformance tests. This module never performs I/O
 * and contains no memory, function, or arbitrary register operations.
 */
#ifndef QCOM_USB_CONTROL_H
#define QCOM_USB_CONTROL_H

#include <stddef.h>
#include <stdint.h>

/* The newline is part of the 128-byte request-frame ceiling. */
#define SXNU_CONTROL_FRAME_BYTES 128u
#define SXNU_CONTROL_LINE_BYTES 127u
#define SXNU_CONTROL_REPLY_BYTES 512u
#define SXNU_CONTROL_WORK_BYTES 256u
#define SXNU_CONTROL_SESSION_HEX 16u
#define SXNU_CONTROL_SEQ_HEX 8u
#define SXNU_CONTROL_CRC_HEX 8u

enum sxnu_control_verb {
    SXNU_CONTROL_VERB_INVALID = 0,
    SXNU_CONTROL_VERB_HELLO,
    SXNU_CONTROL_VERB_PING,
    SXNU_CONTROL_VERB_STATUS,
    SXNU_CONTROL_VERB_REBOOT,
    SXNU_CONTROL_VERB_SCREENSHOT,
    SXNU_CONTROL_VERB_SHOTNEXT,
    /* Argument-bearing diagnostics. This layer still performs no memory
     * access: the verbs only carry text to QcomUsbDiag.c, reached from
     * control_handle_frame(). */
    SXNU_CONTROL_VERB_READ,       /* READ <hexva> <hexlen>    */
    SXNU_CONTROL_VERB_READNEXT,   /* READNEXT [<hexchars>]    */
    SXNU_CONTROL_VERB_WRITE,      /* WRITE <hexva> <hexbytes> */
    SXNU_CONTROL_VERB_SYM,        /* SYM <text>               */
    SXNU_CONTROL_VERB_SYMS,       /* SYMS [prefix]            */
    SXNU_CONTROL_VERB_READPA,     /* READPA <hexpa> <hexlen>  */
    SXNU_CONTROL_VERB_READPANEXT, /* READPANEXT [<hexchars>]  */
    SXNU_CONTROL_VERB_WRITEPA,    /* WRITEPA <hexpa> <hexbytes>, MMIO whitelist only */
    SXNU_CONTROL_VERB_BOOTARGS,   /* BOOTARGS [all] -> kernel command line         */
    SXNU_CONTROL_VERB_VERSION,    /* VERSION -> build/xnu/soc identity            */
    SXNU_CONTROL_VERB_DUMPUFS,    /* DUMPUFS <hexpa> <hexlen> <tag> -> UFS         */
    SXNU_CONTROL_VERB_DUMLIST,    /* DUMLIST -> queued/done slots                  */
    SXNU_CONTROL_VERB_UPTIME,     /* UPTIME -> mach ticks + 19.2MHz timebase      */
    SXNU_CONTROL_VERB_RDBLK,      /* RDBLK <name> <off> <len> -> named MMIO block */
    SXNU_CONTROL_VERB_PERFSTART,  /* PERFSTART <hexms> -> GPU profile window     */
    SXNU_CONTROL_VERB_PERFSTOP,   /* PERFSTOP -> stop the profile window         */
    SXNU_CONTROL_VERB_PERFREAD,   /* PERFREAD -> last window counter snapshot    */
    SXNU_CONTROL_VERB_PERFCLEAR,  /* PERFCLEAR -> drop the last snapshot         */
    SXNU_CONTROL_VERB_GPUFREQ,    /* GPUFREQ -> current GPU clock / vote          */
    SXNU_CONTROL_VERB_GPUHWCG,    /* GPUHWCG <on|off> -> runtime clock gating     */
    SXNU_CONTROL_VERB_GPUPROFILE,/* GPUPROFILE [status] -> profile window state  */
    SXNU_CONTROL_VERB_GMUSTATE,  /* GMUSTATE -> GPU boot step / fence / chip     */
    SXNU_CONTROL_VERB_DISPREGS,  /* DISPREGS -> display counters (driver arrays) */
    SXNU_CONTROL_VERB_FLIPNOW,   /* FLIPNOW -> flips / transfers / surfaces      */
    SXNU_CONTROL_VERB_SMMUDUMP,  /* SMMUDUMP -> display SMMU bank / CB state     */
    SXNU_CONTROL_VERB_DEBUGGER,  /* DEBUGGER -> enter the kernel debugger (KDP)  */
    SXNU_CONTROL_VERB_KDPPUSH,   /* KDPPUSH <hex> -> queue bytes for KDP (no trap) */
    SXNU_CONTROL_VERB_PTDUMP,    /* PTDUMP <hexva> -> stage-1 page-table walk */
    SXNU_CONTROL_VERB_PANICDUMP, /* PANICDUMP [info|log <off> <len>] -> panic area */
    SXNU_CONTROL_VERB_LAST        /* sentinel: one past the last verb */
};

struct sxnu_control_frame {
    uint64_t session;
    uint32_t seq;
    uint32_t crc;
    enum sxnu_control_verb verb;
    /* Argument text between the verb and the CRC; points into the caller's
     * line buffer and is not NUL-terminated. Zero length when absent. */
    const uint8_t *args;
    size_t arg_bytes;
};

struct sxnu_control_parser {
    uint8_t line[SXNU_CONTROL_FRAME_BYTES];
    uint16_t used;
    uint8_t discarding;
    uint32_t overlong;
};

struct sxnu_control_state {
    uint64_t session;
    uint32_t last_seq;
    uint32_t generation;
};

struct sxnu_control_writer {
    char *out;
    size_t capacity;
    size_t used;
    uint8_t failed;
};

static inline uint32_t
sxnu_control_crc32(const void *bytes, size_t count)
{
    const uint8_t *p = bytes;
    uint32_t crc = 0xffffffffu;
    size_t i;

    for (i = 0; i < count; i++) {
        unsigned int bit;
        crc ^= p[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static inline int
sxnu_control_hex_value(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline int
sxnu_control_parse_hex(const uint8_t *text, size_t digits, uint64_t *value)
{
    uint64_t result = 0;
    size_t i;

    if (!value || !digits || digits > 16u) return 0;
    for (i = 0; i < digits; i++) {
        int digit = sxnu_control_hex_value(text[i]);
        if (digit < 0) return 0;
        result = (result << 4) | (uint64_t)digit;
    }
    *value = result;
    return 1;
}

static inline enum sxnu_control_verb
sxnu_control_parse_verb(const uint8_t *text, size_t bytes)
{
    if (bytes == 5u && text[0] == 'H' && text[1] == 'E' && text[2] == 'L' &&
        text[3] == 'L' && text[4] == 'O') return SXNU_CONTROL_VERB_HELLO;
    if (bytes == 4u && text[0] == 'P' && text[1] == 'I' && text[2] == 'N' &&
        text[3] == 'G') return SXNU_CONTROL_VERB_PING;
    if (bytes == 6u && text[0] == 'S' && text[1] == 'T' && text[2] == 'A' &&
        text[3] == 'T' && text[4] == 'U' && text[5] == 'S')
        return SXNU_CONTROL_VERB_STATUS;
    if (bytes == 6u && text[0] == 'R' && text[1] == 'E' && text[2] == 'B' &&
        text[3] == 'O' && text[4] == 'O' && text[5] == 'T')
        return SXNU_CONTROL_VERB_REBOOT;
    if (bytes == 10u && text[0] == 'S' && text[1] == 'C' && text[2] == 'R' &&
        text[3] == 'E' && text[4] == 'E' && text[5] == 'N' && text[6] == 'S' &&
        text[7] == 'H' && text[8] == 'O' && text[9] == 'T') return SXNU_CONTROL_VERB_SCREENSHOT;
    if (bytes == 8u && text[0] == 'S' && text[1] == 'H' && text[2] == 'O' &&
        text[3] == 'T' && text[4] == 'N' && text[5] == 'E' && text[6] == 'X' &&
        text[7] == 'T') return SXNU_CONTROL_VERB_SHOTNEXT;
    if (bytes == 4u && text[0] == 'R' && text[1] == 'E' && text[2] == 'A' &&
        text[3] == 'D') return SXNU_CONTROL_VERB_READ;
    if (bytes == 8u && text[0] == 'R' && text[1] == 'E' && text[2] == 'A' &&
        text[3] == 'D' && text[4] == 'N' && text[5] == 'E' && text[6] == 'X' &&
        text[7] == 'T') return SXNU_CONTROL_VERB_READNEXT;
    if (bytes == 5u && text[0] == 'W' && text[1] == 'R' && text[2] == 'I' &&
        text[3] == 'T' && text[4] == 'E') return SXNU_CONTROL_VERB_WRITE;
    if (bytes == 3u && text[0] == 'S' && text[1] == 'Y' && text[2] == 'M')
        return SXNU_CONTROL_VERB_SYM;
    if (bytes == 4u && text[0] == 'S' && text[1] == 'Y' && text[2] == 'M' &&
        text[3] == 'S') return SXNU_CONTROL_VERB_SYMS;
    if (bytes == 6u && text[0] == 'R' && text[1] == 'E' && text[2] == 'A' &&
        text[3] == 'D' && text[4] == 'P' && text[5] == 'A') return SXNU_CONTROL_VERB_READPA;
    if (bytes == 10u && text[0] == 'R' && text[1] == 'E' && text[2] == 'A' &&
        text[3] == 'D' && text[4] == 'P' && text[5] == 'A' && text[6] == 'N' &&
        text[7] == 'E' && text[8] == 'X' && text[9] == 'T') return SXNU_CONTROL_VERB_READPANEXT;
    if (bytes == 7u && text[0] == 'W' && text[1] == 'R' && text[2] == 'I' &&
        text[3] == 'T' && text[4] == 'E' && text[5] == 'P' && text[6] == 'A')
        return SXNU_CONTROL_VERB_WRITEPA;
    if (bytes == 8u && text[0] == 'B' && text[1] == 'O' && text[2] == 'O' &&
        text[3] == 'T' && text[4] == 'A' && text[5] == 'R' && text[6] == 'G' &&
        text[7] == 'S') return SXNU_CONTROL_VERB_BOOTARGS;
    if (bytes == 7u && text[0] == 'V' && text[1] == 'E' && text[2] == 'R' &&
        text[3] == 'S' && text[4] == 'I' && text[5] == 'O' && text[6] == 'N')
        return SXNU_CONTROL_VERB_VERSION;
    if (bytes == 7u && text[0] == 'D' && text[1] == 'U' && text[2] == 'M' &&
        text[3] == 'P' && text[4] == 'U' && text[5] == 'F' && text[6] == 'S')
        return SXNU_CONTROL_VERB_DUMPUFS;
    if (bytes == 7u && text[0] == 'D' && text[1] == 'U' && text[2] == 'M' &&
        text[3] == 'L' && text[4] == 'I' && text[5] == 'S' && text[6] == 'T')
        return SXNU_CONTROL_VERB_DUMLIST;
    /* kb-40: accept DUMPLIST as well as DUMLIST.
     *
     * Every other dump verb is DUMP*, so "dumplist" is what people and scripts
     * type; the console upper-cases whatever it is given and sends it verbatim,
     * so an 8-character DUMPLIST reached this parser and matched nothing.
     *
     * The failure was silent and unrecoverable, which is what made it look like a
     * hang: parse returns 0 -> control_service counts control_rejected and never
     * calls control_handle_frame -> no reply, and state->last_seq does not
     * advance. The console increments its own seq on every attempt, so the two
     * drift apart by one per failure and every later command is refused too.
     * Measured on kb-38: dumlist (7 chars) answered OK immediately, while
     * dumplist (8 chars) timed out and left last_seq stuck at 22 (read live from
     * 0xfffffff00c81e098) -- only HELLO recovered, because it does not consult
     * last_seq. */
    if (bytes == 8u && text[0] == 'D' && text[1] == 'U' && text[2] == 'M' &&
        text[3] == 'P' && text[4] == 'L' && text[5] == 'I' && text[6] == 'S' &&
        text[7] == 'T')
        return SXNU_CONTROL_VERB_DUMLIST;
    if (bytes == 6u && text[0] == 'U' && text[1] == 'P' && text[2] == 'T' &&
        text[3] == 'I' && text[4] == 'M' && text[5] == 'E')
        return SXNU_CONTROL_VERB_UPTIME;
    if (bytes == 5u && text[0] == 'R' && text[1] == 'D' && text[2] == 'B' &&
        text[3] == 'L' && text[4] == 'K')
        return SXNU_CONTROL_VERB_RDBLK;
    if (bytes == 9u && text[0] == 'P' && text[1] == 'E' && text[2] == 'R' &&
        text[3] == 'F' && text[4] == 'S' && text[5] == 'T' && text[6] == 'A' &&
        text[7] == 'R' && text[8] == 'T')
        return SXNU_CONTROL_VERB_PERFSTART;
    if (bytes == 8u && text[0] == 'P' && text[1] == 'E' && text[2] == 'R' &&
        text[3] == 'F' && text[4] == 'S' && text[5] == 'T' && text[6] == 'O' &&
        text[7] == 'P') return SXNU_CONTROL_VERB_PERFSTOP;
    if (bytes == 8u && text[0] == 'P' && text[1] == 'E' && text[2] == 'R' &&
        text[3] == 'F' && text[4] == 'R' && text[5] == 'E' && text[6] == 'A' &&
        text[7] == 'D') return SXNU_CONTROL_VERB_PERFREAD;
    if (bytes == 9u && text[0] == 'P' && text[1] == 'E' && text[2] == 'R' &&
        text[3] == 'F' && text[4] == 'C' && text[5] == 'L' && text[6] == 'E' &&
        text[7] == 'A' && text[8] == 'R') return SXNU_CONTROL_VERB_PERFCLEAR;
    if (bytes == 7u && text[0] == 'G' && text[1] == 'P' && text[2] == 'U' &&
        text[3] == 'F' && text[4] == 'R' && text[5] == 'E' && text[6] == 'Q')
        return SXNU_CONTROL_VERB_GPUFREQ;
    if (bytes == 7u && text[0] == 'G' && text[1] == 'P' && text[2] == 'U' &&
        text[3] == 'H' && text[4] == 'W' && text[5] == 'C' && text[6] == 'G')
        return SXNU_CONTROL_VERB_GPUHWCG;
    if (bytes == 10u && text[0] == 'G' && text[1] == 'P' && text[2] == 'U' &&
        text[3] == 'P' && text[4] == 'R' && text[5] == 'O' && text[6] == 'F' &&
        text[7] == 'I' && text[8] == 'L' && text[9] == 'E')
        return SXNU_CONTROL_VERB_GPUPROFILE;
    if (bytes == 8u && text[0] == 'G' && text[1] == 'M' && text[2] == 'U' &&
        text[3] == 'S' && text[4] == 'T' && text[5] == 'A' && text[6] == 'T' &&
        text[7] == 'E') return SXNU_CONTROL_VERB_GMUSTATE;
    if (bytes == 8u && text[0] == 'D' && text[1] == 'I' && text[2] == 'S' &&
        text[3] == 'P' && text[4] == 'R' && text[5] == 'E' && text[6] == 'G' &&
        text[7] == 'S') return SXNU_CONTROL_VERB_DISPREGS;
    if (bytes == 7u && text[0] == 'F' && text[1] == 'L' && text[2] == 'I' &&
        text[3] == 'P' && text[4] == 'N' && text[5] == 'O' && text[6] == 'W')
        return SXNU_CONTROL_VERB_FLIPNOW;
    if (bytes == 8u && text[0] == 'S' && text[1] == 'M' && text[2] == 'M' &&
        text[3] == 'U' && text[4] == 'D' && text[5] == 'U' && text[6] == 'M' &&
        text[7] == 'P') return SXNU_CONTROL_VERB_SMMUDUMP;
    if (bytes == 8u && text[0] == 'D' && text[1] == 'E' && text[2] == 'B' &&
        text[3] == 'U' && text[4] == 'G' && text[5] == 'G' && text[6] == 'E' &&
        text[7] == 'R') return SXNU_CONTROL_VERB_DEBUGGER;
    if (bytes == 7u && text[0] == 'K' && text[1] == 'D' && text[2] == 'P' &&
        text[3] == 'P' && text[4] == 'U' && text[5] == 'S' && text[6] == 'H')
        return SXNU_CONTROL_VERB_KDPPUSH;
    /* PTDUMP is deliberately AFTER KDPPUSH: both are 6-7 characters and the
     * comparison is explicit, so order does not matter for correctness -- it is
     * grouped with the other diagnostics verbs by intent, not by prefix. */
    if (bytes == 6u && text[0] == 'P' && text[1] == 'T' && text[2] == 'D' &&
        text[3] == 'U' && text[4] == 'M' && text[5] == 'P')
        return SXNU_CONTROL_VERB_PTDUMP;
    if (bytes == 9u && text[0] == 'P' && text[1] == 'A' && text[2] == 'N' &&
        text[3] == 'I' && text[4] == 'C' && text[5] == 'D' && text[6] == 'U' &&
        text[7] == 'M' && text[8] == 'P') return SXNU_CONTROL_VERB_PANICDUMP;
    return SXNU_CONTROL_VERB_INVALID;
}

static inline const char *
sxnu_control_verb_name(enum sxnu_control_verb verb)
{
    switch (verb) {
    case SXNU_CONTROL_VERB_HELLO: return "HELLO";
    case SXNU_CONTROL_VERB_PING: return "PING";
    case SXNU_CONTROL_VERB_STATUS: return "STATUS";
    case SXNU_CONTROL_VERB_REBOOT: return "REBOOT";
    case SXNU_CONTROL_VERB_SCREENSHOT: return "SCREENSHOT";
    case SXNU_CONTROL_VERB_SHOTNEXT: return "SHOTNEXT";
    case SXNU_CONTROL_VERB_READ: return "READ";
    case SXNU_CONTROL_VERB_READNEXT: return "READNEXT";
    case SXNU_CONTROL_VERB_WRITE: return "WRITE";
    case SXNU_CONTROL_VERB_SYM: return "SYM";
    case SXNU_CONTROL_VERB_SYMS: return "SYMS";
    case SXNU_CONTROL_VERB_READPA: return "READPA";
    case SXNU_CONTROL_VERB_READPANEXT: return "READPANEXT";
    case SXNU_CONTROL_VERB_WRITEPA: return "WRITEPA";
    case SXNU_CONTROL_VERB_BOOTARGS: return "BOOTARGS";
    case SXNU_CONTROL_VERB_VERSION: return "VERSION";
    case SXNU_CONTROL_VERB_DUMPUFS: return "DUMPUFS";
    case SXNU_CONTROL_VERB_DUMLIST: return "DUMLIST";
    case SXNU_CONTROL_VERB_UPTIME: return "UPTIME";
    case SXNU_CONTROL_VERB_RDBLK: return "RDBLK";
    case SXNU_CONTROL_VERB_PERFSTART: return "PERFSTART";
    case SXNU_CONTROL_VERB_PERFSTOP: return "PERFSTOP";
    case SXNU_CONTROL_VERB_PERFREAD: return "PERFREAD";
    case SXNU_CONTROL_VERB_PERFCLEAR: return "PERFCLEAR";
    case SXNU_CONTROL_VERB_GPUFREQ: return "GPUFREQ";
    case SXNU_CONTROL_VERB_GPUHWCG: return "GPUHWCG";
    case SXNU_CONTROL_VERB_GPUPROFILE: return "GPUPROFILE";
    case SXNU_CONTROL_VERB_GMUSTATE: return "GMUSTATE";
    case SXNU_CONTROL_VERB_DISPREGS: return "DISPREGS";
    case SXNU_CONTROL_VERB_FLIPNOW: return "FLIPNOW";
    case SXNU_CONTROL_VERB_SMMUDUMP: return "SMMUDUMP";
    case SXNU_CONTROL_VERB_DEBUGGER: return "DEBUGGER";
    case SXNU_CONTROL_VERB_KDPPUSH: return "KDPPUSH";
    case SXNU_CONTROL_VERB_PTDUMP: return "PTDUMP";
    case SXNU_CONTROL_VERB_PANICDUMP: return "PANICDUMP";
    default: return "";
    }
}

static inline void
sxnu_control_parser_reset(struct sxnu_control_parser *parser)
{
    if (!parser) return;
    parser->used = 0;
    parser->discarding = 0;
}

/* Feed one byte. Returns 1 for a complete line, 0 while waiting, and -1 when
 * an overlong line was discarded through its newline. The parser has no
 * allocation and never interprets a partial frame. */
static inline int
sxnu_control_parser_feed(struct sxnu_control_parser *parser, uint8_t byte,
                         char *line, size_t line_capacity, size_t *line_bytes)
{
    if (!parser || !line || !line_bytes || line_capacity == 0u) return -1;
    if (byte == (uint8_t)'\n') {
        size_t bytes;
        if (parser->discarding) {
            parser->discarding = 0;
            parser->used = 0;
            parser->overlong++;
            return -1;
        }
        bytes = parser->used;
        parser->used = 0;
        if (!bytes) return 0;
        if (bytes >= line_capacity) {
            parser->overlong++;
            return -1;
        }
        for (size_t i = 0; i < bytes; i++) line[i] = (char)parser->line[i];
        line[bytes] = 0;
        *line_bytes = bytes;
        return 1;
    }
    if (parser->discarding) return 0;
    if (parser->used >= SXNU_CONTROL_LINE_BYTES) {
        parser->discarding = 1;
        parser->used = 0;
        return 0;
    }
    parser->line[parser->used++] = byte;
    return 0;
}

/* Strict grammar:
 * SXNU1 <16hex session> <8hex seq> <HELLO|PING|STATUS|REBOOT|SCREENSHOT|SHOTNEXT> <8hex crc>
 * CRC-32/IEEE covers every byte before the space immediately before the CRC. */
static inline int
sxnu_control_parse(const char *line, size_t bytes, struct sxnu_control_frame *frame)
{
    const uint8_t *p = (const uint8_t *)line;
    size_t cursor = 5u, verb_start, verb_bytes, crc_start;
    uint64_t session, seq, crc;
    enum sxnu_control_verb verb;

    if (!line || !frame || bytes < 6u || bytes > SXNU_CONTROL_LINE_BYTES) return 0;
    if (p[0] != 'S' || p[1] != 'X' || p[2] != 'N' || p[3] != 'U' || p[4] != '1' ||
        p[cursor++] != (uint8_t)' ') return 0;
    if (cursor > bytes || bytes - cursor < SXNU_CONTROL_SESSION_HEX) return 0;
    if (!sxnu_control_parse_hex(p + cursor, SXNU_CONTROL_SESSION_HEX, &session)) return 0;
    cursor += SXNU_CONTROL_SESSION_HEX;
    if (cursor >= bytes || p[cursor++] != (uint8_t)' ') return 0;
    if (cursor > bytes || bytes - cursor < SXNU_CONTROL_SEQ_HEX) return 0;
    if (!sxnu_control_parse_hex(p + cursor, SXNU_CONTROL_SEQ_HEX, &seq)) return 0;
    cursor += SXNU_CONTROL_SEQ_HEX;
    if (cursor >= bytes || p[cursor++] != (uint8_t)' ') return 0;

    /* The CRC is always the last 8 hex digits, preceded by a space, so it
     * is located before the verb is split off. Whatever lies between the
     * verb and that space is the argument text; with none present the verb
     * span is exactly what the argument-free grammar used to accept. */
    if (bytes < SXNU_CONTROL_CRC_HEX + 2u) return 0;
    crc_start = bytes - SXNU_CONTROL_CRC_HEX;
    if (p[crc_start - 1u] != (uint8_t)' ') return 0;
    if (!sxnu_control_parse_hex(p + crc_start, SXNU_CONTROL_CRC_HEX, &crc)) return 0;
    if ((uint32_t)crc != sxnu_control_crc32(line, crc_start - 1u)) return 0;
    verb_start = cursor;
    while (cursor < bytes && p[cursor] != (uint8_t)' ') cursor++;
    if (cursor == verb_start || cursor >= bytes) return 0;
    verb_bytes = cursor - verb_start;
    verb = sxnu_control_parse_verb(p + verb_start, verb_bytes);
    if (verb == SXNU_CONTROL_VERB_INVALID) return 0;
    {
        /* cursor sits on the space that follows the verb. Without arguments
         * that space is the one before the CRC, so cursor+1 == crc_start. */
        const size_t arg_start = cursor + 1u;
        if (arg_start < crc_start) {
            frame->args = p + arg_start;
            frame->arg_bytes = crc_start - 1u - arg_start;
        } else {
            frame->args = NULL;
            frame->arg_bytes = 0u;
        }
    }

    frame->session = session;
    frame->seq = (uint32_t)seq;
    frame->crc = (uint32_t)crc;
    frame->verb = verb;
    return 1;
}

static inline void
sxnu_control_state_reset(struct sxnu_control_state *state, uint64_t session,
                         uint32_t generation)
{
    if (!state) return;
    state->session = session;
    state->last_seq = 0;
    state->generation = generation;
}

/* HELLO is read-only and valid only with session zero and sequence zero. All
 * other commands require the current nonzero session and exactly the next
 * sequence number. This function has no side effects. */
static inline enum sxnu_control_verb
sxnu_control_accept(const struct sxnu_control_state *state,
                    const struct sxnu_control_frame *frame)
{
    if (!state || !frame) return SXNU_CONTROL_VERB_INVALID;
    if (frame->verb < SXNU_CONTROL_VERB_HELLO ||
        frame->verb >= SXNU_CONTROL_VERB_LAST) return SXNU_CONTROL_VERB_INVALID;
    if (frame->verb == SXNU_CONTROL_VERB_HELLO)
        return frame->session == 0 && frame->seq == 0 ?
               SXNU_CONTROL_VERB_HELLO : SXNU_CONTROL_VERB_INVALID;
    if (!state->session || frame->seq == 0 || frame->session != state->session ||
        state->last_seq == UINT32_MAX ||
        frame->seq != state->last_seq + 1u) return SXNU_CONTROL_VERB_INVALID;
    return frame->verb;
}

static inline int
sxnu_control_commit(struct sxnu_control_state *state,
                    const struct sxnu_control_frame *frame,
                    enum sxnu_control_verb accepted)
{
    if (!state || !frame || accepted < SXNU_CONTROL_VERB_HELLO ||
        accepted >= SXNU_CONTROL_VERB_LAST || frame->verb != accepted) return 0;
    if (accepted == SXNU_CONTROL_VERB_HELLO)
        return frame->session == 0 && frame->seq == 0;
    if (!state->session || frame->seq == 0 || frame->session != state->session ||
        state->last_seq == UINT32_MAX ||
        frame->seq != state->last_seq + 1u) return 0;
    state->last_seq = frame->seq;
    return 1;
}

static inline void
sxnu_control_writer_init(struct sxnu_control_writer *writer, char *out, size_t capacity)
{
    writer->out = out;
    writer->capacity = out ? capacity : 0;
    writer->used = 0;
    writer->failed = writer->capacity == 0u;
    if (out && capacity) out[0] = 0;
}

static inline int
sxnu_control_writer_put(struct sxnu_control_writer *writer, const char *text,
                        size_t bytes)
{
    size_t i;
    if (!writer || writer->failed || !text) return 0;
    if (bytes > writer->capacity - writer->used) {
        writer->failed = 1;
        return 0;
    }
    for (i = 0; i < bytes; i++) writer->out[writer->used + i] = text[i];
    writer->used += bytes;
    if (writer->used < writer->capacity) writer->out[writer->used] = 0;
    return 1;
}

static inline int
sxnu_control_writer_text(struct sxnu_control_writer *writer, const char *text)
{
    size_t bytes = 0;
    if (!text) return 0;
    /* kb-39: bound the scan. This runs inside control_handle_frame(), which holds
     * the USB guard with interrupts masked, so an unterminated string here is not
     * a cosmetic bug: the loop walks memory until it happens to hit a zero byte.
     *
     * Measured on hardware (kb-38): DUMPLIST stopped answering, and every verb
     * after it timed out as well, while the kernel kept logging normally. The
     * caller passes slot->tag, a uint8_t[16] documented as "NUL-padded" -- but a
     * full 16-byte tag has no terminator, and writer_put() cannot help because it
     * only sees the length after this loop has already run.
     *
     * A reply is capped at SXNU_CONTROL_REPLY_BYTES anyway, so a longer string
     * could never be delivered; failing fast is strictly better than scanning the
     * kernel image while holding the USB lock. */
    while (bytes < writer->capacity && text[bytes]) bytes++;
    if (bytes == writer->capacity) {
        writer->failed = 1;
        return 0;
    }
    return sxnu_control_writer_put(writer, text, bytes);
}

static inline int
sxnu_control_writer_char(struct sxnu_control_writer *writer, char value)
{
    return sxnu_control_writer_put(writer, &value, 1u);
}

static inline int
sxnu_control_writer_hex(struct sxnu_control_writer *writer, uint64_t value,
                        unsigned int digits)
{
    static const char alphabet[] = "0123456789abcdef";
    char text[16];
    unsigned int i;
    if (!writer || writer->failed || digits > 16u) return 0;
    for (i = 0; i < digits; i++) {
        text[digits - i - 1u] = alphabet[value & 15u];
        value >>= 4;
    }
    return sxnu_control_writer_put(writer, text, digits);
}

static inline int
sxnu_control_writer_decimal(struct sxnu_control_writer *writer, uint64_t value)
{
    char text[20];
    unsigned int used = 0;
    do {
        text[used++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    for (unsigned int i = 0; i < used / 2u; i++) {
        char swap = text[i];
        text[i] = text[used - i - 1u];
        text[used - i - 1u] = swap;
    }
    return sxnu_control_writer_put(writer, text, used);
}

static inline int
sxnu_control_writer_signed(struct sxnu_control_writer *writer, int64_t value)
{
    uint64_t magnitude;
    if (!writer) return 0;
    if (value < 0) {
        if (!sxnu_control_writer_char(writer, '-')) return 0;
        magnitude = (uint64_t)(-(value + 1)) + 1u;
    } else {
        magnitude = (uint64_t)value;
    }
    return sxnu_control_writer_decimal(writer, magnitude);
}

static inline int
sxnu_control_reply_start(struct sxnu_control_writer *writer, uint64_t session,
                         uint32_t seq, enum sxnu_control_verb verb,
                         const char *status)
{
    if (!writer || verb == SXNU_CONTROL_VERB_INVALID || !status) return 0;
    return sxnu_control_writer_text(writer, "SXNU1R ") &&
           sxnu_control_writer_hex(writer, session, SXNU_CONTROL_SESSION_HEX) &&
           sxnu_control_writer_char(writer, ' ') &&
           sxnu_control_writer_hex(writer, seq, SXNU_CONTROL_SEQ_HEX) &&
           sxnu_control_writer_char(writer, ' ') &&
           sxnu_control_writer_text(writer, sxnu_control_verb_name(verb)) &&
           sxnu_control_writer_char(writer, ' ') &&
           sxnu_control_writer_text(writer, status);
}

/* Append the CRC and newline. Returns the complete reply byte count including
 * newline, or zero if the fixed 512-byte ceiling would be exceeded. */
static inline size_t
sxnu_control_reply_finish(struct sxnu_control_writer *writer)
{
    uint32_t crc;
    if (!writer || writer->failed) return 0;
    crc = sxnu_control_crc32(writer->out, writer->used);
    if (!sxnu_control_writer_char(writer, ' ') ||
        !sxnu_control_writer_hex(writer, crc, SXNU_CONTROL_CRC_HEX) ||
        !sxnu_control_writer_char(writer, '\n')) return 0;
    return writer->used;
}

#endif /* QCOM_USB_CONTROL_H */
