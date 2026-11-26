/* SPDX-License-Identifier: MIT
 * Declarations for the host-side diagnostics/kernel-service stubs that the
 * extracted control handler (tests/check.py -> control-handler.inc) links
 * against. Implemented in control_handler_stubs.c.
 */
#ifndef QCOM_TEST_CONTROL_HANDLER_STUBS_H
#define QCOM_TEST_CONTROL_HANDLER_STUBS_H

#include <stddef.h>
#include <stdint.h>

/* The handler checks "&kd_control_trace == NULL" on a kernel data symbol;
 * with the stub visible in this translation unit that compare is a tautology. */
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wtautological-pointer-compare"
#endif

#define QUD_DIAG_CHUNK 192u
#define QUD_DIAG_MAX_LEN (1u << 20)
#define QUD_DIAG_MAX_PHYS (1u << 10)

struct qud_pt_result {
    uint64_t entry[4];
    unsigned level;
    uint64_t pa;
    uint64_t root;
    uint64_t ttbr1_mrs;
    uint64_t last_entry_pa;
    unsigned t1sz;
    unsigned tg1;
    unsigned page_shift;
    uint8_t valid;
    uint8_t block;
    uint8_t source;
    uint8_t stage;
};

struct qud_panic_summary {
    uint64_t base;
    uint32_t size;
    uint32_t magic;
    uint32_t version;
    uint64_t flags;
    uint32_t log_offset;
    uint32_t log_len;
    uint32_t ss_offset;
    uint32_t ss_len;
    uint32_t other_offset;
    uint32_t other_len;
    char os_version[33];
};

struct qud_bkp_entry {
    uint64_t va;
    uint32_t kind;
    uint32_t used;
};

const char *PE_boot_args(void);
int PE_parse_boot_argn(const char *arg, void *out, size_t len);
void Debugger(const char *message);
void (panic)(const char *format, ...) __attribute__((noreturn));
extern char kd_control_trace; /* opaque kernel data symbol, read via fields */

size_t qcom_usb_debug_write(const void *bytes, size_t count);
void qcom_usb_debug_kdp_tx_mode(int on);
int qcom_usb_debug_kdp_push(uint8_t value);

void kdp_connection_wait(void);
int kdp_getc(void);
int kdp_unserialize_packet(void *packet);

int native_adreno_set_hwcg(int enable);
uint32_t native_adreno_get_gpu_freq(uint32_t *requested_khz, uint32_t *vote,
                                     uint32_t *ready);
uint32_t native_adreno_gpu_profile_start(uint32_t ms);
void native_adreno_gpu_profile_stop(void);
uint32_t native_adreno_gpu_profile_read(uint64_t *out, uint32_t cap,
                                        uint32_t *count, uint32_t *dropped,
                                        uint32_t *flags);
extern volatile uint32_t native_adreno_a6xx_stats[24];
extern volatile uint32_t native_legacy_transfer[8];
extern volatile uint32_t native_legacy_surfaces[14];
extern volatile uint32_t native_legacy_direct[17];
extern volatile uint32_t native_legacy_validation[6];
extern volatile uint32_t native_ufs_dpu_smmu[24];
extern volatile uint32_t native_ufs_dpu_state;

/* KDP ring counters referenced by the DEBUGGER/KDPPUSH/STATUS replies. */
extern volatile uint32_t kdp_rx_armed;
extern uint32_t kdp_rx_used;
extern uint32_t kdp_rx_overflow;
extern uint32_t kdp_gen_seen, kdp_gen_valid, kdp_empty_drains;

extern void *tasks;
extern void *kernproc;
extern unsigned proc_struct_size;
extern volatile uint32_t control_reply_lost;

size_t qud_diag_read(uint64_t va, void *dst, size_t len);
size_t qud_diag_write(uint64_t va, const void *src, size_t len);
size_t qud_diag_read_phys(uint64_t pa, void *dst, size_t len);
int qud_read_pa_allowed(uint64_t pa, size_t len, const char **why);
int qud_diag_write_phys(uint64_t pa, const void *src, size_t len,
                        uint32_t *readback);
int qud_diag_read_begin(uint64_t va, uint32_t len, uint32_t frame, int phys);
size_t qud_diag_read_next(uint8_t *out, size_t capacity, uint32_t *offset,
                          uint32_t *frame);
int qud_diag_cursor_active(void);
int qud_diag_cursor_is_phys(void);
uint64_t qud_diag_cursor_crc(void);
uint64_t qud_diag_cursor_length(void);

int qud_dump_ufs_available(void);
int qud_dump_ufs_enqueue(uint64_t pa, uint32_t len, const uint8_t *tag,
                         uint32_t taglen);
uint32_t qud_dump_ufs_slot(uint32_t idx, uint64_t *pa, uint32_t *len,
                           uint32_t *done, const char **tag);
int qud_dump_ufs_write_first(uint32_t slot, uint32_t *blocks_written,
                             int *write_err, int *synced);
int qud_log_ufs_read_block(uint64_t rel, void *dst, int *why);
int qud_panic_header(struct qud_panic_summary *out);
int qud_pt_walk(uint64_t va, struct qud_pt_result *r);
int qud_sysctl_read(const char *name, void *out, size_t cap, size_t *needed);
int qud_kdebug_state(uint64_t *oldest_time, uint32_t *enabled, uint32_t *mode,
                     uint32_t *live_flags, uint32_t *cpus, uint32_t *flags);
int qud_ptr_probe(uint64_t va, uint64_t *words, unsigned count);
int qud_task_layout_ok(uint32_t *actual);
int qud_task_info(uint64_t task, uint64_t *map, uint64_t *threads,
                  uint32_t *nthreads, uint32_t *priority);
int qud_task_qhead(uint64_t *next, uint64_t *prev);
int qud_thread_task(uint64_t thread, uint64_t *task);
int qud_sysreg_read(const uint8_t *tok, size_t len, uint64_t *out);
int qud_sysreg_write(const uint8_t *tok, size_t len, uint64_t value);
int qud_sysreg_encoding(const uint8_t *tok, size_t len, unsigned *op0,
                        unsigned *op1, unsigned *crn, unsigned *crm,
                        unsigned *op2);
uint64_t counter(void);
void platform_smc_reset(void) __attribute__((noreturn));
int qud_bkp_add(uint64_t va, uint32_t kind);
int qud_bkp_del(uint64_t va);
unsigned qud_bkp_list(struct qud_bkp_entry *out, unsigned max);

#endif /* QCOM_TEST_CONTROL_HANDLER_STUBS_H */
