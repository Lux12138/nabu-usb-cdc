/* SPDX-License-Identifier: MIT
 * Host-side symbol stubs for the extracted production control handler.
 *
 * tests/check.py slices kernel/QcomXnuUsbDebug.c between control_reply_reboot()
 * and control_service() and links that text into a host test together with the
 * real kernel/QcomUsbScreenshot.c. The slice references the diagnostics API
 * (qud_*) and a few kernel services; this unit provides inert definitions so
 * the host link succeeds without dragging in XNU headers.
 *
 * These stubs return safe empty values on purpose: the host tests only drive
 * the screenshot/reboot paths. The forensics verbs have their own stubs in
 * tests/test_forensics_verbs.c.
 */
#include <stddef.h>
#include <stdint.h>

#include "control_handler_stubs.h"

const char *PE_boot_args(void) { return ""; }
int PE_parse_boot_argn(const char *arg, void *out, size_t len)
{
    (void)arg; (void)out; (void)len;
    return 0;
}
void Debugger(const char *message) { (void)message; }
void (panic)(const char *format, ...)
{
    (void)format;
    __builtin_trap();
}
char kd_control_trace;
volatile uint32_t native_adreno_a6xx_stats[24];
volatile uint32_t native_legacy_transfer[8];
volatile uint32_t native_legacy_surfaces[14];
volatile uint32_t native_legacy_direct[17];
volatile uint32_t native_legacy_validation[6];
volatile uint32_t native_ufs_dpu_smmu[24];
volatile uint32_t native_ufs_dpu_state;
volatile uint32_t kdp_rx_armed;
uint32_t kdp_rx_used;
uint32_t kdp_rx_overflow;
uint32_t kdp_gen_seen, kdp_gen_valid, kdp_empty_drains;
void *tasks;
void *kernproc;
unsigned proc_struct_size;
volatile uint32_t control_reply_lost;

size_t qcom_usb_debug_write(const void *bytes, size_t count)
{
    (void)bytes;
    return count;
}
void qcom_usb_debug_kdp_tx_mode(int on) { (void)on; }
int qcom_usb_debug_kdp_push(uint8_t value) { (void)value; return 1; }

void kdp_connection_wait(void) {}
int kdp_getc(void) { return -1; }
int kdp_unserialize_packet(void *packet) { (void)packet; return 0; }

int native_adreno_set_hwcg(int enable) { (void)enable; return 0; }
uint32_t native_adreno_get_gpu_freq(uint32_t *requested_khz, uint32_t *vote,
                                     uint32_t *ready)
{
    if (requested_khz) *requested_khz = 0;
    if (vote) *vote = 0;
    if (ready) *ready = 0;
    return 0;
}
uint32_t native_adreno_gpu_profile_start(uint32_t ms) { (void)ms; return 0; }
void native_adreno_gpu_profile_stop(void) {}
uint32_t native_adreno_gpu_profile_read(uint64_t *out, uint32_t cap,
                                        uint32_t *count, uint32_t *dropped,
                                        uint32_t *flags)
{
    (void)out; (void)cap;
    if (count) *count = 0;
    if (dropped) *dropped = 0;
    if (flags) *flags = 0;
    return 0;
}

size_t qud_diag_read(uint64_t va, void *dst, size_t len)
{
    (void)va; (void)dst; (void)len;
    return 0;
}
size_t qud_diag_write(uint64_t va, const void *src, size_t len)
{
    (void)va; (void)src; (void)len;
    return 0;
}
size_t qud_diag_read_phys(uint64_t pa, void *dst, size_t len)
{
    (void)pa; (void)dst; (void)len;
    return 0;
}
int qud_read_pa_allowed(uint64_t pa, size_t len, const char **why)
{
    (void)pa; (void)len;
    if (why) *why = "stub";
    return 0;
}
int qud_diag_write_phys(uint64_t pa, const void *src, size_t len,
                        uint32_t *readback)
{
    (void)pa; (void)src; (void)len; (void)readback;
    return 0;
}
int qud_diag_read_begin(uint64_t va, uint32_t len, uint32_t frame, int phys)
{
    (void)va; (void)len; (void)frame; (void)phys;
    return 0;
}
size_t qud_diag_read_next(uint8_t *out, size_t capacity, uint32_t *offset,
                          uint32_t *frame)
{
    (void)out; (void)capacity;
    if (offset) *offset = 0;
    if (frame) *frame = 0;
    return 0;
}
int qud_diag_cursor_active(void) { return 0; }
int qud_diag_cursor_is_phys(void) { return 0; }
uint64_t qud_diag_cursor_crc(void) { return 0; }
uint64_t qud_diag_cursor_length(void) { return 0; }

int qud_dump_ufs_available(void) { return 0; }
int qud_dump_ufs_enqueue(uint64_t pa, uint32_t len, const uint8_t *tag,
                         uint32_t taglen)
{
    (void)pa; (void)len; (void)tag; (void)taglen;
    return -2;
}
uint32_t qud_dump_ufs_slot(uint32_t idx, uint64_t *pa, uint32_t *len,
                           uint32_t *done, const char **tag)
{
    (void)idx;
    if (pa) *pa = 0;
    if (len) *len = 0;
    if (done) *done = 0;
    if (tag) *tag = "";
    return 0;
}
int qud_dump_ufs_write_first(uint32_t slot, uint32_t *blocks_written,
                             int *write_err, int *synced)
{
    (void)slot;
    if (blocks_written) *blocks_written = 0;
    if (write_err) *write_err = 0;
    if (synced) *synced = 0;
    return 0;
}
int qud_log_ufs_read_block(uint64_t rel, void *dst, int *why)
{
    (void)rel; (void)dst;
    if (why) *why = 1;
    return 0;
}
int qud_panic_header(struct qud_panic_summary *out)
{
    (void)out;
    return 0;
}
int qud_pt_walk(uint64_t va, struct qud_pt_result *r)
{
    (void)va; (void)r;
    return 0;
}
int qud_sysctl_read(const char *name, void *out, size_t cap, size_t *needed)
{
    (void)name; (void)out; (void)cap;
    if (needed) *needed = 0;
    return 0;
}
int qud_kdebug_state(uint64_t *oldest_time, uint32_t *enabled, uint32_t *mode,
                     uint32_t *live_flags, uint32_t *cpus, uint32_t *flags)
{
    (void)oldest_time; (void)enabled; (void)mode;
    (void)live_flags; (void)cpus; (void)flags;
    return 0;
}
int qud_ptr_probe(uint64_t va, uint64_t *words, unsigned count)
{
    (void)va; (void)words; (void)count;
    return 0;
}
int qud_task_layout_ok(uint32_t *actual)
{
    if (actual) *actual = 0;
    return 0;
}
int qud_task_info(uint64_t task, uint64_t *map, uint64_t *threads,
                  uint32_t *nthreads, uint32_t *priority)
{
    (void)task; (void)map; (void)threads;
    if (nthreads) *nthreads = 0;
    if (priority) *priority = 0;
    return 0;
}
int qud_task_qhead(uint64_t *next, uint64_t *prev)
{
    if (next) *next = 0;
    if (prev) *prev = 0;
    return 0;
}
int qud_thread_task(uint64_t thread, uint64_t *task)
{
    (void)thread;
    if (task) *task = 0;
    return 0;
}
int qud_sysreg_read(const uint8_t *tok, size_t len, uint64_t *out)
{
    (void)tok; (void)len;
    if (out) *out = 0;
    return 0;
}
int qud_sysreg_write(const uint8_t *tok, size_t len, uint64_t value)
{
    (void)tok; (void)len; (void)value;
    return 0;
}
int qud_sysreg_encoding(const uint8_t *tok, size_t len, unsigned *op0,
                        unsigned *op1, unsigned *crn, unsigned *crm,
                        unsigned *op2)
{
    (void)tok; (void)len; (void)op0; (void)op1; (void)crn; (void)crm; (void)op2;
    return 0;
}
uint64_t counter(void) { return 0; }
void platform_smc_reset(void) { __builtin_trap(); }
int qud_bkp_add(uint64_t va, uint32_t kind)
{
    (void)va; (void)kind;
    return -2;
}
int qud_bkp_del(uint64_t va) { (void)va; return 0; }
unsigned qud_bkp_list(struct qud_bkp_entry *out, unsigned max)
{
    (void)out; (void)max;
    return 0;
}
