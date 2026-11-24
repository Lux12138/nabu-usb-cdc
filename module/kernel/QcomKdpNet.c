/* SPDX-License-Identifier: MIT
 * nabu: optional KDP transport over the shared USB CDC byte stream.
 *
 * Default off.  With boot-arg "nabu-kdp-net=1" and the native USB core ACTIVE,
 * this module replaces the serial-KDP transport callbacks with a pair of raw
 * Ethernet-frame callbacks and frames them on the same CDC stream using KDPF
 * v1 -- the format already implemented and self-tested by the host bridge
 * (answers/kdp_net_bridge/kdp_cdc_frame.py):
 *
 *   offset size field
 *   0      4    magic   = "KDPF"
 *   4      1    version = 0x01
 *   5      1    flags   = 0x00 (reserved)
 *   6      2    length  = payload length, little-endian (1..1518)
 *   8      4    crc32   = zlib CRC-32(payload), little-endian
 *   12     N    payload = complete Ethernet frame (14+20+8+KDP)
 *
 * No escaping.  XNU kdp_send()/kdp_reply() hand the transport a complete
 * Ethernet frame unconditionally (osfmk/kdp/kdp_udp.c:544-687), so the payload
 * is exactly what the UDP transport would have carried.  1518 mirrors
 * KDP_MAXPACKET (kdp_udp.c:219-222).
 *
 * Default-off equivalence: with the boot-arg absent, try_attach() records
 * net_disabled once and every later call returns before touching any kdp_*
 * symbol; qcom_usb_debug_kdp_net_route() then hands serial_rx_drain() the
 * original chunk pointer and length.  No other kernel behaviour changes.
 */
#include "QcomXnuUsbDebug.h"
#include <stddef.h>
#include <stdint.h>
#include <pexpert/pexpert.h>

#define NABU_KDPF_HDR_BYTES  12u
#define NABU_KDPF_ETH_MAX    1518u
#define NABU_KDPF_Q_SLOTS    4u
#define NABU_KDPF_PASS_BYTES 256u     /* one drain chunk: SXNU_CONTROL_WORK_BYTES */
#define NABU_KDPF_IDLE_LIMIT 40u      /* ~200 ms at the 5 ms worker tick */

enum { NET_SCAN_IDLE = 0, NET_SCAN_HEADER = 1, NET_SCAN_PAYLOAD = 2 };

static const uint8_t net_magic[4] = { 'K', 'D', 'P', 'F' };

static volatile uint32_t net_mode;       /* 0 = default off, 1 = attached */
static volatile uint32_t net_disabled;   /* boot-arg evaluated once and denied */

static uint32_t net_scan;
static uint32_t net_magic_len;
static uint32_t net_hdr_len;
static uint32_t net_payload_len;
static uint32_t net_payload_got;
static uint32_t net_want_crc;
static uint32_t net_gen;
static uint32_t net_idle_calls;
static uint8_t  net_hdr[NABU_KDPF_HDR_BYTES];
static uint8_t  net_payload[NABU_KDPF_ETH_MAX];
static uint8_t  net_pass[NABU_KDPF_PASS_BYTES];
static size_t   net_pass_len;

static uint8_t  net_q[NABU_KDPF_Q_SLOTS][NABU_KDPF_ETH_MAX];
static uint16_t net_q_len[NABU_KDPF_Q_SLOTS];
static uint32_t net_q_head, net_q_tail, net_q_count;

static uint32_t net_bad_headers, net_crc_errors, net_oversize_drops, net_resyncs;
static uint32_t net_q_overflow, net_tx_drop;

/* Local prototypes: kdp_en_debugger.h/kdp_udp.h are not exported to this
 * module, and kdp_udp.c is the only definition site.  The signatures below are
 * ABI-identical to kdp_en_debugger.h:36-41. */
typedef void (*qcom_kdp_send_fn)(void *, unsigned int);
typedef void (*qcom_kdp_recv_fn)(void *, unsigned int *, unsigned int);
extern void (*kdp_en_send_pkt)(void *, unsigned int);
extern void kdp_register_send_receive(qcom_kdp_send_fn send, qcom_kdp_recv_fn receive);
extern void kdp_register_link(void *link, void *mode);
extern void kdp_set_ip_and_mac_addresses(void *ipaddr, void *macaddr);
extern unsigned long z_crc32(unsigned long crc, const unsigned char *buf, unsigned len);

static uint32_t
net_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void
net_copy(uint8_t *dst, const uint8_t *src, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];
}

static void
net_emit(uint8_t b)
{
    if (net_pass_len < sizeof(net_pass)) net_pass[net_pass_len++] = b;
}

static void
net_reset(uint32_t generation)
{
    net_scan = NET_SCAN_IDLE;
    net_magic_len = 0;
    net_hdr_len = 0;
    net_payload_len = 0;
    net_payload_got = 0;
    net_want_crc = 0;
    net_idle_calls = 0;
    net_gen = generation;
}

static void
net_q_push(const uint8_t *p, uint32_t len)
{
    uint64_t daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #3" : "=r"(daif) :: "memory");
    if (len == 0 || len > NABU_KDPF_ETH_MAX || net_q_count >= NABU_KDPF_Q_SLOTS) {
        net_q_overflow++;
    } else {
        net_copy(net_q[net_q_head], p, len);
        net_q_len[net_q_head] = (uint16_t)len;
        net_q_head = (net_q_head + 1u) % NABU_KDPF_Q_SLOTS;
        net_q_count++;
    }
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
}

uint32_t
qcom_usb_debug_kdp_net_pop(void *dst, size_t cap)
{
    uint64_t daif;
    uint32_t len = 0;
    if (!dst) return 0;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #3" : "=r"(daif) :: "memory");
    if (net_q_count != 0) {
        len = net_q_len[net_q_tail];
        if (len <= cap) {
            net_copy(dst, net_q[net_q_tail], len);
            net_q_tail = (net_q_tail + 1u) % NABU_KDPF_Q_SLOTS;
            net_q_count--;
        } else {
            len = 0;    /* caller buffer too small: leave the frame queued */
        }
    }
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
    return len;
}

size_t
qcom_usb_debug_kdp_net_pending(void)
{
    return (size_t)net_q_count;
}

static void
net_feed_byte(uint8_t b, int *progress)
{
    switch (net_scan) {
    case NET_SCAN_IDLE:
        if (b == net_magic[net_magic_len]) {
            net_magic_len++;
            if (net_magic_len == sizeof(net_magic)) {
                net_magic_len = 0;
                net_scan = NET_SCAN_HEADER;
                /* Keep the magic in the header so the validated layout below
                 * is exactly the 12 wire bytes. */
                net_copy(net_hdr, net_magic, sizeof(net_magic));
                net_hdr_len = sizeof(net_magic);
                *progress = 1;
            }
            break;
        }
        /* The withheld prefix does not extend into this byte: flush it back
         * to the normal text/serial routing and retry this byte.  Every byte
         * that is not part of an accepted frame stays visible to SXNU1 and
         * serial KDP, so control traffic keeps working while net mode is on. */
        for (uint32_t j = 0; j < net_magic_len; j++) net_emit(net_magic[j]);
        net_magic_len = 0;
        if (b == net_magic[0]) net_magic_len = 1;
        else net_emit(b);
        break;
    case NET_SCAN_HEADER:
        net_hdr[net_hdr_len++] = b;
        *progress = 1;
        if (net_hdr_len < NABU_KDPF_HDR_BYTES) break;
        {
            uint32_t len = (uint32_t)net_hdr[6] | ((uint32_t)net_hdr[7] << 8);
            net_want_crc = net_le32(net_hdr + 8);
            net_hdr_len = 0;
            net_scan = NET_SCAN_IDLE;
            net_magic_len = 0;
            if (net_hdr[4] != 0x01 || net_hdr[5] != 0x00) {
                net_bad_headers++;
            } else if (len == 0 || len > NABU_KDPF_ETH_MAX) {
                net_oversize_drops++;
            } else {
                net_payload_len = len;
                net_payload_got = 0;
                net_scan = NET_SCAN_PAYLOAD;
                break;
            }
            /* Host-compatible resync: rescan from the second magic byte so a
             * real magic overlapping a rejected header is still found. */
            for (uint32_t j = 1; j < NABU_KDPF_HDR_BYTES; j++)
                net_feed_byte(net_hdr[j], progress);
        }
        break;
    case NET_SCAN_PAYLOAD:
        net_payload[net_payload_got++] = b;
        *progress = 1;
        if (net_payload_got < net_payload_len) break;
        {
            uint32_t crc = (uint32_t)z_crc32(0UL, net_payload, (unsigned)net_payload_len);
            if (crc == net_want_crc) net_q_push(net_payload, net_payload_len);
            else net_crc_errors++;
            net_scan = NET_SCAN_IDLE;
            net_magic_len = 0;
            net_payload_len = 0;
            net_payload_got = 0;
        }
        break;
    }
}

void
qcom_usb_debug_kdp_net_route(const uint8_t *in, size_t n, uint32_t generation,
                             const uint8_t **out, size_t *out_n)
{
    uint64_t daif;
    int progress = 0;

    if (!__atomic_load_n(&net_mode, __ATOMIC_ACQUIRE)) {
        *out = in;
        *out_n = n;
        return;
    }
    /* Callers pass at most SXNU_CONTROL_WORK_BYTES (256).  If that ever grows,
     * fall back to plain passthrough rather than dropping bytes silently. */
    if (n > sizeof(net_pass)) {
        *out = in;
        *out_n = n;
        return;
    }

    __asm__ volatile("mrs %0, daif\nmsr daifset, #3" : "=r"(daif) :: "memory");
    if (generation != net_gen) {
        /* Bus reset or first sight of a generation: a half frame from before
         * the reset cannot be completed by bytes from after it. */
        net_resyncs++;
        net_reset(generation);
    }
    net_pass_len = 0;
    for (size_t i = 0; i < n; i++) net_feed_byte(in[i], &progress);
    if (net_scan != NET_SCAN_IDLE) {
        if (progress) net_idle_calls = 0;
        else if (++net_idle_calls >= NABU_KDPF_IDLE_LIMIT) {
            /* Idle resync: the peer abandoned a half frame.  Drop it so the
             * next complete frame is recognised. */
            net_resyncs++;
            net_reset(generation);
        }
    } else {
        net_idle_calls = 0;
    }
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");

    *out = net_pass;
    *out_n = net_pass_len;
}

static uint8_t net_tx_wire[NABU_KDPF_HDR_BYTES + NABU_KDPF_ETH_MAX];

static void
qcom_usb_debug_kdp_net_tx(void *pkt, unsigned int pkt_len)
{
    uint8_t *o = net_tx_wire;
    uint32_t len = pkt_len;
    uint32_t crc;

    if (!pkt || len == 0 || len > NABU_KDPF_ETH_MAX) {
        net_tx_drop++;
        return;
    }
    o[0] = (uint8_t)'K';
    o[1] = (uint8_t)'D';
    o[2] = (uint8_t)'P';
    o[3] = (uint8_t)'F';
    o[4] = 0x01;
    o[5] = 0x00;
    o[6] = (uint8_t)(len & 0xffu);
    o[7] = (uint8_t)((len >> 8) & 0xffu);
    crc = (uint32_t)z_crc32(0UL, (const uint8_t *)pkt, (unsigned)len);
    o[8]  = (uint8_t)(crc & 0xffu);
    o[9]  = (uint8_t)((crc >> 8) & 0xffu);
    o[10] = (uint8_t)((crc >> 16) & 0xffu);
    o[11] = (uint8_t)((crc >> 24) & 0xffu);
    net_copy(o + NABU_KDPF_HDR_BYTES, (const uint8_t *)pkt, len);
    (void)qcom_usb_debug_write(net_tx_wire, (size_t)NABU_KDPF_HDR_BYTES + len);
}

static void
qcom_usb_debug_kdp_net_rx(void *pkt, unsigned int *pkt_len, unsigned int timeout)
{
    uint32_t got;

    (void)timeout;              /* the caller (kdp_poll) retries every 3 ms */
    if (!pkt_len) return;
    *pkt_len = 0;
    if (!pkt) return;
    /* In the debugger the 5 ms worker is blocked, so service the controller
     * here; outside it the worker already drained.  acquire() failure in a
     * panicked kernel simply leaves the queue untouched (kb-61/kb-62). */
    qcom_usb_debug_serial_poll();
    got = qcom_usb_debug_kdp_net_pop(pkt, NABU_KDPF_ETH_MAX);
    if (got != 0) *pkt_len = (unsigned int)got;
}

int
qcom_usb_debug_kdp_net_active(void)
{
    return __atomic_load_n(&net_mode, __ATOMIC_ACQUIRE) != 0;
}

void
qcom_usb_debug_kdp_net_get_stats(uint32_t *bad_headers, uint32_t *crc_errors,
                                 uint32_t *oversize_drops, uint32_t *resyncs,
                                 uint32_t *q_overflow, uint32_t *tx_drop)
{
    if (bad_headers) *bad_headers = net_bad_headers;
    if (crc_errors) *crc_errors = net_crc_errors;
    if (oversize_drops) *oversize_drops = net_oversize_drops;
    if (resyncs) *resyncs = net_resyncs;
    if (q_overflow) *q_overflow = net_q_overflow;
    if (tx_drop) *tx_drop = net_tx_drop;
}

int
qcom_usb_debug_kdp_net_try_attach(void)
{
    struct qcom_usb_debug_status status = {0};
    uint8_t ip[4]  = { 10, 0, 0, 2 };
    uint8_t mac[6] = { 0x02, 0x42, 0xac, 0x11, 0x00, 0x02 };
    int enabled = 0;

    if (qcom_usb_debug_kdp_net_active()) return 1;      /* idempotent */
    if (__atomic_load_n(&net_disabled, __ATOMIC_ACQUIRE)) return 0;
    if (!PE_parse_boot_argn("nabu-kdp-net", &enabled, sizeof(enabled)) || enabled != 1) {
        __atomic_store_n(&net_disabled, 1, __ATOMIC_RELEASE);
        return 0;
    }
    qcom_usb_debug_get_status(&status);
    if (status.phase != QCOM_USB_DEBUG_ACTIVE) return 0;    /* wait for a later tick */

    /* Net mode owns no link and must not let kdp_serial_setmode() initialise
     * the serial port when KDP is entered.  Clear both hooks. */
    kdp_register_link((void *)0, (void *)0);

    /* Order matters: register the callbacks first, then write the addresses.
     * While kdp_en_send_pkt is still kdp_serial_send, KDP_SERIAL_ENABLED()
     * is true and the guard at kdp_udp.c:773-778 refuses to replace the
     * serial fake-up IP/MAC. */
    kdp_register_send_receive(qcom_usb_debug_kdp_net_tx, qcom_usb_debug_kdp_net_rx);
    if (kdp_en_send_pkt != qcom_usb_debug_kdp_net_tx) {
        /* debug==0 / kernel_debugging_restricted(): registration refused. */
        return 0;
    }

    kdp_set_ip_and_mac_addresses((void *)ip, (void *)mac);
    __atomic_store_n(&net_mode, 1, __ATOMIC_RELEASE);
    return 1;
}
