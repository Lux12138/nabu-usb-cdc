/* SPDX-License-Identifier: MIT
 * Native XNU backend for the freestanding DWC3 core. No EFI state,
 * Android, pstore, partition, filesystem, allocation or scheduler dependency.
 */
#include "QcomXnuUsbDebug.h"
#include "QcomUsbBuffer.h"
#include "QcomUsbControl.h"
#include "QcomUsbForensics.h" /* T140: pure parsing/planning for the forensics verbs */
#include "QcomUsbScreenshot.h"
#include "../usb/Dwc3.h"
#include "../soc/QcomUsbQualify.h"
#include "../soc/QcomUsbDmaDomain.h"
#include "../soc/QcomUsbBypass.h"
#include "../include/QcomUsbPrepare.h"
#include <machine/machine_routines.h>
/* XNU 17.7.11 keeps ml_io_map in osfmk/arm/machine_routines.h, which the
 * generic <machine/machine_routines.h> does not pull in for this SDK. */
extern vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size);
/* GPU runtime control (GPUHWCG); weak so a kernel without the driver links. */
extern int native_adreno_set_hwcg(int enable) __attribute__((weak_import));
extern uint32_t native_adreno_get_gpu_freq(uint32_t *requested_khz,
    uint32_t *vote, uint32_t *ready) __attribute__((weak_import));
/* S4 PERF family: weak so a kernel without the GPU profiler still links. */
extern uint32_t native_adreno_gpu_profile_start(uint32_t) __attribute__((weak_import));
extern void native_adreno_gpu_profile_stop(void) __attribute__((weak_import));
extern uint32_t native_adreno_gpu_profile_read(uint64_t *, uint32_t, uint32_t *, uint32_t *, uint32_t *) __attribute__((weak_import));

/* kb-16: data symbols the drivers already export under their C names (verified
 * with llvm-nm against the live kernel). A weak_import *data* symbol needs an
 * address test (&sym == 0), which is what the handlers below do. */
extern volatile uint32_t native_adreno_a6xx_stats[24] __attribute__((weak_import));
extern volatile uint32_t native_legacy_transfer[8] __attribute__((weak_import));
extern volatile uint32_t native_legacy_surfaces[14] __attribute__((weak_import));
extern volatile uint32_t native_legacy_direct[17] __attribute__((weak_import));
extern volatile uint32_t native_legacy_validation[6] __attribute__((weak_import));
extern volatile uint32_t native_ufs_dpu_smmu[24] __attribute__((weak_import));
extern volatile uint32_t native_ufs_dpu_state __attribute__((weak_import));

/* kb-16a: XNU's debugger entry point. Debugger() takes the debugger trap and
 * returns once the KDP session ends, so it is the supported way to bring KDP
 * up on demand (panic() would not come back). */
extern void Debugger(const char *message) __attribute__((weak_import));
#include <kern/cpu_number.h>
#include <pexpert/pexpert.h>

/* Arbitrary kernel memory access for the diagnostics verbs; QcomUsbDiag.c. */
#include "QcomUsbDiag.c"
/* Read-only bus primitives (TLMM GPIO, GIC snapshot) and the SYSREGSCAN
 * batch reader. Both are gated behind nabu-verbs-bus=1. Included rather than
 * added to build.py UNITS so the archive keeps one translation unit and the
 * new code compiles under exactly the same recipe flags as the console. */
#include "QcomUsbBusPrims.c"
#include "QcomUsbSysregScan.c"
/* T144: PM8150B Gen4 fuel gauge probe (T91 P1).  Read-only, nabu-fg=1 gated,
 * same weak-import transport discipline as the buttons. */
#include "QcomFuelGauge.c"

/* The real panic(), deliberately NOT the osfmk/kern/debug.h macro: that header
 * is kernel-private and its panic() appends the call site, which would put
 * __FILE__/__LINE__ in the message instead of the marker PANICTRIG wants.
 * _panic is on the build.py import allow-list. */
/* panic is already a macro in the kernel headers this file pulls in, so the
 * declaration has to escape it with parentheses. Same for the call site. */
extern void (panic)(const char *format, ...) __attribute__((noreturn));
/* T140 forensics verbs: device-tree walk (kernel/QcomUsbDTDebug.cpp) and the
 * live NABULOG window geometry (kernel/QcomUsbDiag.c). Explicit declarations
 * keep the new call sites out of the implicit-declaration warning list. */
extern int native_dt_explore(const char *path, char *out, size_t cap,
                             size_t *used, const char **why,
                             uint32_t max_entries, int *truncated);
extern int qud_nabulog_open(uint64_t *first, uint64_t *blocks, const char **why);

/* T150 DEVTREE off-console execution (boot-arg nabu-dt-thread=1, default off).
 *
 * T149 measured every DEVTREE producing no reply at all while the rest of the
 * console stayed healthy. The walk is the only code on that path that enters
 * the IORegistry, and its locks can sleep; running it synchronously on the
 * console worker turns a slow/wedged registry operation into console silence.
 * With this gate on, the handler snapshots the request, a one-shot kernel
 * thread performs the walk, and the handler answers with the cached dump or an
 * explicit ERR (busy/slow/nothread) after a bounded wait.
 *
 * Prototypes mirror kern/sched_prim.h / kern/thread.h, which stay hidden from
 * this translation unit's header set; IOLog comes from iokit/Kernel/IOLib.cpp.
 */
typedef void (*qud_dt_thread_fn)(void *, unsigned int);
extern int kernel_thread_start(qud_dt_thread_fn continuation, void *parameter,
                               void **new_thread);
extern void thread_deallocate(void *thread);
extern void thread_terminate_self(void);
extern void IOLog(const char *format, ...);

/* Shared job slot: the console handler fills it and starts the walker; the
 * walker publishes the result. state: 0 idle, 1 running, 2 done, 3 failed.
 * The handler only overwrites a slot that is not running. */
struct qud_dt_job {
    char path[64];
    uint32_t max_entries;
    char body[384];
    size_t used;
    const char *why;
    int truncated;
    uint32_t state;
    uint64_t submit_tick;
};
static struct qud_dt_job qud_dt_job;
static int qud_dt_gate = -1;            /* -1 unevaluated, 0 off, 1 on */

static int
qud_dt_thread_enabled(void)
{
    if (qud_dt_gate < 0) {
        int v = 0;
        (void)PE_parse_boot_argn("nabu-dt-thread", &v, sizeof(v));
        qud_dt_gate = (v == 1) ? 1 : 0;
    }
    return qud_dt_gate == 1;
}

/* Defined far below (the KDP reply gate), but PANICTRIG needs it before the call
 * to make sure the panic message can still leave. */
void qcom_usb_debug_kdp_tx_mode(int on);

/* T131: the optional KDP transport module (QcomKdpNet.c) is linked after this
 * file, and its active flag decides which CDC data interface carries KDP. */
int qcom_usb_debug_kdp_net_active(void);
/* T143: qcom_usb_debug_write() consults the KDP reply transmit gate far above
 * the definition further down.  T131 added the call without a prototype and
 * clang-24 rejects it as an implicit declaration; the required real compile
 * caught it.  Declaration only -- no behaviour change. */
int qcom_usb_debug_kdp_tx_active(void);

/* kb-17u KDP ring state. Declared up here because qcom_usb_debug_write()
 * (which runs from kprintf) and the DEBUGGER verb both consult them
 * long before the ring implementation further down. */
static volatile uint32_t kdp_rx_armed;
/* T143: pipe 1 (the second CDC data interface) owns an independent KDP latch.
 * kdp_rx_armed stays the pipe-0/console latch, so raising or clearing one can
 * never change the other. */
static volatile uint32_t kdp_rx_armed1;
/* T143: a Debugger() call is on the stack, from either pipe.  Serialises trap
 * entry so a frame arriving while a session is unwinding cannot nest a second
 * Debugger() (XNU's CPUDEBUGGERCOUNT guard would spin). */
static volatile uint32_t kdp_trap_active;
/* T143: true while either pipe owns a KDP session.  Only the controller-level
 * effects (poll budget, io_failed latch, diag flush window, serial input
 * routing) look at this; per-pipe flags stay authoritative everywhere else. */
static uint32_t
kdp_session_active(void)
{
    return __atomic_load_n(&kdp_rx_armed, __ATOMIC_ACQUIRE) ||
           __atomic_load_n(&kdp_rx_armed1, __ATOMIC_ACQUIRE);
}
/* kb-32: the shared bounded DWC3 RX drain, defined far below. The KDP input
 * predicates call it while armed so a request sent after CONNECT is seen. */
static void serial_rx_drain(void);
static uint32_t kdp_rx_used;
/* kb-28 (T41-c #1): bytes dropped because the ring was full. Lives here for
 * the same reason as the two above -- KDPPUSH and DEBUGGER report it and both
 * are compiled before the ring implementation. */
static uint32_t kdp_rx_overflow;
/* kb-28 (T41-c): KDP reply transmit gate.
 *
 * The pe_serial CDC backend is receive-only on purpose: forwarding
 * uart_putc() into qcom_usb_debug_putc() re-enters the USB write path from
 * inside kprintf (interrupts off, kprintf_lock held) and boot died twice that
 * way. But KDP replies travel exactly that path:
 *
 *   kdp_serial_send -> pal_serial_putc_nocr -> serial_putc -> uart_putc
 *     -> uart_putc_device -> gPESF -> nabu_cdc_transmit_data -> (dropped)
 *
 * So a CONNECT reply, or any request/response payload, was thrown away. The
 * console printed "Connected to remote debugger." and then the host saw
 * nothing -- measured on kb-27, where the capture stops right there.
 *
 * This flag narrows the forwarding to exactly that case: kdp_serial_send()
 * raises it around kdp_serialize_packet(), so only KDP framing bytes reach
 * the transmit hook. kprintf never takes it, which keeps the re-entry hazard
 * out of the boot path -- the original reason the hook was a no-op. */
static volatile uint32_t kdp_tx_mode;

#define NATIVE_CONTEXT_BYTES DWC3_CONTEXT_BYTES
#define GCC_BYTES QUD_GCC_BYTES
#define USB_BYTES QUD_DWC3_BYTES
#define PAGE_BYTES 4096u
#define USB_GCTL 0xc110u
#define USB_DCTL 0xc704u
#define USB_DSTS 0xc70cu
#define USB_GHWPARAMS0 0xc140u
#define USB_GUSB2PHYCFG 0xc200u
#define USB_PHY_KEEP_AWAKE_MASK ((1u << 6) | (1u << 8))
#define USB_RUN_STOP (1u << 31)
#define PHY_MAP_BYTES 0x1000u
#define USB_HALTED (1u << 22)
#define USB_HS_PHY_CTRL QUD_QSCRATCH_HS_PHY_CTRL_OFFSET
#define USB_SW_VBUS_MASK QUD_QSCRATCH_VBUS_MASK
#define COUNTER_MAX_FREQUENCY UINT64_C(1000000000)
#define CONTROL_PE_RESTART_CPU 1u

/* T142 trace-ring gates.  Tentative declarations here because
 * qcom_usb_debug_init() assigns them; the definitions live in the trace
 * block further down so tests/check.py's extracted control-handler window
 * is a complete translation unit.  0 = boot-arg absent = verbs refuse. */
static int qud_trace_bootarg_gate;
static int qud_tel_bootarg_gate;

/* The platform callback ABI is unchanged from Darwin22 IOPlatformExpert.h.
 * Only the control opt-in may atomically install into this NULL global. */
extern int (*PE_halt_restart)(unsigned int);

static uint8_t usb_dma[DWC3_DMA_BYTES] __attribute__((aligned(DWC3_DMA_ALIGNMENT)));
static uint8_t usb_tables[QUSB_DOMAIN_PT_BYTES] __attribute__((aligned(PAGE_BYTES)));
static uint8_t usb_context[NATIVE_CONTEXT_BYTES] __attribute__((aligned(DWC3_CONTEXT_ALIGNMENT)));
static volatile unsigned int usb_busy;
/* T131: nabu-cdc-second-port=1 asks the DWC3 core for the composite dual-CDC
 * descriptors and endpoints.  Default off keeps the single-function device and
 * every existing code path exactly as before. */
static unsigned int cdc_second_port;
/* T143: nabu-cdc-second-port=2 adds "pipe 1 activates KDP by itself" on top of
 * the =1 dual-interface behaviour.  0/absent and 1 keep the historical path,
 * so the boot-arg string itself costs zero extra command-line bytes. */
static unsigned int kdp_pipe1_mode;
/* Volatile like usb_serial_rx_kdp: the worker and a keyboard poll can both
 * reach the drain, and the detector state must not be cached across them. */
static volatile uint32_t kdp_pipe1_in_frame;    /* inside 0xFA..0xFB */
static volatile uint32_t kdp_pipe1_arm_pending; /* complete frame waits for trap */
static volatile uint32_t kdp_pipe1_enters, kdp_pipe1_stray;
static volatile unsigned int usb_diag_enabled, usb_diag_checkpoint, usb_diag_resetting, usb_diag_target;
static volatile unsigned int usb_diag_mode, usb_diag_polling;
static uint8_t usb_log_bytes[SOC_USB_LOG_BUFFER_BYTES];
static struct soc_usb_buffer usb_log_buffer = {
    .data = usb_log_bytes, .capacity = sizeof(usb_log_bytes)
};
static uint64_t usb_log_replayed, usb_log_loss_reported;
static struct {
    struct qcom_usb_debug_status status;
    vm_offset_t gcc_va, usb_va, smmu_va, phy_va;
    struct dwc3 *core;
    QUSB_DOMAIN_LEASE lease;
    QUSB_DMA_LEASE dma_lease;
    QUSB_BYPASS_LEASE bypass_lease;
    QUSB_DOMAIN_PLATFORM domain_platform;
    uint64_t deadline;
    unsigned int deadline_active, deadline_expired, core_initialized;
} native;
static struct {
    struct sxnu_control_parser parser;
    struct sxnu_control_state state;
    uint64_t seed;
} usb_control_context;
static volatile unsigned int control_started, control_enabled;
/* T140: DEVTREE / FBRAW / NABULOGRING are parsed always but only execute
 * behind this boot-arg. Default off; with it off the handler rejects them
 * exactly like an unknown verb (counted, no reply, sequence untouched). */
static unsigned usb_forensics_verbs;
static volatile unsigned int control_hook_owned, control_reboot_requested;
static volatile unsigned int control_reboot_taken, control_reboot_armed;
static volatile uint32_t control_rejected, control_reply_count, control_failures;
static volatile uint32_t control_reboot_seq, control_reboot_generation;
static volatile uint64_t control_reboot_session;
/* T80: an ack that could not be queued is not the same as a request the parser
 * rejected. Count both separately so STATUS can distinguish "HELLO answered,
 * REBOOT silent" (rl=) from a sequence rejection (rej=). */
static volatile uint32_t control_reply_lost;
/* T80: KDP teardown state. kdp_gen_* track the bus generation the armed session
 * started on; kdp_empty_drains counts consecutive drain calls that returned no
 * bytes while armed. Both feed the optional channel-death degrade. */
static volatile uint32_t kdp_release_resyncs, kdp_death_count, kdp_generation_changes;
static uint32_t kdp_gen_seen, kdp_gen_valid, kdp_empty_drains;

static void platform_smc_reset(void) __attribute__((noreturn));
/* PSCI SYSTEM_RESET for the control REBOOT path. reboot_kernel() stalls
 * in vfs_unmountall() on this port, so the worker calls this instead. */
void
qcom_usb_debug_platform_reset(void)
{
    platform_smc_reset();
}
static int qcom_usb_control_halt_restart(unsigned int type);

static void bytes_zero(void *pointer, size_t bytes)
{
    volatile uint8_t *p = pointer;
    while (bytes--) *p++ = 0;
}
static void bytes_copy(void *to, const void *from, size_t bytes)
{
    volatile uint8_t *d = to;
    const volatile uint8_t *s = from;
    while (bytes--) *d++ = *s++;
}
static uint64_t counter(void)
{
    uint64_t value;
    /* Match XNU nonspeculative_timebase and the hardware-qualified UEFI
     * TimerLib: intervals use CNTVCT with CNTFRQ, not the physical counter. */
    __asm__ volatile("isb\nmrs %0, cntvct_el0" : "=r"(value) :: "memory");
    return value;
}
static void diag_reset(unsigned int seconds);
static void platform_smc_reset(void)
{
    /* Existing hardware-qualified final reset sequence. The diagnostic path
     * and the opted-in control callback share this exact clobber contract. */
#if QUD_PSCI_SMC_RESET
    __asm__ volatile("movz w0, #9\nmovk w0, #0x8400, lsl #16\n"
                     "mov x1, xzr\nmov x2, xzr\nmov x3, xzr\ndsb sy\nsmc #0"
                     ::: "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
                         "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
                         "x16", "x17", "memory");
#endif
    for (;;) __asm__ volatile("wfe" ::: "memory");
}
static int qcom_usb_control_halt_restart(unsigned int type)
{
    if (type == CONTROL_PE_RESTART_CPU &&
        __atomic_load_n(&control_reboot_requested, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&control_reboot_armed, __ATOMIC_ACQUIRE))
        platform_smc_reset();
    return -1;
}
static void diag_checkpoint(unsigned int checkpoint)
{
    if (!usb_diag_enabled) return;
    usb_diag_checkpoint = checkpoint;
    native.status.diag_checkpoint = checkpoint;
    __asm__ volatile("dsb ishst" ::: "memory");
    if (usb_diag_target == checkpoint) diag_reset(0);
}
static void diag_reset(unsigned int seconds)
{
    uint64_t frequency, start, ticks;
    if (!usb_diag_enabled) return;
    /* Standalone early-panic path: no CPU-data, USB guard, EFI or deadline
     * dependency. A second panic while timing goes directly to reset. */
    __asm__ volatile("msr daifset, #15" ::: "memory");
    if (__atomic_exchange_n(&usb_diag_resetting, 1u, __ATOMIC_RELAXED)) seconds = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    /* Probe204 uses the separately device-verified 19.2MHz reference so
     * an incorrect advertised CNTFRQ cannot corrupt its own classification. */
    if (usb_diag_mode == 204u) frequency = UINT64_C(19200000);
    if (frequency >= 1000u && frequency <= COUNTER_MAX_FREQUENCY && seconds <= 256u) {
        start = counter(); ticks = frequency * seconds;
        while (counter() - start < ticks) __asm__ volatile("yield" ::: "memory");
    }
    platform_smc_reset();
}
static void barrier(void *cookie)
{
    (void)cookie;
    __asm__ volatile("dsb sy" ::: "memory");
}
static int deadline_expired(void)
{
    if (native.deadline_active && counter() >= native.deadline) {
        native.deadline_expired = 1;
        return 1;
    }
    return 0;
}
static int acquire(uint64_t *saved_daif)
{
    if (cpu_number() != master_cpu) return 0;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #3" : "=r"(*saved_daif) :: "memory");
    if (__atomic_exchange_n(&usb_busy, 1u, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("msr daif, %0" :: "r"(*saved_daif) : "memory");
        return 0;
    }
    return 1;
}
static void release(uint64_t saved_daif)
{
    __atomic_store_n(&usb_busy, 0u, __ATOMIC_RELEASE);
    __asm__ volatile("msr daif, %0" :: "r"(saved_daif) : "memory");
}
static size_t backlog_write(const void *bytes, size_t count)
{
    uint64_t flags;
    size_t accepted;
    /* No CPU-data access: console hooks may run before PE_init_kprintf.
     * Local IRQ/FIQ masking prevents self-interruption inside the ring guard;
     * other CPUs and recursive producers never spin waiting for this CPU. */
    __asm__ volatile("mrs %0, daif\nmsr daifset, #3" : "=r"(flags) :: "memory");
    accepted = soc_usb_buffer_write(&usb_log_buffer, bytes, count);
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
    return accepted;
}
static size_t backlog_sink(void *cookie, const void *bytes, size_t count)
{
    (void)cookie;
    return dwc3_queue(native.core, bytes, count);
}
static void backlog_drain_owned(void)
{
    size_t free_bytes = DWC3_TX_BYTES - dwc3_queued(native.core);
    /* usb_busy already owns DWC3 and masks local IRQ/FIQ. Only queue CPU
     * memory while the backlog guard is held; MMIO is serviced afterwards. */
    if (free_bytes)
        usb_log_replayed += soc_usb_buffer_drain(&usb_log_buffer, backlog_sink, 0, free_bytes);
}
static void buffer_status_owned(void)
{
    native.status.buffer_capacity = sizeof(usb_log_bytes);
    native.status.buffer_pending = soc_usb_buffer_pending(&usb_log_buffer);
    native.status.buffer_high_water = __atomic_load_n(&usb_log_buffer.high_water, __ATOMIC_RELAXED);
    native.status.buffer_accepted = __atomic_load_n(&usb_log_buffer.accepted, __ATOMIC_RELAXED);
    native.status.buffer_overflow = __atomic_load_n(&usb_log_buffer.overflow, __ATOMIC_RELAXED);
    native.status.buffer_contended = __atomic_load_n(&usb_log_buffer.contended, __ATOMIC_RELAXED);
    native.status.buffer_replayed = usb_log_replayed;
    native.status.transport_pending = native.core_initialized ? dwc3_queued(native.core) : 0;
    native.status.dropped = soc_usb_buffer_dropped(&usb_log_buffer) +
                           (native.core_initialized ? dwc3_dropped(native.core) : 0);
    native.status.rx_pending = native.core_initialized ? (uint32_t)dwc3_rx_pending(native.core) : 0;
    native.status.rx_overflow = native.core_initialized ? dwc3_rx_overflow(native.core) : 0;
    native.status.rx_generation = native.core_initialized ? dwc3_rx_generation(native.core) : 0;
    native.status.control_enabled = __atomic_load_n(&control_enabled, __ATOMIC_ACQUIRE);
    native.status.control_hook_owned = __atomic_load_n(&control_hook_owned, __ATOMIC_ACQUIRE);
    native.status.control_rejected = __atomic_load_n(&control_rejected, __ATOMIC_RELAXED);
    native.status.control_replies = __atomic_load_n(&control_reply_count, __ATOMIC_RELAXED);
    native.status.control_reboot_state = !__atomic_load_n(&control_reboot_requested, __ATOMIC_ACQUIRE) ? 0u :
        (__atomic_load_n(&control_reboot_taken, __ATOMIC_ACQUIRE) ? 2u : 1u);
}
static char *append_text(char *to, const char *text)
{
    while (*text) *to++ = *text++;
    return to;
}
static char *append_decimal(char *to, uint64_t value)
{
    char digits[20];
    unsigned int n = 0;
    do { digits[n++] = (char)('0' + value % 10u); value /= 10u; } while (value);
    while (n) *to++ = digits[--n];
    return to;
}
/* Small in-band accounting records use no printf/console recursion. */
static size_t loss_record(char *text, int final)
{
    char *p = append_text(text, final ? "\nQCOM USB DRAIN pending=0" : "\nQCOM USB LOSS");
    p = append_text(p, " overflow=");
    p = append_decimal(p, __atomic_load_n(&usb_log_buffer.overflow, __ATOMIC_RELAXED));
    p = append_text(p, " contention=");
    p = append_decimal(p, __atomic_load_n(&usb_log_buffer.contended, __ATOMIC_RELAXED));
    p = append_text(p, " transport=");
    p = append_decimal(p, dwc3_dropped(native.core));
    *p++ = '\n';
    return (size_t)(p - text);
}
static void loss_notice_owned(void)
{
    char text[160];
    size_t bytes;
    uint64_t dropped = soc_usb_buffer_dropped(&usb_log_buffer) + dwc3_dropped(native.core);
    if (dropped == usb_log_loss_reported || soc_usb_buffer_pending(&usb_log_buffer)) return;
    bytes = loss_record(text, 0);
    if (DWC3_TX_BYTES - dwc3_queued(native.core) >= bytes &&
        dwc3_queue(native.core, text, bytes) == bytes) usb_log_loss_reported = dropped;
}
static void fail(unsigned int error, int core_result)
{
    native.status.error = error;
    native.status.core_result = core_result;
    native.status.phase = QCOM_USB_DEBUG_REFUSED;
    native.status.dma_lease_active = native.lease.active;
}
static int in_range(uint64_t address, uint64_t base, uint64_t bytes)
{
    return !(address & 3u) && address >= base && address - base <= bytes - 4;
}
static volatile uint32_t *mmio(uint64_t physical)
{
    vm_offset_t base = 0;
    uint64_t offset = 0;
    if (in_range(physical, QUSB_GCC_BASE, GCC_BYTES)) {
        base = native.gcc_va; offset = physical - QUSB_GCC_BASE;
    } else if (in_range(physical, QUSB_DWC3_BASE, USB_BYTES)) {
        base = native.usb_va; offset = physical - QUSB_DWC3_BASE;
    } else if (in_range(physical, QUSB_SMMU_BASE, QUSB_SMMU_BYTES)) {
        base = native.smmu_va; offset = physical - QUSB_SMMU_BASE;
    } else if (native.phy_va && in_range(physical, QUD_PHY_BASE, PHY_MAP_BYTES)) {
        base = native.phy_va; offset = physical - QUD_PHY_BASE;
    }
    if (!base) {
        native.status.io_failed = 1;
        native.status.error = QCOM_USB_DEBUG_CALLBACK_RANGE_FAILED;
        return 0;
    }
    return (volatile uint32_t *)(uintptr_t)(base + offset);
}
static int physical_read(void *cookie, QUSB_U64 address, QUSB_U32 *value)
{
    volatile uint32_t *p;
    (void)cookie;
    if (!value || deadline_expired()) {
        native.status.io_failed = 1;
        return 0;
    }
    p = mmio(address);
    if (!p) return 0;
    barrier(0); *value = *p; barrier(0);
    return 1;
}
static int physical_write(void *cookie, QUSB_U64 address, QUSB_U32 value)
{
    volatile uint32_t *p;
    (void)cookie;
    if (deadline_expired()) { native.status.io_failed = 1; return 0; }
    /* Only USB registers or shared-policy-approved selected-domain registers
     * can be written. Private CB, local TLB and exact SID40 SMR/S2CR writes
     * must match the recorded phase/value. Global SMMU/GCC writes are denied. */
    if (!in_range(address, QUSB_DWC3_BASE, USB_BYTES) &&
        !in_range(address, QUSB_SMMU_BASE, QUSB_SMMU_BYTES)) {
        native.status.io_failed = 1;
        native.status.error = QCOM_USB_DEBUG_CALLBACK_RANGE_FAILED;
        return 0;
    }
    if (in_range(address, QUSB_SMMU_BASE, QUSB_SMMU_BYTES)) {
        QUSB_U32 cr0;
        QUSB_U32 expected_cr0;
        /* Two audited writers: the private translated domain, or (when the
         * bootloader already routes our SID) the identity/bypass lease. */
        if (!QcomUsbDomainWriteAllowed(&native.lease, address, value, 4) &&
            !QcomUsbDmaWriteAllowed(&native.dma_lease, address, value) &&
            !QcomUsbBypassWriteAllowed(&native.bypass_lease, address, value)) {
            native.status.io_failed = 1;
            native.status.error = QCOM_USB_DEBUG_CALLBACK_RANGE_FAILED;
            return 0;
        }
        expected_cr0 = native.dma_lease.active ? native.dma_lease.original_cr0 :
                       native.bypass_lease.active ? native.bypass_lease.original_cr0 :
                                                    native.lease.original_cr0;
        if (!physical_read(0, QUSB_SMMU_BASE, &cr0) ||
            (cr0 & 0x409u) != (expected_cr0 & 0x409u)) {
            native.status.io_failed = 1;
            native.status.error = QCOM_USB_DEBUG_CALLBACK_RANGE_FAILED;
            return 0;
        }
    }
    p = mmio(address);
    if (!p) return 0;
    barrier(0); *p = value; barrier(0);
    return 1;
}
/* Prepare-only writer: the device-mode bring-up must touch GCC clocks/resets
 * and the HS PHY, which physical_write deliberately refuses. SMMU stays out of
 * reach here. */
static int prepare_write(void *cookie, QUSB_U64 address, QUSB_U32 value)
{
    volatile uint32_t *p;
    (void)cookie;
    if (deadline_expired()) { native.status.io_failed = 1; return 0; }
    if (!in_range(address, QUSB_GCC_BASE, QUD_GCC_BYTES) &&
        !in_range(address, QUSB_DWC3_BASE, USB_BYTES) &&
        !(native.phy_va && in_range(address, QUD_PHY_BASE, PHY_MAP_BYTES))) {
        native.status.io_failed = 1;
        native.status.error = QCOM_USB_DEBUG_CALLBACK_RANGE_FAILED;
        return 0;
    }
    p = mmio(address);
    if (!p) return 0;
    barrier(0); *p = value; barrier(0);
    return 1;
}
static int physical_read64(void *cookie, QUSB_U64 address, QUSB_U64 *value)
{
    volatile QUSB_U64 *p;
    (void)cookie;
    if (!value || (address & 7u) || address < QUSB_SMMU_BASE ||
        address - QUSB_SMMU_BASE > QUSB_SMMU_BYTES - 8u || !native.smmu_va || deadline_expired()) {
        native.status.io_failed = 1;
        native.status.error = QCOM_USB_DEBUG_CALLBACK_RANGE_FAILED;
        return 0;
    }
    p = (volatile QUSB_U64 *)(uintptr_t)(native.smmu_va + address - QUSB_SMMU_BASE);
    barrier(0); *value = *p; barrier(0);
    return 1;
}
static int physical_write64(void *cookie, QUSB_U64 address, QUSB_U64 value)
{
    volatile QUSB_U64 *p;
    QUSB_U32 cr0;
    (void)cookie;
    if ((address & 7u) || address < QUSB_SMMU_BASE ||
        address - QUSB_SMMU_BASE > QUSB_SMMU_BYTES - 8u || !native.smmu_va || deadline_expired() ||
        !QcomUsbDomainWriteAllowed(&native.lease, address, value, 8) ||
        !physical_read(0, QUSB_SMMU_BASE, &cr0) ||
        (cr0 & 0x409u) != (native.lease.original_cr0 & 0x409u)) {
        native.status.io_failed = 1;
        native.status.error = QCOM_USB_DEBUG_CALLBACK_RANGE_FAILED;
        return 0;
    }
    /* One aligned volatile 64-bit access: never split a high TTBR into two
     * 32-bit writes with an intermediate truncated translation-table base. */
    p = (volatile QUSB_U64 *)(uintptr_t)(native.smmu_va + address - QUSB_SMMU_BASE);
    barrier(0); *p = value; barrier(0);
    return 1;
}
static uint32_t core_read(void *cookie, uint32_t offset)
{
    QUSB_U32 value;
    if (offset >= USB_BYTES || (offset & 3u) ||
        !physical_read(cookie, QUSB_DWC3_BASE + offset, &value)) return UINT32_MAX;
    if (usb_diag_polling && usb_diag_mode == 201u && offset == 0xc40cu)
        diag_reset(value == 0u ? 0u : (value <= 4096u && !(value & 3u)) ? 4u : 8u);
    return value;
}
static void core_write(void *cookie, uint32_t offset, uint32_t value)
{
    if (offset >= USB_BYTES || (offset & 3u)) {
        native.status.io_failed = 1;
        native.status.error = QCOM_USB_DEBUG_CALLBACK_RANGE_FAILED;
        return;
    }
    (void)physical_write(cookie, QUSB_DWC3_BASE + offset, value);
}
static void delay_us(void *cookie, uint32_t usec)
{
    uint64_t start, ticks, iterations;
    (void)cookie;
    if (!usec || deadline_expired()) return;
    /* Core and qualification delays are 1us or 1ms; refuse unbounded input. */
    if (usec > 10000u || !native.status.counter_frequency) {
        native.status.io_failed = 1;
        return;
    }
    start = counter();
    ticks = (native.status.counter_frequency * usec + 999999u) / 1000000u;
    iterations = (uint64_t)usec * 1024u + 1024u;
    while (counter() - start < ticks && iterations--) {
        if (deadline_expired()) return;
        __asm__ volatile("yield" ::: "memory");
    }
    if (counter() - start < ticks) native.status.io_failed = 1;
}
static int cache_sync(void *cpu, size_t bytes, enum dwc3_sync_direction direction,
                       const void *arena, size_t arena_bytes)
{
    uintptr_t address = (uintptr_t)cpu, start, end;
    uintptr_t base = (uintptr_t)arena;
    size_t line = (size_t)native.status.cache_line;
    if (!bytes) return 1;
    if (!line || address < base || address - base >= arena_bytes ||
        bytes > arena_bytes - (address - base)) {
        native.status.io_failed = 1;
        native.status.error = QCOM_USB_DEBUG_CALLBACK_RANGE_FAILED;
        return 0;
    }
    start = address & ~(line - 1u);
    end = (address + bytes + line - 1u) & ~(line - 1u);
    for (; start < end; start += line) {
        if (direction == DWC3_TO_DEVICE)
            __asm__ volatile("dc cvac, %0" :: "r"(start) : "memory");
        else
            __asm__ volatile("dc ivac, %0" :: "r"(start) : "memory");
    }
    barrier(0);
    return 1;
}
static void dma_sync(void *cookie, void *cpu, size_t bytes,
                     enum dwc3_sync_direction direction)
{
    (void)cookie;
    if (usb_diag_polling && usb_diag_mode == 202u && direction == DWC3_FROM_DEVICE)
        diag_reset(0);
    if (cache_sync(cpu, bytes, direction, usb_dma, sizeof(usb_dma)) &&
        usb_diag_polling && usb_diag_mode == 203u && direction == DWC3_FROM_DEVICE)
        diag_reset(0);
}
static int clean_tables(void *cookie, void *cpu, QUSB_U32 bytes)
{
    (void)cookie;
    if (deadline_expired()) return 0;
    return cache_sync(cpu, bytes, DWC3_TO_DEVICE, usb_tables, sizeof(usb_tables));
}
static void core_trace(void *cookie, enum dwc3_trace event, uint32_t a, uint32_t b)
{
    (void)cookie;
    native.status.last_trace = (uint32_t)event;
    native.status.trace_a = a; native.status.trace_b = b;
    native.status.traces++;
}
static int halt_inherited(void)
{
    QUSB_U32 control, status;
    unsigned int i;
    if (!physical_read(0, QUSB_DWC3_BASE + USB_DCTL, &control) ||
        !physical_write(0, QUSB_DWC3_BASE + USB_DCTL, control & ~USB_RUN_STOP)) return 0;
    for (i = 0; i < 10000u; i++) {
        if (!physical_read(0, QUSB_DWC3_BASE + USB_DCTL, &control) ||
            !physical_read(0, QUSB_DWC3_BASE + USB_DSTS, &status)) return 0;
        if (!(control & USB_RUN_STOP) && (status & USB_HALTED)) return 1;
        delay_us(0, 1);
    }
    return 0;
}
static int restore_lease(void)
{
    QUSB_DOMAIN_RESULT result;
    if (!native.lease.active) return 1;
    if (!halt_inherited()) return 0;
    result = QcomUsbDomainRestore(&native.domain_platform, &native.lease);
    native.status.restore_result = (uint32_t)result;
    native.status.dma_lease_active = native.lease.active;
    /* domain_* describe the install attempt and are never rewritten here: the
     * restore reuses the same lease, and clobbering them would replace the
     * failure evidence with the rollback's own last register access. */
    return result == QusbDomainOkay;
}
static int restore_identity(void)
{
    QUSB_RESULT result;
    if (!native.dma_lease.active) return 1;
    if (!halt_inherited()) return 0;
    result = QcomUsbRestoreDma(physical_read, physical_write, barrier, 0, &native.dma_lease);
    native.status.restore_result = (uint32_t)result;
    native.status.dma_lease_active = native.dma_lease.active;
    /* domain_* keep describing the install attempt; a failed commit leaves an
     * unrecognised value in the entry and RestoreDma reports OwnershipChanged
     * (18) without touching it. */
    return result == QusbQualified;
}
static int restore_bypass(void)
{
    QUSB_RESULT result;
    if (!native.bypass_lease.active) return 1;
    if (!halt_inherited()) return 0;
    result = QcomUsbBypassRestore(physical_read, physical_write, barrier, 0, &native.bypass_lease);
    native.status.restore_result = (uint32_t)result;
    native.status.dma_lease_active = native.bypass_lease.active;
    return result == QusbQualified;
}
static void capture_bypass_status(QUSB_RESULT result)
{
    native.status.domain_result = (uint32_t)result;
    native.status.domain_stage = native.bypass_lease.phase;
    native.status.domain_slot = native.bypass_lease.slot;
    native.status.domain_bank = native.bypass_lease.bank;
    native.status.domain_asid = native.bypass_lease.original_cr0;
    native.status.domain_last_address = native.bypass_lease.original_smr;
    native.status.domain_last_expected = native.bypass_lease.original_s2cr;
    native.status.domain_last_readback = native.bypass_lease.readback;
    native.status.dma_lease_active = native.bypass_lease.active;
}
static int acquire_vbus(void)
{
    QUSB_U32 saved, check;
    if (!halt_inherited() ||
        !physical_read(0, QUSB_DWC3_BASE + USB_HS_PHY_CTRL, &saved)) return 0;    native.status.vbus_saved = saved;
    if ((saved & USB_SW_VBUS_MASK) == USB_SW_VBUS_MASK) return 1;
    /* The pinned dwc3-msm-core.c uses QSCRATCH software session-valid/VBUS.
     * Mark ownership before a write because even a failed readback can leave
     * a side effect. Only these two bits will be restored after a proven halt. */
    native.status.vbus_owned = 1;
    if (!physical_write(0, QUSB_DWC3_BASE + USB_HS_PHY_CTRL, saved | USB_SW_VBUS_MASK) ||
        !physical_read(0, QUSB_DWC3_BASE + USB_HS_PHY_CTRL, &check)) return 0;
    return (check & USB_SW_VBUS_MASK) == USB_SW_VBUS_MASK;
}
static int restore_vbus(void)
{
    QUSB_U32 current, wanted, check;
    if (!native.status.vbus_owned) return 1;
    if (!halt_inherited() ||
        !physical_read(0, QUSB_DWC3_BASE + USB_HS_PHY_CTRL, &current)) return 0;
    /* Preserve all unrelated bits. If our failed set never took effect, the
     * original selected bits already prove no restoration write is needed. */
    if ((current & USB_SW_VBUS_MASK) == (native.status.vbus_saved & USB_SW_VBUS_MASK)) {
        native.status.vbus_owned = 0;
        return 1;
    }
    if ((current & USB_SW_VBUS_MASK) != USB_SW_VBUS_MASK) return 0;
    wanted = (current & ~USB_SW_VBUS_MASK) | (native.status.vbus_saved & USB_SW_VBUS_MASK);
    if (!physical_write(0, QUSB_DWC3_BASE + USB_HS_PHY_CTRL, wanted) ||
        !physical_read(0, QUSB_DWC3_BASE + USB_HS_PHY_CTRL, &check) ||
        (check & USB_SW_VBUS_MASK) != (wanted & USB_SW_VBUS_MASK)) return 0;
    native.status.vbus_owned = 0;
    return 1;
}
static int phy_core_halted(void)
{
    QUSB_U32 control, status;
    return physical_read(0, QUSB_DWC3_BASE + USB_DCTL, &control) &&
           physical_read(0, QUSB_DWC3_BASE + USB_DSTS, &status) &&
           !(control & USB_RUN_STOP) && (status & USB_HALTED);
}
static int acquire_phy(void)
{
    QUSB_U32 current, check;
    if (native.status.phy_owned || !phy_core_halted() ||
        !physical_read(0, QUSB_DWC3_BASE + USB_GUSB2PHYCFG, &current)) return 0;
    native.status.phy_saved = current & USB_PHY_KEEP_AWAKE_MASK;
    native.status.phy_applied = native.status.phy_restoring = 0;
    /* DT dis_u2_susphy requires bit6 clear. Holding bit8 clear is this
     * HS-only, non-LPM polling diagnostic's keep-awake policy. Save only the
     * selected controls and record a possible side effect before writing. */
    native.status.phy_owned = 1;
    if (!physical_write(0, QUSB_DWC3_BASE + USB_GUSB2PHYCFG,
                         current & ~USB_PHY_KEEP_AWAKE_MASK) ||
        !physical_read(0, QUSB_DWC3_BASE + USB_GUSB2PHYCFG, &check) ||
        (check & USB_PHY_KEEP_AWAKE_MASK)) return 0;
    native.status.phy_applied = 1;
    return 1;
}
static int restore_phy(void)
{
    QUSB_U32 current, selected, wanted, check;
    if (!native.status.phy_owned) return 1;
    if (native.status.stop_failed || native.status.vbus_owned || native.lease.active ||
        !phy_core_halted() ||
        !physical_read(0, QUSB_DWC3_BASE + USB_GUSB2PHYCFG, &current)) return 0;
    selected = current & USB_PHY_KEEP_AWAKE_MASK;
    if ((!native.status.phy_applied || native.status.phy_restoring) &&
        selected == native.status.phy_saved) {
        /* Failed acquire never changed the controls, or a prior restore
         * write completed despite its callback/readback failing. */
        native.status.phy_owned = native.status.phy_applied = native.status.phy_restoring = 0;
        return 1;
    }
    if (selected) return 0; /* Unexpected owner changed our active lease. */
    wanted = (current & ~USB_PHY_KEEP_AWAKE_MASK) | native.status.phy_saved;
    native.status.phy_restoring = 1;
    if (!physical_write(0, QUSB_DWC3_BASE + USB_GUSB2PHYCFG, wanted) ||
        !physical_read(0, QUSB_DWC3_BASE + USB_GUSB2PHYCFG, &check) ||
        (check & USB_PHY_KEEP_AWAKE_MASK) != native.status.phy_saved) return 0;
    native.status.phy_owned = native.status.phy_applied = native.status.phy_restoring = 0;
    return 1;
}
static int restore_after_failure(void)
{
    /* A failed stop must retain both leases and all static DMA buffers.
     * Never change VBUS or SMMU merely because another status bit looked idle. */
    if (native.status.stop_failed) return 0;
    if ((native.core_initialized && dwc3_stop(native.core)) || !halt_inherited()) {
        native.status.stop_failed = 1; return 0;
    }
    if (!restore_vbus() || !restore_identity() || !restore_bypass() || !restore_lease() || !restore_phy()) return 0;
    return 1;
}
static int dma_addressable(uint64_t physical, size_t bytes, uint32_t width)
{
    uint64_t last;
    if (!bytes || width < 32u || width > 64u || physical > UINT64_MAX - (bytes - 1u)) return 0;
    last = physical + bytes - 1u;
    return width == 64u || (last >> width) == 0;
}
static int validate_static_pages(const uint8_t *arena, size_t arena_bytes, uint64_t *address)
{
    uint64_t base, physical;
    size_t offset;
    if (!arena_bytes || (arena_bytes & (PAGE_BYTES - 1u))) return 0;
    base = (uint64_t)ml_static_vtop((vm_offset_t)(uintptr_t)arena);
    if ((base & (PAGE_BYTES - 1u)) || base > UINT64_MAX - arena_bytes) return 0;
    for (offset = 0; offset < arena_bytes; offset += PAGE_BYTES) {
        physical = (uint64_t)ml_static_vtop((vm_offset_t)(uintptr_t)(arena + offset));
        if (physical != base + offset) return 0;
        physical = (uint64_t)ml_static_vtop((vm_offset_t)(uintptr_t)(arena + offset + PAGE_BYTES - 1));
        if (physical != base + offset + PAGE_BYTES - 1) return 0;
    }
    *address = base;
    return 1;
}

int qcom_usb_debug_init(void)
{
    uint64_t flags, frequency, ctr;
    uint64_t dma_device;
    int enabled = 0, ret = 0;
    QUSB_RESULT result;
    QUSB_DOMAIN_RESULT domain_result;
    QUSB_INHERITED inherited;
    struct dwc3_platform platform;
    struct dwc3_identity identity = {
        QUD_USB_VID, QUD_USB_PID, "UniXNU", QUD_USB_PRODUCT, QUD_USB_SERIAL, 0u
    };
    int second_port = 0;
    /* T131: default off.  Parsed before the identity is handed to dwc3_init so
     * the descriptor set the host sees is decided by this single boot-arg. */
    (void)PE_parse_boot_argn("nabu-cdc-second-port", &second_port, sizeof(second_port));
    cdc_second_port = second_port >= 1;
    kdp_pipe1_mode = second_port >= 2;
    identity.options = cdc_second_port ? DWC3_ID_OPTION_SECOND_PORT : 0u;
    if (native.status.phase == QCOM_USB_DEBUG_NOT_INITIALIZED) {
        int diagnostic = 0;
        if (QUD_PSCI_SMC_RESET &&
            PE_parse_boot_argn("qcom-usb-diag", &diagnostic, sizeof(diagnostic)) &&
            (diagnostic == 1 || diagnostic == 2 || (diagnostic >= 101 && diagnostic <= 119) ||
             (diagnostic >= 201 && diagnostic <= 205))) {
            usb_diag_enabled = 1;
            usb_diag_mode = (unsigned int)diagnostic;
            native.status.diag_enabled = 1;
            usb_diag_target = diagnostic >= 101 && diagnostic <= 119 ? (unsigned int)diagnostic - 100u : 0u;
            diag_checkpoint(QCOM_USB_DEBUG_DIAG_BEFORE_ACQUIRE);
            /* Mode2 measures this exact kernel/handoff path's zero-delay
             * reboot baseline before interpreting mode1's encoded delays. */
            if (diagnostic == 2) diag_reset(0);
        }
    }
    if (!acquire(&flags)) {
        if (usb_diag_enabled && native.status.phase == QCOM_USB_DEBUG_NOT_INITIALIZED) {
            fail(QCOM_USB_DEBUG_ACQUIRE_REFUSED, 0);
            diag_reset(12u + 4u * native.status.error);
        }
        return 0;
    }
    if (native.status.phase != QCOM_USB_DEBUG_NOT_INITIALIZED) {
        ret = native.status.phase == QCOM_USB_DEBUG_ACTIVE; goto out;
    }
    if (!PE_parse_boot_argn("qcom-usb-debug", &enabled, sizeof(enabled)) || enabled != 1) {
        native.status.phase = QCOM_USB_DEBUG_DISABLED; goto out;
    }
    {
        int forensics = 0;
        (void)PE_parse_boot_argn("nabu-verbs-forensics", &forensics, sizeof(forensics));
        usb_forensics_verbs = forensics == 1;
    }
    /* T142: optional trace ring / telemetry store.  Parsed only on the
     * enabled path; both gates stay 0 (refuse) without their boot-arg. */
    {
        int trace_gate = 0, tel_gate = 0;
        (void)PE_parse_boot_argn("nabu-trace", &trace_gate, sizeof(trace_gate));
        (void)PE_parse_boot_argn("nabu-tel", &tel_gate, sizeof(tel_gate));
        qud_trace_bootarg_gate = trace_gate == 1;
        qud_tel_bootarg_gate = tel_gate == 1;
    }
    native.status.phase = QCOM_USB_DEBUG_PREPARING;
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_AFTER_ACQUIRE);
    if (dwc3_context_bytes() > sizeof(usb_context)) {
        fail(QCOM_USB_DEBUG_CONTEXT_TOO_SMALL, 0); goto out;
    }
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_COUNTER_CACHE);
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    if (usb_diag_mode == 204u)
        diag_reset(frequency == UINT64_C(19200000) ? 0u :
                   frequency == UINT64_C(24000000) ? 4u :
                   frequency == UINT64_C(1000000000) ? 8u : 12u);
    if (frequency < 1000u || frequency > COUNTER_MAX_FREQUENCY) {
        fail(QCOM_USB_DEBUG_BAD_COUNTER, 0); goto out;
    }
    if (usb_diag_mode == 205u) {
        uint64_t v0 = counter(), p0, p1, vd, pd;
        unsigned int samples = 100000000u;
        __asm__ volatile("isb\nmrs %0, cntpct_el0" : "=r"(p0) :: "memory");
        while (samples-- && counter() - v0 < frequency / 4u) __asm__ volatile("yield" ::: "memory");
        __asm__ volatile("isb\nmrs %0, cntpct_el0" : "=r"(p1) :: "memory");
        vd = counter() - v0; pd = p1 - p0;
        /*4s=matching progress;8s=physical frozen;12s=different rate;
         *16s=virtual sampling budget exhausted. Waits use virtual time. */
        diag_reset(vd < frequency / 4u ? 16u : !pd ? 8u :
                   (pd > vd ? pd - vd : vd - pd) > vd / 10u ? 12u : 4u);
    }
    native.status.counter_frequency = frequency;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    native.status.cache_line = UINT64_C(4) << ((ctr >> 16) & 15u);
    if (native.status.cache_line > 64u || native.status.cache_line < 16u) {
        fail(QCOM_USB_DEBUG_CACHE_GRANULE_UNSUPPORTED, 0); goto out;
    }
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_DATA_VTOP);
    if (!validate_static_pages(usb_dma, sizeof(usb_dma), &native.status.dma_pa)) {
        fail(QCOM_USB_DEBUG_DMA_NOT_CONTIGUOUS, 0); goto out;
    }
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_TABLE_VTOP);
    if (!validate_static_pages(usb_tables, sizeof(usb_tables), &native.status.tables_pa)) {
        fail(QCOM_USB_DEBUG_TABLES_NOT_CONTIGUOUS, 0); goto out;
    }
    native.status.dma_iova = QUSB_DOMAIN_IOVA;
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_MAP_GCC);
    native.gcc_va = ml_io_map((vm_offset_t)QUSB_GCC_BASE, GCC_BYTES);
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_MAP_USB);
    native.usb_va = ml_io_map((vm_offset_t)QUSB_DWC3_BASE, USB_BYTES);
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_MAP_SMMU);
    native.smmu_va = ml_io_map((vm_offset_t)QUSB_SMMU_BASE, QUSB_SMMU_BYTES);
    if (!native.gcc_va || !native.usb_va || !native.smmu_va) {
        fail(QCOM_USB_DEBUG_MAP_FAILED, 0); goto out;
    }
    /* Optional: bring the controller up to device mode first. ABL hands it over
     * in host mode, so without this the inherited-state gate below refuses. */
    {
        int prepare = 0;
        (void)PE_parse_boot_argn("qcom-usb-prepare", &prepare, sizeof(prepare));
        if (prepare == 1) {
            QCOM_PREPARE_PLATFORM platform;
            QCOM_PREPARE_RESULT prepared;
            uint32_t stage = 0, readback = 0;

            native.phy_va = ml_io_map((vm_offset_t)QUD_PHY_BASE, PHY_MAP_BYTES);
            if (!native.phy_va) {
                fail(QCOM_USB_DEBUG_MAP_FAILED, 0); goto out;
            }
            bytes_zero(&platform, sizeof(platform));
            platform.read32 = physical_read;
            platform.write32 = prepare_write;
            platform.delay_us = delay_us;
            platform.deadline = native.deadline_active ? native.deadline : 0;
            prepared = QcomUsbPrepare(&platform, &stage, &readback);
            native.status.prepare_result = (uint32_t)prepared;
            native.status.prepare_stage = stage;
            native.status.prepare_readback = readback;
            if (prepared != QPREP_OK) {
                fail(QCOM_USB_DEBUG_PREPARE_FAILED, (int)prepared); goto out;
            }
        }
    }
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_INSPECT);
    result = QcomUsbInspectInherited(physical_read, 0, &inherited);
    native.status.qcom_result = (uint32_t)result;
    if (result != QusbQualified) {
        /* In particular do not wake a clock-gated old DMA engine: the native
         * kernel cannot prove ABL's original DMA buffers are still retained. */
        fail(QCOM_USB_DEBUG_INHERITED_STATE_REFUSED, 0); goto out;
    }
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_GATES_WIDTH);
    /* GDSCR HW_CONTROL is bit1; CFG_GDSCR.POWER_UP_COMPLETE is a separate
     * register's bit16, already checked by QcomUsbInspectInherited. */
    if (inherited.gcc_gdscr & 3u) {
        fail(QCOM_USB_DEBUG_UNSTABLE_POWER_STATE, 0); goto out;
    }
    if (((inherited.gctl >> 12) & 3u) != 2u) {
        fail(QCOM_USB_DEBUG_CORE_NOT_DEVICE, 0); goto out;
    }
    {
        QUSB_U32 hwparams0;
        if (!physical_read(0, QUSB_DWC3_BASE + USB_GHWPARAMS0, &hwparams0)) {
            fail(QCOM_USB_DEBUG_DMA_WIDTH_UNSUPPORTED, 0); goto out;
        }
        native.status.hwparams0 = hwparams0;
        /* Linux drivers/usb/dwc3/core.h DWC3_GHWPARAMS0_AWIDTH and TI
         * SPRUHJ7A table3-91: bits31:24 are master address width in bits.
         * DWC3 sees only the fixed IOVA span. The domain backend separately
         * checks high physical DMA/table addresses against SMMU output width.
         * No truncation, identity mapping or BYPASS fallback is used. */
        native.status.dma_address_width = (hwparams0 >> 24) & 0xffu;
        if (!dma_addressable(native.status.dma_iova, sizeof(usb_dma), native.status.dma_address_width)) {
            fail(QCOM_USB_DEBUG_DMA_WIDTH_UNSUPPORTED, 0); goto out;
        }
    }
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_HALT);
    if (!halt_inherited()) { fail(QCOM_USB_DEBUG_HALT_TIMEOUT, 0); goto out; }
    bytes_zero(&native.domain_platform, sizeof(native.domain_platform));
    native.domain_platform.read32 = physical_read;
    native.domain_platform.write32 = physical_write;
    native.domain_platform.read64 = physical_read64;
    native.domain_platform.write64 = physical_write64;
    native.domain_platform.barrier = barrier;
    native.domain_platform.delay_us = delay_us;
    native.domain_platform.clean_tables = clean_tables;
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_DOMAIN);
    dma_device = native.status.dma_iova;
    if (QUD_DMA_PREFER_IDENTITY) {
        QUSB_RESULT dma_result;
        if (QUD_DMA_BYPASS_BANK) {
            /* This firmware rejects a TYPE=BYPASS S2CR (it reads back as
             * FAULT), so route the stream at a pass-through context bank the
             * way the board's own UFS takeover does: SCTLR=0,
             * CBAR=S1_TRANS_S2_BYPASS, CBA2R.VA64=1, selected by an ordinary
             * TYPE=TRANSLATE S2CR. Reads of these registers do not return the
             * written value, so the verdict is left to real DMA traffic. */
            native.status.dma_mode = QCOM_USB_DEBUG_DMA_BYPASS_BANK;
            dma_result = QcomUsbBypassPlan(physical_read, 0, native.status.dma_pa,
                                           (QUSB_U32)sizeof(usb_dma), &native.bypass_lease);
            if (dma_result == QusbQualified)
                dma_result = QcomUsbBypassEnter(physical_read, physical_write, barrier, 0,
                                                &native.bypass_lease);
            capture_bypass_status(dma_result);
            if (dma_result != QusbQualified || native.status.io_failed) {
                fail(QCOM_USB_DEBUG_DMA_REFUSED, 0); (void)restore_bypass(); goto out;
            }
        } else {
            /* The bootloader already routes this SID, so a private translated
             * domain cannot be installed (StreamExists). Use the module's
             * audited identity lease: reuse an inherited bypass, or rewrite the
             * existing translated pair to TYPE-only bypass saving the original,
             * and program the core with the static buffer's physical address. */
            QUSB_SMMU_STATE smmu;
            bytes_zero(&smmu, sizeof(smmu));
            dma_result = QcomUsbEnterIdentityDma(physical_read, physical_write, barrier, 0,
                                                 &native.dma_lease, &smmu);
            /* The status struct is shared verbatim with the kernel adapter, so
             * the identity outcome is reported through the existing fields. */
            native.status.domain_result = (uint32_t)dma_result;
            native.status.domain_stage = native.dma_lease.phase;
            native.status.domain_slot = native.dma_lease.index;
            native.status.domain_last_address = native.dma_lease.smr;
            native.status.domain_last_expected = native.dma_lease.original_s2cr;
            native.status.domain_last_readback = native.dma_lease.readback;
            native.status.dma_lease_active = native.dma_lease.active;
            {
                /* GR0_IDR0 proves the SMMU is alive, sACR says whether EL1 may
                 * write the stream table at all. */
                QUSB_U32 idr0 = 0, sacr = 0;
                if (!physical_read(0, QUSB_SMMU_BASE + 0x20u, &idr0) ||
                    !physical_read(0, QUSB_SMMU_BASE + 0x10u, &sacr)) {
                    native.status.io_failed = 1;
                }
                native.status.domain_bank = idr0;
                native.status.domain_asid = sacr;
            }
            /* Mark the path before checking the result, so a failure is
             * reported as an identity-lease failure (QUSB_RESULT) and not
             * misread as a QUSB_DOMAIN_RESULT from the private backend. */
            native.status.dma_mode = QCOM_USB_DEBUG_DMA_INHERITED_IDENTITY;
            if (dma_result != QusbQualified || native.status.io_failed) {
                fail(QCOM_USB_DEBUG_DMA_REFUSED, 0); (void)restore_identity(); goto out;
            }
        }
        if (!dma_addressable(native.status.dma_pa, sizeof(usb_dma), native.status.dma_address_width)) {
            fail(QCOM_USB_DEBUG_DMA_WIDTH_UNSUPPORTED, 0);
            if (QUD_DMA_BYPASS_BANK) (void)restore_bypass(); else (void)restore_identity();
            goto out;
        }
        dma_device = native.status.dma_pa;
        diag_checkpoint(QCOM_USB_DEBUG_DIAG_VERIFY_DOMAIN);
    }
    if (native.status.dma_mode == QCOM_USB_DEBUG_DMA_NONE) {
        domain_result = QcomUsbDomainInstall(&native.domain_platform, usb_tables, native.status.tables_pa,
                                             sizeof(usb_tables), native.status.dma_pa, sizeof(usb_dma), &native.lease);
        native.status.domain_result = (uint32_t)domain_result;
        native.status.domain_stage = native.lease.stage;
        native.status.domain_bank = native.lease.cb;
        native.status.domain_slot = native.lease.index;
        native.status.domain_asid = native.lease.asid;
        native.status.domain_last_address = native.lease.last_address;
        native.status.domain_last_expected = native.lease.last_expected;
        native.status.domain_last_readback = native.lease.last_readback;
        native.status.dma_lease_active = native.lease.active;
        if (domain_result != QusbDomainOkay) {
            fail(QCOM_USB_DEBUG_DMA_REFUSED, 0); (void)restore_lease(); goto out;
        }
        diag_checkpoint(QCOM_USB_DEBUG_DIAG_VERIFY_DOMAIN);
        {
            QUSB_U32 sctlr, s2cr;
            if (!native.lease.active || native.status.io_failed ||
                native.lease.iova != native.status.dma_iova ||
                native.lease.dma_pa != native.status.dma_pa || native.lease.table_pa != native.status.tables_pa ||
                !in_range(native.lease.cb_base, QUSB_SMMU_BASE, QUSB_SMMU_BYTES) ||
                native.lease.index >= native.lease.stream_count ||
                !physical_read(0, native.lease.cb_base, &sctlr) || !(sctlr & 1u) ||
                !physical_read(0, QUSB_SMMU_BASE + 0xc00u + 4u * native.lease.index, &s2cr) ||
                ((s2cr >> 16) & 3u) != 0u || (s2cr & 0xffu) != native.lease.cb) {
                fail(QCOM_USB_DEBUG_DOMAIN_STATE_INVALID, 0); (void)restore_lease(); goto out;
            }
        }
        native.status.dma_mode = QCOM_USB_DEBUG_DMA_PRIVATE_TRANSLATED;
    }
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_VBUS);
    if (!acquire_phy()) {
        fail(QCOM_USB_DEBUG_PHY_LEASE_FAILED, 0); (void)restore_after_failure(); goto out;
    }
    if (!acquire_vbus()) {
        fail(QCOM_USB_DEBUG_VBUS_FAILED, 0); (void)restore_after_failure(); goto out;
    }
    bytes_zero(&platform, sizeof(platform));
    platform.read32 = core_read; platform.write32 = core_write;
    platform.delay_us = delay_us; platform.dma_sync = dma_sync;
    platform.barrier = barrier; platform.trace = core_trace;
    native.core = (struct dwc3 *)(void *)usb_context;
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_CORE_INIT);
    ret = dwc3_init(usb_context, sizeof(usb_context), &platform, &identity, usb_dma,
                         dma_device, sizeof(usb_dma));
    if (ret || native.status.io_failed) {
        fail(QCOM_USB_DEBUG_CORE_INIT_FAILED, ret); (void)restore_after_failure(); ret = 0; goto out;
    }
    native.core_initialized = 1;
    {
        static const char banner[] = "QCOM XNU USB CDC ACM v1\n";
        (void)backlog_write(banner, sizeof(banner) - 1);
    }
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_CORE_START);
    ret = dwc3_start(native.core);
    if (ret || native.status.io_failed) {
        fail(QCOM_USB_DEBUG_CORE_START_FAILED, ret);
        (void)restore_after_failure(); ret = 0; goto out;
    }
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_CORE_STARTED);
    usb_diag_polling = 1;
    /* Let the host complete EP0 enumeration before early kernel startup can
     * reach a path with no more log output. Once-per-character polling alone
     * may stop before the host has even requested the device descriptor.
     * This is bounded without installing an asynchronous worker or IRQ. */
    {
        uint64_t start = counter();
        unsigned int attempts = 50000u;
        while (attempts-- && counter() - start < frequency * 5u) {
            int events;
            backlog_drain_owned();
            events = dwc3_poll(native.core, 64);
            native.status.poll_calls++;
            native.status.last_poll_events = events < 0 ? 0u : (uint32_t)events;
            if (events < 0 || native.status.io_failed) {
                usb_diag_polling = 0;
                fail(QCOM_USB_DEBUG_CORE_POLL_FAILED, events);
                (void)restore_after_failure(); ret = 0; goto out;
            }
            if (native.status.poll_calls == 1u) diag_checkpoint(QCOM_USB_DEBUG_DIAG_FIRST_POLL);
            if (dwc3_ready(native.core)) break;
            delay_us(0, 100);
        }
    }
    usb_diag_polling = 0;
    if (usb_diag_mode >= 201u && usb_diag_mode <= 203u) diag_reset(12);
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_ENUM_DONE);
    native.status.phase = QCOM_USB_DEBUG_ACTIVE;
    /* T175: one identity line per port, printed once after enumeration.  The
     * console line always appears; the log/KDP line only exists when the second
     * pipe is enabled.  Both use the historical output channel, and a full or
     * absent ring cannot block startup. */
    {
        static const char port_console[] = "NABU PORT role=console if=0\n";
        static const char port_log[] = "NABU PORT role=log if=1\n";
        (void)qcom_usb_debug_write(port_console, sizeof(port_console) - 1u);
        if (cdc_second_port)
            (void)qcom_usb_debug_write(port_log, sizeof(port_log) - 1u);
    }
    ret = 1;
out:
    buffer_status_owned();
    release(flags);
    if (usb_diag_enabled && native.status.phase == QCOM_USB_DEBUG_REFUSED)
        diag_reset(12u + 4u * native.status.error);
    return ret;
}

static int poll_owned(void)
{
    int ret;
    if (native.status.phase != QCOM_USB_DEBUG_ACTIVE) return 0;
    backlog_drain_owned();
    loss_notice_owned();
    /* kb-32: drain more events per call while the kernel is in the debugger.
     *
     * pump() refuses to start an IN transfer while d->active[EP_IN] is set, and
     * that flag only clears when a XFERCOMPLETE event is handled. With budget=1
     * a single poll retires one event, so a reply longer than one packet leaves
     * active[EP_IN] set across calls and the next pump() bails -- the rest of
     * the reply just sits in the 16 KiB tx ring.
     *
     * Measured three times, always on the third reply of a session: kb-30
     * HOSTINFO(seq=2) stopped after 24 B, kb-31 READMEM64(seq=2) stopped after
     * ~30 B, while the same session's second reply (69 B) completed. That is
     * event starvation, not a controller fault.
     *
     * 64 is still bounded (dwc3_poll clamps at 256) and every event it handles
     * is the same work a single event was: rearm an endpoint or copy a
     * completed transfer.
     *
     * Outside the debugger the budget used to be 1, on the theory that the 5 ms
     * tick paced the worker and a larger budget only added latency under the USB
     * lock. That was wrong in a way the throughput measurement showed: a 1 KiB
     * reply is one tx chunk but arrives as several IN transfers, and with
     * budget=1 a single poll retires one event, so the next pump() still saw
     * active[EP_IN] set and had to wait for the following tick. Measured 2.7
     * ticks per 1 KiB packet. 8 lets one poll both retire the completions and
     * start the next transfer, which is what makes the whole packet move in one
     * tick. It stays far below the 256 clamp. */
    ret = dwc3_poll(native.core, kdp_session_active() ? 64u : 8u);
    native.status.poll_calls++;
    native.status.last_poll_events = ret < 0 ? 0u : (uint32_t)ret;
    buffer_status_owned();
    if (ret < 0 || native.status.io_failed) {
        native.status.core_result = ret;
        native.status.error = native.deadline_active && native.deadline_expired ? QCOM_USB_DEBUG_DEADLINE_EXPIRED :
                                                      QCOM_USB_DEBUG_CORE_POLL_FAILED;
        /* kb-28 (T41-a / T41-b): do NOT latch the phase while the kernel is
         * inside the debugger.
         *
         * phase = IO_FAILED is a one-way door: nothing in this file ever
         * restores it (only stop/init paths set a different phase, and neither
         * runs from the debugger). Every later qcom_usb_debug_write() then
         * short-circuits on the phase check and only backlog_write()s, so all
         * output is absorbed into the 4 MiB ring and never reaches the wire.
         *
         * That is the most likely explanation for the "random silence" T41-b
         * found: the entry banner is the first dense burst after the trap, so
         * the controller is most likely mid-transfer or mid-EP0 when the other
         * cores get halted, and one transient dwc3_poll() error would kill the
         * rest of the session's output. kb-17u/kb-17s/kb-17q stopped at
         * "Options.....    Type"; the identical kb-17v rerun printed all four
         * lines and "Continuing...".
         *
         * A transient error in this context is recoverable: the next
         * dwc3_poll() rearms EP_OUT and retries the IN transfer. So record the
         * error, skip the latch, and report failure to this one caller only.
         * Outside the debugger the old behaviour stays -- a real controller
         * fault must still stop the worker instead of polling a dead core. */
        if (!kdp_session_active()) {
            native.status.phase = QCOM_USB_DEBUG_IO_FAILED;
        } else {
            /* io_failed is just as one-way as phase, and it is the other half
             * of this same test, so clear it too -- otherwise the guard above
             * would never fire again: the very next call would take the
             * `|| native.status.io_failed` branch and short-circuit. The
             * counters keep the evidence for later inspection. */
            native.status.io_failed = 0;
        }
        return 0;
    }
    return 1;
}
static uint64_t control_mix(uint64_t value)
{
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}
static void control_rotate_session_owned(void)
{
    qcom_usb_shot_cancel();
    uint64_t next = 0;
    unsigned int attempt;
    for (attempt = 0; attempt < 4u; attempt++) {
        usb_control_context.seed += UINT64_C(0x9e3779b97f4a7c15);
        next = control_mix(counter() ^ usb_control_context.seed);
        if (!next) next = UINT64_C(0x53584e55314b4559); /* "SXNU1KEY" */
        if (next != usb_control_context.state.session) break;
    }
    /* A transport generation change must never restart sequence numbering in
     * the same session. The bounded fallback also covers a theoretical mix
     * collision with the previous value. */
    if (next == usb_control_context.state.session)
        next = next == UINT64_MAX ? 1u : next + 1u;
    sxnu_control_state_reset(&usb_control_context.state, next,
                             usb_control_context.state.generation);
    sxnu_control_parser_reset(&usb_control_context.parser);
}
static size_t control_reply_queue(const char *reply, size_t bytes, unsigned int pipe)
{
    uint8_t wire[SXNU_CONTROL_REPLY_BYTES + 1u];
    uint64_t flags;
    int queued = 0;
    if (!reply || !bytes || bytes > SXNU_CONTROL_REPLY_BYTES ||
        (pipe != 0u && pipe != 1u) || !acquire(&flags)) return 0;
    if (native.status.phase == QCOM_USB_DEBUG_ACTIVE) {
        backlog_drain_owned();
        if (dwc3_tx_free_pipe(native.core, pipe) >= bytes + 1u) {
            wire[0] = (uint8_t)'\n';
            bytes_copy(wire + 1u, reply, bytes);
            if (dwc3_queue_pipe(native.core, pipe, wire, bytes + 1u) == bytes + 1u) queued = 1;
        }
    }
    release(flags);
    if (queued) (void)__atomic_add_fetch(&control_reply_count, 1u, __ATOMIC_RELAXED);
    return queued ? bytes : 0;
}
static size_t control_reply_hello(char *reply, size_t capacity,
                                  const struct sxnu_control_frame *frame,
                                  unsigned int pipe)
{
    struct sxnu_control_writer writer;
    sxnu_control_writer_init(&writer, reply, capacity);
    if (!sxnu_control_reply_start(&writer, usb_control_context.state.session, frame->seq,
                                  frame->verb, "OK") ||
        !sxnu_control_writer_text(&writer, " session=") ||
        !sxnu_control_writer_hex(&writer, usb_control_context.state.session,
                                 SXNU_CONTROL_SESSION_HEX) ||
        !sxnu_control_writer_text(&writer, " last_seq=") ||
        !sxnu_control_writer_hex(&writer, usb_control_context.state.last_seq,
                                 SXNU_CONTROL_SEQ_HEX) ||
        !sxnu_control_writer_text(&writer, " control=1") ||
        !sxnu_control_writer_text(&writer, " role=") ||
        !sxnu_control_writer_decimal(&writer, pipe)) return 0;
    return sxnu_control_reply_finish(&writer);
}

/* T175: identify the log/KDP port.  serial_rx_drain() routes pipe 1 bytes to
 * the KDP detectors only, so a HELLO sent there would never be answered and a
 * host could not tell the two COM ports apart.  This mirror answers a
 * session==0/seq==0 HELLO on pipe 1 with the same read-only body as the console
 * reply plus role=1.  It keeps its own parser instance and never touches
 * usb_control_context.parser/state, so pipe-0 sequencing is unaffected. */
static void control_pipe1_hello_scan(const uint8_t *bytes, size_t count)
{
    /* Persistent across chunks: a HELLO split by the USB transfer boundary is
     * still recognised, exactly like the console parser in
     * usb_control_context.parser.  Zero-initialised in .bss. */
    static struct sxnu_control_parser parser;
    struct sxnu_control_frame frame;
    char line[SXNU_CONTROL_FRAME_BYTES];
    char reply[SXNU_CONTROL_REPLY_BYTES];
    size_t line_bytes = 0;
    size_t reply_bytes;
    size_t i;

    for (i = 0; i < count; i++) {
        uint8_t b = bytes[i];
        int complete = sxnu_control_parser_feed(&parser, b, line,
                                                 sizeof(line), &line_bytes);
        if (complete <= 0) continue;
        if (!sxnu_control_parse(line, line_bytes, &frame)) continue;
        if (frame.verb != SXNU_CONTROL_VERB_HELLO ||
            frame.session != 0u || frame.seq != 0u) continue;
        reply_bytes = control_reply_hello(reply, sizeof(reply), &frame, 1u);
        if (reply_bytes) (void)control_reply_queue(reply, reply_bytes, 1u);
    }
}
static size_t control_reply_ping(char *reply, size_t capacity,
                                 const struct sxnu_control_frame *frame,
                                 const struct qcom_usb_debug_status *status)
{
    struct sxnu_control_writer writer;
    sxnu_control_writer_init(&writer, reply, capacity);
    if (!sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK") ||
        !sxnu_control_writer_text(&writer, " rx=") ||
        !sxnu_control_writer_decimal(&writer, status->rx_pending) ||
        !sxnu_control_writer_text(&writer, " rxo=") ||
        !sxnu_control_writer_decimal(&writer, status->rx_overflow) ||
        !sxnu_control_writer_text(&writer, " tx=") ||
        !sxnu_control_writer_decimal(&writer, status->transport_pending) ||
        !sxnu_control_writer_text(&writer, " poll=") ||
        !sxnu_control_writer_decimal(&writer, status->poll_calls)) return 0;
    return sxnu_control_reply_finish(&writer);
}
static size_t control_reply_status(char *reply, size_t capacity,
                                   const struct sxnu_control_frame *frame,
                                   const struct qcom_usb_debug_status *status)
{
    struct sxnu_control_writer writer;
    sxnu_control_writer_init(&writer, reply, capacity);
    if (!sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK") ||
        !sxnu_control_writer_text(&writer, " usb=") ||
        !sxnu_control_writer_decimal(&writer, status->phase) ||
        !sxnu_control_writer_text(&writer, " er=") ||
        !sxnu_control_writer_decimal(&writer, status->error) ||
        !sxnu_control_writer_text(&writer, " rx=") ||
        !sxnu_control_writer_decimal(&writer, status->rx_pending) ||
        !sxnu_control_writer_text(&writer, " rxo=") ||
        !sxnu_control_writer_decimal(&writer, status->rx_overflow) ||
        !sxnu_control_writer_text(&writer, " tx=") ||
        !sxnu_control_writer_decimal(&writer, status->transport_pending) ||
        !sxnu_control_writer_text(&writer, " bp=") ||
        !sxnu_control_writer_decimal(&writer, status->buffer_pending) ||
        !sxnu_control_writer_text(&writer, " bo=") ||
        !sxnu_control_writer_decimal(&writer, status->buffer_overflow) ||
        !sxnu_control_writer_text(&writer, " poll=") ||
        !sxnu_control_writer_decimal(&writer, status->poll_calls) ||
        !sxnu_control_writer_text(&writer, " ev=") ||
        !sxnu_control_writer_decimal(&writer, status->last_poll_events) ||
        !sxnu_control_writer_text(&writer, " gen=") ||
        !sxnu_control_writer_decimal(&writer, status->rx_generation) ||
        !sxnu_control_writer_text(&writer, " rej=") ||
        !sxnu_control_writer_decimal(&writer, status->control_rejected) ||
        !sxnu_control_writer_text(&writer, " rl=") ||
        !sxnu_control_writer_decimal(&writer, __atomic_load_n(&control_reply_lost, __ATOMIC_RELAXED)) ||
        !sxnu_control_writer_text(&writer, " krs=") ||
        !sxnu_control_writer_decimal(&writer, __atomic_load_n(&kdp_release_resyncs, __ATOMIC_RELAXED)) ||
        !sxnu_control_writer_text(&writer, " kdd=") ||
        !sxnu_control_writer_decimal(&writer, __atomic_load_n(&kdp_death_count, __ATOMIC_RELAXED)) ||
        !sxnu_control_writer_text(&writer, " kgen=") ||
        !sxnu_control_writer_decimal(&writer, __atomic_load_n(&kdp_generation_changes, __ATOMIC_RELAXED)) ||
        !sxnu_control_writer_text(&writer, " kp1=") ||
        !sxnu_control_writer_decimal(&writer, __atomic_load_n(&kdp_pipe1_enters, __ATOMIC_RELAXED)) ||
        !sxnu_control_writer_text(&writer, " kp1d=") ||
        !sxnu_control_writer_decimal(&writer, __atomic_load_n(&kdp_pipe1_stray, __ATOMIC_RELAXED))) return 0;
    /* T150: DEVTREE job visibility, appended only when the new gate is on so
     * the default-off STATUS reply stays byte-for-byte identical. */
    if (qud_dt_thread_enabled()) {
        uint64_t now = counter();
        uint32_t ms = 0;
        if (qud_dt_job.submit_tick && now > qud_dt_job.submit_tick &&
            native.status.counter_frequency)
            ms = (uint32_t)((now - qud_dt_job.submit_tick) * 1000u /
                            native.status.counter_frequency);
        (void)sxnu_control_writer_text(&writer, " dt=");
        (void)sxnu_control_writer_decimal(&writer,
            __atomic_load_n(&qud_dt_job.state, __ATOMIC_RELAXED));
        (void)sxnu_control_writer_text(&writer, " dtms=");
        (void)sxnu_control_writer_decimal(&writer, ms);
    }
    return sxnu_control_reply_finish(&writer);
}
static size_t control_reply_reboot(char *reply, size_t capacity,
                                   const struct sxnu_control_frame *frame,
                                   const char *status, const char *reason)
{
    struct sxnu_control_writer writer;
    sxnu_control_writer_init(&writer, reply, capacity);
    if (!sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, status) ||
        !sxnu_control_writer_text(&writer, " state=") ||
        !sxnu_control_writer_text(&writer, reason)) return 0;
    return sxnu_control_reply_finish(&writer);
}
/* Split the argument span into up to max whitespace-separated tokens. */
static unsigned
control_args_split(const uint8_t *text, size_t bytes, const uint8_t **tok,
                   size_t *len, unsigned max)
{
    unsigned count = 0;
    size_t i = 0;
    if (!text || !bytes) return 0;
    while (i < bytes && count < max) {
        while (i < bytes && text[i] == (uint8_t)' ') i++;
        if (i >= bytes) break;
        const size_t start = i;
        while (i < bytes && text[i] != (uint8_t)' ') i++;
        tok[count] = text + start;
        len[count] = i - start;
        count++;
    }
    return count;
}

/* Accept bare hex and 0x-prefixed hex. */
static int
control_args_hex(const uint8_t *text, size_t bytes, uint64_t *value)
{
    if (bytes > 2u && text[0] == (uint8_t)'0' &&
        (text[1] == (uint8_t)'x' || text[1] == (uint8_t)'X')) {
        text += 2;
        bytes -= 2;
    }
    return sxnu_control_parse_hex(text, bytes, value);
}


/* T142-trace: begin */
/* ---- T142: periodic trace ring and telemetry-store verbs ------------------
 *
 * TRACEON/TRACEREAD/TRACECLEAR sample this console's own health into a
 * private ring, so a crash leaves a time-ordered tail even when no operator
 * was watching the port.  TELDUMP/TELREAD are the interface skeleton for the
 * NABUTEL1 store designed in T103: they call the weak native_ufs_tel_*
 * exports the storage driver is expected to add (PLAN.md 4.2).  Until those
 * exports exist both answer state=unsupported -- nothing is guessed and no
 * medium is written.
 *
 * Where the memory lives: qud_trace_ring is kernel BSS, 512 records x 16
 * bytes = 8 KiB fixed at link time.  No allocation, no scheduler, no
 * interrupt dependency; the only writer is the USB worker tick, so the ring
 * cannot fail in a way that disturbs the console.
 *
 * Default off: every verb refuses with state=disabled unless nabu-trace=1
 * (ring) or nabu-tel=1 (store).  With the boot-args absent nothing is sampled,
 * written or cleared, so this patch is inert on a device that does not opt in.
 *
 * Record layout (16 bytes, little-endian):
 *   ts   8B  CNTVCT_EL0 ticks -- the same base UPTIME reports; monotonic
 *   type 4B  QUD_TRACE_TYPE_*
 *   arg  4B  class-specific scalar
 *
 * Event classes (TRACEON mask bits):
 *   0x1 tick      one record per interval; arg = poll_calls
 *   0x2 pressure  buffer/backlog pressure changes only; arg = overflow|rxo<<16
 *   0x4 control   control-reject count changes only; arg = control_rejected
 *   0x8 error     phase/error changes only; arg = error|phase<<16
 * The change-only classes keep a quiet console from filling the ring with
 * identical rows; an event is still recorded at the next interval boundary.
 */
#define QUD_TRACE_RECORDS_MAX   512u
#define QUD_TRACE_READ_MAX      10u
#define QUD_TRACE_TYPES         UINT32_C(0xf)
#define QUD_TRACE_INTERVAL_MAX  60000u

#define QUD_TRACE_TYPE_TICK      UINT32_C(0x1)
#define QUD_TRACE_TYPE_PRESSURE  UINT32_C(0x2)
#define QUD_TRACE_TYPE_CONTROL   UINT32_C(0x4)
#define QUD_TRACE_TYPE_ERROR     UINT32_C(0x8)

struct qud_trace_rec {
    uint64_t ts;
    uint32_t type;
    uint32_t arg;
};
_Static_assert(sizeof(struct qud_trace_rec) == 16u,
               "trace record must stay 16 bytes");

static struct qud_trace_rec qud_trace_ring[QUD_TRACE_RECORDS_MAX];
static uint64_t qud_trace_total;      /* records appended since clear */
static uint32_t qud_trace_head;       /* next write slot (monotonic) */
static uint32_t qud_trace_dropped;    /* records lost to overwrite */
static uint32_t qud_trace_mask;       /* enabled event types */
static uint32_t qud_trace_interval_ms;
static uint64_t qud_trace_last_ts;    /* monotonic guard */
static uint32_t qud_trace_last_pressure;
static uint32_t qud_trace_last_control;
static uint32_t qud_trace_last_error;
static uint64_t qud_trace_next_at;    /* worker-only: next sample deadline */
static uint32_t qud_trace_armed;      /* worker-only: deadline is set */

/* Gate definitions live here (not beside the tentative declarations at the
 * top of the file) so tests/check.py's extracted control-handler window is a
 * complete translation unit. */
static int qud_trace_bootarg_gate = 0;
static int qud_tel_bootarg_gate = 0;

/* Weak exports the storage driver adds for T103 section 4.2.  Declared here,
 * inside the extracted window, so a host build of this file links with them
 * undefined and the verbs report unsupported instead of failing to build. */
extern int native_ufs_tel_open(uint64_t *first_lba, uint64_t *blocks, const char **why)
    __attribute__((weak_import));
extern int native_ufs_tel_read(uint64_t rel, uint32_t blocks, void *dst)
    __attribute__((weak_import));
extern int native_ufs_tel_write(uint64_t rel, uint32_t blocks, const void *src)
    __attribute__((weak_import));
extern int native_ufs_tel_sync(void) __attribute__((weak_import));

static uint32_t
qud_trace_valid(void)
{
    uint64_t total = __atomic_load_n(&qud_trace_total, __ATOMIC_ACQUIRE);
    return total > QUD_TRACE_RECORDS_MAX ? QUD_TRACE_RECORDS_MAX : (uint32_t)total;
}

static uint32_t
qud_trace_oldest_seq(uint32_t valid)
{
    uint64_t total = __atomic_load_n(&qud_trace_total, __ATOMIC_ACQUIRE);
    return (uint32_t)(total - (uint64_t)valid);
}

static void
qud_trace_clear(void)
{
    __atomic_store_n(&qud_trace_total, UINT64_C(0), __ATOMIC_RELAXED);
    __atomic_store_n(&qud_trace_head, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&qud_trace_dropped, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&qud_trace_last_ts, UINT64_C(0), __ATOMIC_RELAXED);
    __atomic_store_n(&qud_trace_last_pressure, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&qud_trace_last_control, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&qud_trace_last_error, 0u, __ATOMIC_RELAXED);
    qud_trace_next_at = 0;
    qud_trace_armed = 0;
}

static void
qud_trace_put(uint64_t ts, uint32_t type, uint32_t arg)
{
    struct qud_trace_rec *slot;
    uint64_t last = __atomic_load_n(&qud_trace_last_ts, __ATOMIC_RELAXED);

    /* CNTVCT_EL0 is monotonic; clamp rather than store a backwards stamp. */
    if (ts < last) ts = last;
    __atomic_store_n(&qud_trace_last_ts, ts, __ATOMIC_RELAXED);
    if (qud_trace_valid() >= QUD_TRACE_RECORDS_MAX)
        (void)__atomic_add_fetch(&qud_trace_dropped, 1u, __ATOMIC_RELAXED);
    slot = &qud_trace_ring[__atomic_load_n(&qud_trace_head, __ATOMIC_RELAXED) &
                           (QUD_TRACE_RECORDS_MAX - 1u)];
    slot->ts = ts;
    __atomic_store_n(&slot->type, type, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->arg, arg, __ATOMIC_RELAXED);
    (void)__atomic_add_fetch(&qud_trace_total, UINT64_C(1), __ATOMIC_RELAXED);
    (void)__atomic_add_fetch(&qud_trace_head, 1u, __ATOMIC_RELEASE);
}

static void
qud_trace_start(uint32_t interval_ms, uint32_t mask)
{
    qud_trace_clear();
    qud_trace_interval_ms = interval_ms;
    qud_trace_mask = mask;
}

/* Called from qcom_usb_debug_poll() with the ARM virtual counter and its
 * frequency.  Both arrive as arguments so this block compiles and can be
 * tested without the counter instruction. */
static void
qud_trace_tick(uint64_t now, uint64_t hz, const struct qcom_usb_debug_status *status)
{
    uint64_t span;
    uint32_t pressure, control, error;

    if (!qud_trace_bootarg_gate || !qud_trace_mask || !hz || !status) return;
    span = hz * (uint64_t)qud_trace_interval_ms / 1000u;
    if (!span) span = hz / 1000u;             /* sub-tick interval */
    if (!qud_trace_armed) {                    /* first tick after TRACEON */
        qud_trace_next_at = now + span;
        qud_trace_armed = 1;
        return;
    }
    if (now < qud_trace_next_at) return;
    /* Behind by more than one interval: skip the missed samples instead of
     * writing a burst of back-dated rows. */
    qud_trace_next_at = now + span;

    if (qud_trace_mask & QUD_TRACE_TYPE_TICK)
        qud_trace_put(now, QUD_TRACE_TYPE_TICK, status->poll_calls);

    pressure = (uint32_t)status->buffer_overflow |
               ((uint32_t)status->rx_overflow << 16);
    if ((qud_trace_mask & QUD_TRACE_TYPE_PRESSURE) &&
        pressure != qud_trace_last_pressure) {
        qud_trace_last_pressure = pressure;
        qud_trace_put(now, QUD_TRACE_TYPE_PRESSURE, pressure);
    }

    control = status->control_rejected;
    if ((qud_trace_mask & QUD_TRACE_TYPE_CONTROL) &&
        control != qud_trace_last_control) {
        qud_trace_last_control = control;
        qud_trace_put(now, QUD_TRACE_TYPE_CONTROL, control);
    }

    error = status->error | ((uint32_t)status->phase << 16);
    if ((qud_trace_mask & QUD_TRACE_TYPE_ERROR) && error != qud_trace_last_error) {
        qud_trace_last_error = error;
        qud_trace_put(now, QUD_TRACE_TYPE_ERROR, error);
    }
}

/* Count whitespace-separated tokens without a cap.  control_args_split stops
 * at its caller's maximum, which would silently accept a third argument; the
 * parsers below need to reject that. */
static unsigned
control_args_count(const uint8_t *text, size_t bytes)
{
    unsigned count = 0;
    size_t i = 0;

    if (!text || !bytes) return 0;
    while (i < bytes) {
        while (i < bytes && text[i] == (uint8_t)' ') i++;
        if (i >= bytes) break;
        count++;
        while (i < bytes && text[i] != (uint8_t)' ') i++;
    }
    return count;
}

/* Decimal argument parser (control_args_hex parses bare digits as hex, which
 * is the wrong default for an interval in milliseconds). */
static int
control_args_decimal(const uint8_t *text, size_t bytes, uint64_t *value)
{
    uint64_t result = 0;
    size_t i;

    if (!value || !bytes || bytes > 19u) return 0;
    for (i = 0; i < bytes; i++) {
        if (text[i] < (uint8_t)'0' || text[i] > (uint8_t)'9') return 0;
        if (result > (UINT64_MAX - 9u) / 10u) return 0;
        result = result * 10u + (uint64_t)(text[i] - (uint8_t)'0');
    }
    *value = result;
    return 1;
}

/* TRACEON <interval-ms> <type-mask>.  Returns 1 when both arguments are in
 * range; the caller turns 0 into ERR args.  Kept out of the switch so the
 * offline self-check drives the real parser. */
static int
qud_trace_parse_start(const uint8_t *args, size_t bytes,
                      uint32_t *interval_ms, uint32_t *mask)
{
    const uint8_t *tok[2]; size_t len[2];
    uint64_t ms = 0, bits = 0;
    unsigned argc;

    if (control_args_count(args, bytes) != 2u) return 0;
    argc = control_args_split(args, bytes, tok, len, 2);
    if (argc != 2) return 0;
    if (!control_args_decimal(tok[0], len[0], &ms) ||
        ms < 1u || ms > QUD_TRACE_INTERVAL_MAX) return 0;
    if (!control_args_hex(tok[1], len[1], &bits) || !bits ||
        (bits & ~QUD_TRACE_TYPES)) return 0;
    if (interval_ms) *interval_ms = (uint32_t)ms;
    if (mask) *mask = (uint32_t)bits;
    return 1;
}

/* TRACEREAD [start] [count]: defaults cover the whole window; the reply is
 * capped at QUD_TRACE_READ_MAX records so one frame stays under 512 bytes. */
static int
qud_trace_parse_read(const uint8_t *args, size_t bytes,
                     uint32_t *start, uint32_t *count)
{
    const uint8_t *tok[2]; size_t len[2];
    uint64_t from = 0, want = QUD_TRACE_READ_MAX;
    unsigned argc;

    if (control_args_count(args, bytes) > 2u) return 0;
    argc = control_args_split(args, bytes, tok, len, 2);
    if (argc > 0 && !control_args_decimal(tok[0], len[0], &from)) return 0;
    if (argc > 1 && !control_args_decimal(tok[1], len[1], &want)) return 0;
    if (!want || want > QUD_TRACE_READ_MAX) return 0;
    if (start) *start = (uint32_t)from;
    if (count) *count = (uint32_t)want;
    return 1;
}

static uint32_t
qud_trace_read(uint32_t start, uint32_t count, struct qud_trace_rec *dst)
{
    uint32_t valid = qud_trace_valid(), i, base;

    if (!dst || start >= valid) return 0;
    if (count > valid - start) count = valid - start;
    base = __atomic_load_n(&qud_trace_head, __ATOMIC_ACQUIRE) - valid;
    for (i = 0; i < count; i++)
        dst[i] = qud_trace_ring[(base + start + i) & (QUD_TRACE_RECORDS_MAX - 1u)];
    return count;
}

/* ---- TELDUMP / TELREAD: NABUTEL1 skeleton (T103 section 4.2) ------------- */

static int
qud_tel_available(void)
{
    return &native_ufs_tel_open != 0 && &native_ufs_tel_read != 0 &&
           &native_ufs_tel_write != 0;
}

static int
qud_tel_parse_dump(const uint8_t *args, size_t bytes,
                   uint64_t *pa, uint32_t *blocks,
                   const uint8_t **tag, size_t *taglen)
{
    const uint8_t *tok[3]; size_t len[3];
    uint64_t addr = 0, count = 0;
    unsigned total, argc;

    total = control_args_count(args, bytes);
    if (total < 2u || total > 3u) return 0;
    argc = control_args_split(args, bytes, tok, len, 3);
    if (argc < 2 ||
        !control_args_hex(tok[0], len[0], &addr) ||
        !control_args_hex(tok[1], len[1], &count) ||
        !count || (count & 0xfffu) || count > (64u << 20)) return 0;
    if (pa) *pa = addr;
    if (blocks) *blocks = (uint32_t)(count / 4096u);
    if (tag) *tag = argc > 2 ? tok[2] : (const uint8_t *)"tel";
    if (taglen) *taglen = argc > 2 ? len[2] : 3u;
    return 1;
}
/* T142-trace: end */

/* ---- T150: DEVTREE walk executed off the console worker ------------------- */

#define QUD_DT_WAIT_MS  500u

static void
qud_dt_thread_main(void *argument, unsigned int wait_result)
{
    struct qud_dt_job *job = (struct qud_dt_job *)argument;
    int ok;

    (void)wait_result;
    IOLog("nabu dt run begin path=%s max=%u\n", job->path,
          (unsigned)job->max_entries);
    ok = native_dt_explore(job->path, job->body, sizeof(job->body), &job->used,
                           &job->why, job->max_entries, &job->truncated);
    __atomic_store_n(&job->state, ok ? 2u : 3u, __ATOMIC_RELEASE);
    IOLog("nabu dt run end path=%s rc=%d\n", job->path, ok);
    thread_terminate_self();
}

/* Builder for a finished dump: identical wire format to the synchronous path
 * so host scripts keep working when the gate is on. */
static size_t
qud_dt_reply_dump(char *reply, size_t capacity,
                  const struct sxnu_control_frame *frame,
                  const char *path, const struct qud_dt_job *job)
{
    struct sxnu_control_writer writer;

    sxnu_control_writer_init(&writer, reply, capacity);
    (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
    (void)sxnu_control_writer_text(&writer, " state=ready path=");
    (void)sxnu_control_writer_text(&writer, path);
    (void)sxnu_control_writer_text(&writer, " bytes=");
    (void)sxnu_control_writer_decimal(&writer, job->used);
    (void)sxnu_control_writer_text(&writer, " trunc=");
    (void)sxnu_control_writer_decimal(&writer, (uint64_t)job->truncated);
    (void)sxnu_control_writer_char(&writer, '\n');
    (void)sxnu_control_writer_put(&writer, job->body, job->used);
    return sxnu_control_reply_finish(&writer);
}

/* Path equality without libc: the module links no strcmp. */
static int
qud_dt_path_same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static size_t
qud_dt_thread_dispatch(char *reply, size_t capacity,
                       const struct sxnu_control_frame *frame,
                       const char *path, uint64_t max_entries)
{
    struct qud_dt_job *job = &qud_dt_job;
    struct sxnu_control_writer writer;
    uint32_t state;
    size_t n = 0;

    state = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE);
    if (state == 1u)
        return control_reply_reboot(reply, capacity, frame, "ERR", "busy");

    /* Cache hit: the same path and cap returns the finished walk as-is. */
    if (state == 2u && job->max_entries == (uint32_t)max_entries &&
        qud_dt_path_same(job->path, path))
        return qud_dt_reply_dump(reply, capacity, frame, path, job);

    /* Fresh request: bounded path copy (the caller validated the length). */
    __builtin_memset(job, 0, sizeof(*job));
    while (path[n] && n + 1u < sizeof(job->path)) {
        job->path[n] = path[n];
        n++;
    }
    job->path[n] = 0;
    job->max_entries = (uint32_t)max_entries;
    job->submit_tick = counter();
    __atomic_store_n(&job->state, 1u, __ATOMIC_RELEASE);
    {
        void *thread = 0;
        if (kernel_thread_start(qud_dt_thread_main, job, &thread) != 0) {
            __atomic_store_n(&job->state, 0u, __ATOMIC_RELEASE);
            return control_reply_reboot(reply, capacity, frame, "ERR", "nothread");
        }
        thread_deallocate(thread);
    }

    /* Bounded wait with interrupts on. A healthy walk finishes in
     * milliseconds; a wedged one is reported instead of stalling the
     * console. The helper thread keeps running either way. */
    {
        uint64_t hz = native.status.counter_frequency;
        uint64_t deadline = job->submit_tick +
                            (hz ? hz * QUD_DT_WAIT_MS / 1000u : 1000000u);
        while (__atomic_load_n(&job->state, __ATOMIC_ACQUIRE) == 1u &&
               counter() < deadline)
            __asm__ volatile("yield" ::: "memory");
    }

    state = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE);
    if (state == 1u)
        return control_reply_reboot(reply, capacity, frame, "ERR", "slow");
    if (state == 3u) {
        sxnu_control_writer_init(&writer, reply, capacity);
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "ERR");
        (void)sxnu_control_writer_text(&writer, " state=");
        (void)sxnu_control_writer_text(&writer, job->why ? job->why : "failed");
        (void)sxnu_control_writer_text(&writer, " path=");
        (void)sxnu_control_writer_text(&writer, path);
        return sxnu_control_reply_finish(&writer);
    }
    return qud_dt_reply_dump(reply, capacity, frame, path, job);
}

static void control_handle_frame(const struct sxnu_control_frame *frame,
                                 const struct qcom_usb_debug_status *status,
                                 uint32_t generation, unsigned int pipe)
{
    enum sxnu_control_verb accepted = sxnu_control_accept(&usb_control_context.state, frame);
    char reply[SXNU_CONTROL_REPLY_BYTES];
    size_t reply_bytes = 0;
    int queued = 0;
    int reboot_accepted = 0;
    size_t shot_bytes = 0;

    if (accepted == SXNU_CONTROL_VERB_INVALID) {
        (void)__atomic_add_fetch(&control_rejected, 1u, __ATOMIC_RELAXED);
        return;
    }
    /* T140 gate: without nabu-verbs-forensics=1 the three forensics verbs take
     * the identical path an unknown verb takes, so a host cannot tell the
     * difference on the wire. */
    if (!usb_forensics_verbs &&
        (accepted == SXNU_CONTROL_VERB_DEVTREE ||
         accepted == SXNU_CONTROL_VERB_FBRAW ||
         accepted == SXNU_CONTROL_VERB_NABULOGRING)) {
        (void)__atomic_add_fetch(&control_rejected, 1u, __ATOMIC_RELAXED);
        return;
    }
    /* T151 PIPE1STAT: diagnostics only exist when the optional second
     * pipe is enabled.  With it off the verb takes the unknown-verb path,
     * so single-port wire behaviour is unchanged. */
    if (accepted == SXNU_CONTROL_VERB_PIPE1STAT && !cdc_second_port) {
        (void)__atomic_add_fetch(&control_rejected, 1u, __ATOMIC_RELAXED);
        return;
    }
    switch (accepted) {
    case SXNU_CONTROL_VERB_HELLO:
        reply_bytes = control_reply_hello(reply, sizeof(reply), frame, pipe);
        break;
    case SXNU_CONTROL_VERB_PING:
        reply_bytes = control_reply_ping(reply, sizeof(reply), frame, status);
        break;
    case SXNU_CONTROL_VERB_STATUS:
        reply_bytes = control_reply_status(reply, sizeof(reply), frame, status);
        break;
    case SXNU_CONTROL_VERB_SCREENSHOT: {
        struct qcom_usb_screenshot_info info;
        struct sxnu_control_writer writer;
        int result = __atomic_load_n(&control_reboot_requested, __ATOMIC_ACQUIRE) ?
                     -3 : qcom_usb_shot_capture(frame->seq);
        if (result != 1 || !qcom_usb_shot_info(&info)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR",
                result == -1 ? "unavailable" : result == -3 ? "busy" : "capture-failed");
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " state=ready width=");
        (void)sxnu_control_writer_decimal(&writer, info.width);
        (void)sxnu_control_writer_text(&writer, " height=");
        (void)sxnu_control_writer_decimal(&writer, info.height);
        (void)sxnu_control_writer_text(&writer, " bytes=");
        (void)sxnu_control_writer_decimal(&writer, info.bytes);
        (void)sxnu_control_writer_text(&writer, " encoded=");
        (void)sxnu_control_writer_decimal(&writer, info.encoded_bytes);
        (void)sxnu_control_writer_text(&writer, " crc32=");
        (void)sxnu_control_writer_hex(&writer, info.crc32, 8);
        (void)sxnu_control_writer_text(&writer, " frame=");
        (void)sxnu_control_writer_decimal(&writer, info.frame);
        (void)sxnu_control_writer_text(&writer, info.rle ? " encoding=rle" : " encoding=rgb");
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_SHOTNEXT: {
        struct qcom_usb_screenshot_info info;
        struct sxnu_control_writer writer;
        uint8_t chunk[QUD_SCREENSHOT_CHUNK];
        uint32_t offset;
        if (!qcom_usb_shot_info(&info)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "noimage");
            break;
        }
        shot_bytes = qcom_usb_shot_peek(chunk, sizeof(chunk), &offset);
        if (!shot_bytes) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "done");
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " state=data frame=");
        (void)sxnu_control_writer_decimal(&writer, info.frame);
        (void)sxnu_control_writer_text(&writer, " offset=");
        (void)sxnu_control_writer_decimal(&writer, offset);
        (void)sxnu_control_writer_text(&writer, " data=");
        for (size_t i = 0; i < shot_bytes; ++i)
            (void)sxnu_control_writer_hex(&writer, chunk[i], 2);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_READ: {
        const uint8_t *tok[4];
        size_t len[4];
        uint64_t va = 0, count = 0;
        struct sxnu_control_writer writer;
        uint8_t chunk[QUD_DIAG_CHUNK];
        uint32_t offset = 0, frame_no = 0;
        size_t got;
        int opened;
        if (control_args_split(frame->args, frame->arg_bytes, tok, len, 4) != 2 ||
            !control_args_hex(tok[0], len[0], &va) ||
            !control_args_hex(tok[1], len[1], &count) ||
            !count || count > QUD_DIAG_MAX_LEN) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        opened = qud_diag_read_begin(va, (uint32_t)count, frame->seq, 0);
        if (opened != 1) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR",
                opened == -2 ? "unmapped" : "args");
            break;
        }
        got = qud_diag_read_next(chunk, sizeof(chunk), &offset, &frame_no);
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " state=ready len=");
        (void)sxnu_control_writer_decimal(&writer, qud_diag_cursor_length());
        (void)sxnu_control_writer_text(&writer, " crc32=");
        (void)sxnu_control_writer_hex(&writer, qud_diag_cursor_crc(), 8);
        (void)sxnu_control_writer_text(&writer, " offset=");
        (void)sxnu_control_writer_decimal(&writer, offset);
        (void)sxnu_control_writer_text(&writer, " data=");
        for (size_t i = 0; i < got; ++i)
            (void)sxnu_control_writer_hex(&writer, chunk[i], 2);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_READPA: {
        const uint8_t *tok[4];
        size_t len[4];
        uint64_t pa = 0, count = 0;
        struct sxnu_control_writer writer;
        uint8_t chunk[QUD_DIAG_CHUNK];
        uint32_t offset = 0, frame_no = 0;
        size_t got;
        int opened;
        if (control_args_split(frame->args, frame->arg_bytes, tok, len, 4) != 2 ||
            !control_args_hex(tok[0], len[0], &pa) ||
            !control_args_hex(tok[1], len[1], &count) ||
            !count || count > QUD_DIAG_MAX_PHYS) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        /* Refuse a PA that is neither RAM nor a known MMIO block before any
         * mapping happens. Measured on kb-46: reading a nonexistent PA wedged
         * the USB worker for good (ml_io_map's KMA_NOFAIL allocation returns a
         * valid VA for a PA that maps to nothing, and the memcpy never
         * returns). One mistyped address should not cost a power cycle. */
        {
            const char *allow_why = NULL;
            if (!qud_read_pa_allowed(pa, (size_t)count, &allow_why)) {
                /* state=denied cost a debugging round on kb-47 because it did
                 * not say WHICH check failed. name= names the block that would
                 * have allowed it, or "outside" when nothing does. */
                struct sxnu_control_writer w2;
                sxnu_control_writer_init(&w2, reply, sizeof(reply));
                (void)sxnu_control_reply_start(&w2, frame->session, frame->seq, frame->verb, "ERR");
                (void)sxnu_control_writer_text(&w2, " state=denied name=");
                (void)sxnu_control_writer_text(&w2, allow_why);
                (void)sxnu_control_writer_text(&w2, " pa=0x");
                (void)sxnu_control_writer_hex(&w2, pa, 16);
                (void)sxnu_control_writer_text(&w2, " len=");
                (void)sxnu_control_writer_decimal(&w2, count);
                reply_bytes = sxnu_control_reply_finish(&w2);
                break;
            }
        }
        opened = qud_diag_read_begin(pa, (uint32_t)count, frame->seq, 1);
        if (opened != 1) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR",
                opened == -2 ? "unmapped" : "args");
            break;
        }
        got = qud_diag_read_next(chunk, sizeof(chunk), &offset, &frame_no);
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " state=ready len=");
        (void)sxnu_control_writer_decimal(&writer, qud_diag_cursor_length());
        (void)sxnu_control_writer_text(&writer, " crc32=");
        (void)sxnu_control_writer_hex(&writer, qud_diag_cursor_crc(), 8);
        (void)sxnu_control_writer_text(&writer, " offset=");
        (void)sxnu_control_writer_decimal(&writer, offset);
        (void)sxnu_control_writer_text(&writer, " data=");
        for (size_t i = 0; i < got; ++i)
            (void)sxnu_control_writer_hex(&writer, chunk[i], 2);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_PANICTRIG: {
        /* PANICTRIG <key>: panic on purpose, to test that the landing path's
         * is_panic=1 tail survives a reboot. This is the only verb that is
         * allowed to take the kernel down, so it is behind a boot-arg gate AND
         * requires a literal key -- two independent ways to not fire by accident.
         *
         * the panic is noreturn, so the reply is written to the wire before
         * the call. Everything the host needs to correlate is in that reply.
         *
         * osfmk/kern/debug.h is kernel-private and its panic() is a macro, so the
         * real symbol is declared locally and the import is
         * allow-listed as _panic. */
        static const char want[] = "PANIC-NOW";
        unsigned gated = 0;
        int key_ok = 0;
        (void)PE_parse_boot_argn("nabu-panic-trig", &gated, sizeof(gated));
        if (frame->arg_bytes == sizeof(want) - 1u) {
            const uint8_t *tok[1]; size_t len[1];
            if (control_args_split(frame->args, frame->arg_bytes, tok, len, 1) == 1 &&
                len[0] == sizeof(want) - 1u) {
                unsigned i;
                key_ok = 1;
                for (i = 0; i + 1u < sizeof(want); i++) {
                    if (tok[0][i] != (uint8_t)want[i]) { key_ok = 0; break; }
                }
            }
        }
        if (!gated || !key_ok) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR",
                !gated ? "gated" : "args");
            break;
        }
        {
            struct sxnu_control_writer w2;
            sxnu_control_writer_init(&w2, reply, sizeof(reply));
            (void)sxnu_control_reply_start(&w2, frame->session, frame->seq,
                                           frame->verb, "OK");
            (void)sxnu_control_writer_text(&w2, " state=panicking");
            reply_bytes = sxnu_control_reply_finish(&w2);
        }
        /* Send the reply first: panic does not return, so anything queued
         * afterwards never leaves. qcom_usb_debug_write drains synchronously. */
        (void)qcom_usb_debug_write(reply, reply_bytes);
        qcom_usb_debug_kdp_tx_mode(1);
        (panic)("nabu: PANICTRIG requested by host");
        break;   /* not reached */
    }
    case SXNU_CONTROL_VERB_KDBG: {
        /* KDBG: kdebug control state, read from the exported kd_control_trace
         * instead of through sysctl.
         *
         * T50: kern.kdbg.* carries CTLFLAG_LOCKED and sysctlbyname cannot take that
         * lock from this context -- kb-68 measured state=notfound for both
         * kern.kdbg.oldest_time and kern.kdbg.debug while hw.ncpu worked. The
         * structure itself is exported as D _kd_control_trace, so reading it
         * directly bypasses the sysctl path entirely.
         *
         * Safe from here: the only lock involved is kdc_storage_lock, a lck_spin
         * taken with interrupts off (kdebug_common.c:31), so it cannot sleep. We do
         * not even take it -- kdc_oldest_time is a naturally aligned uint64_t, so
         * the read is one instruction. A torn value is impossible on arm64 and a
         * slightly stale timestamp is fine for this purpose. */
        struct sxnu_control_writer writer;
        uint64_t oldest = 0;
        uint32_t enabled = 0, mode = 0, live_flags = 0, cpus = 0, flags = 0;

        if (&kd_control_trace == 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
            break;
        }
        if (!qud_kdebug_state(&oldest, &enabled, &mode, &live_flags, &cpus, &flags)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unreadable");
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " enabled=");
        (void)sxnu_control_writer_decimal(&writer, enabled);
        (void)sxnu_control_writer_text(&writer, " mode=");
        (void)sxnu_control_writer_decimal(&writer, mode);
        (void)sxnu_control_writer_text(&writer, " live_flags=0x");
        (void)sxnu_control_writer_hex(&writer, live_flags, 8);
        (void)sxnu_control_writer_text(&writer, " kdc_flags=0x");
        (void)sxnu_control_writer_hex(&writer, flags, 8);
        (void)sxnu_control_writer_text(&writer, " cpus=");
        (void)sxnu_control_writer_decimal(&writer, cpus);
        (void)sxnu_control_writer_text(&writer, " oldest_time=0x");
        (void)sxnu_control_writer_hex(&writer, oldest, 16);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_STACKSHOT: {
        /* STACKSHOT: capture every thread's stack into the kernel buffer.
         *
         * T48: the native panic-time stackshot is dead on arm64
         * (panic_stackshot_to_disk_enabled() returns FALSE unconditionally and has
         * no caller), but the *online* capture path is fully exported and used by
         * bsd/kern/stackshot.c:613 -- kern_stack_snapshot_with_reason() fills
         * kernel_stackshot_buf with STACKSHOT_SAVE_IN_KERNEL_BUFFER.
         *
         * That call refuses to overwrite an existing snapshot (KERN_MEMORY_PRESENT),
         * so this verb reports state=present instead of clobbering. Retrieval is a
         * separate step: kernel_stackshot_buf + kernel_stackshot_buf_size are
         * exported, so the host can read them with READ/READPA, or a later verb can
         * stage them into the UFS export region.
         *
         * NOT gated: this only reads kernel state and allocates one buffer; it does
         * not write memory the kernel owns or touch hardware. */
        struct sxnu_control_writer writer;
        extern int kern_stack_snapshot_with_reason(char *reason);
        extern void *kernel_stackshot_buf;
        extern int kernel_stackshot_buf_size;
        int rc;

        if (&kern_stack_snapshot_with_reason == 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
            break;
        }
        rc = kern_stack_snapshot_with_reason("nabu-console");
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb,
            rc == 0 ? "OK" : "ERR");
        (void)sxnu_control_writer_text(&writer, rc == 0 ? " state=captured rc=0" : " state=failed rc=");
        if (rc != 0) {
            (void)sxnu_control_writer_decimal(&writer, (uint32_t)rc);
        }
        /* Report where the snapshot landed either way -- on the "already present"
         * error the old snapshot is still there and still worth reading. */
        {
            uint64_t buf = (uint64_t)(uintptr_t)kernel_stackshot_buf;
            (void)sxnu_control_writer_text(&writer, " buf=0x");
            (void)sxnu_control_writer_hex(&writer, buf, 16);
            (void)sxnu_control_writer_text(&writer, " size=");
            (void)sxnu_control_writer_decimal(&writer, (uint32_t)kernel_stackshot_buf_size);
        }
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_READUFS: {
        /* READUFS <hexrel> [<hexoffset>]: read the diagnostic log window back off
         * the medium. This is the read side of the landing path -- without it the
         * best we can prove is "the payload handed to the writer was correct",
         * not "the bytes on the UFS are correct".
         *
         * One block per reply, so the reply stays under the 512-byte cap: 64 bytes
         * of hex plus the CRC of the whole block. <offset> picks where inside the
         * block those 64 bytes start -- needed because a 4 KiB block usually holds
         * several lines and the interesting one (a NABULOG1 header, or a PANICSTR
         * marker near the end of the newest block) is not always at the front.
         * rel is a block offset from gLogRegionFirst, not an absolute LBA. */
        const uint8_t *tok[3];
        size_t len[3];
        uint64_t rel = 0, offset = 0;
        struct sxnu_control_writer writer;
        int argc = control_args_split(frame->args, frame->arg_bytes, tok, len, 3);
        if (argc < 1 || argc > 2 ||
            !control_args_hex(tok[0], len[0], &rel) ||
            (argc == 2 && !control_args_hex(tok[1], len[1], &offset)) ||
            (offset & 0x3fu) || offset > 4096u - 64u) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        {
            static uint8_t qud_logufs_block[4096];
            int why = 0;
            if (!qud_log_ufs_read_block(rel, qud_logufs_block, &why)) {
                sxnu_control_writer_init(&writer, reply, sizeof(reply));
                (void)sxnu_control_reply_start(&writer, frame->session, frame->seq,
                                               frame->verb, "ERR");
                (void)sxnu_control_writer_text(&writer, " state=");
                (void)sxnu_control_writer_text(&writer,
                    why == 1 ? "unsupported" : (why == 2 ? "nowindow" : "read"));
                (void)sxnu_control_writer_text(&writer, " rel=");
                (void)sxnu_control_writer_decimal(&writer, (uint32_t)rel);
                reply_bytes = sxnu_control_reply_finish(&writer);
                break;
            }
            sxnu_control_writer_init(&writer, reply, sizeof(reply));
            (void)sxnu_control_reply_start(&writer, frame->session, frame->seq,
                                           frame->verb, "OK");
            (void)sxnu_control_writer_text(&writer, " state=ready rel=");
            (void)sxnu_control_writer_decimal(&writer, (uint32_t)rel);
            (void)sxnu_control_writer_text(&writer, " crc32=");
            {
                uint32_t crc = 0xffffffffu;
                for (uint32_t i = 0; i < 4096u; i++) {
                    crc ^= qud_logufs_block[i];
                    for (unsigned bit = 0; bit < 8; bit++) {
                        crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
                    }
                }
                (void)sxnu_control_writer_hex(&writer, (uint64_t)(crc ^ 0xffffffffu), 8);
            }
            (void)sxnu_control_writer_text(&writer, " off=");
            (void)sxnu_control_writer_decimal(&writer, (uint32_t)offset);
            (void)sxnu_control_writer_text(&writer, " data=");
            for (uint32_t i = 0; i < 64u; i++) {
                (void)sxnu_control_writer_hex(&writer, qud_logufs_block[offset + i], 2);
            }
            reply_bytes = sxnu_control_reply_finish(&writer);
        }
        break;
    }
    case SXNU_CONTROL_VERB_READPANEXT: {
        const uint8_t *tok[4];
        size_t len[4];
        uint64_t want = QUD_DIAG_CHUNK;
        struct sxnu_control_writer writer;
        uint8_t chunk[QUD_DIAG_CHUNK];
        uint32_t offset = 0, frame_no = 0;
        size_t got;
        if (!qud_diag_cursor_active() || !qud_diag_cursor_is_phys()) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "noimage");
            break;
        }
        if (control_args_split(frame->args, frame->arg_bytes, tok, len, 4) == 1) {
            if (!control_args_hex(tok[0], len[0], &want) ||
                !want || want > QUD_DIAG_CHUNK) {
                reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
                break;
            }
        }
        got = qud_diag_read_next(chunk, (size_t)want, &offset, &frame_no);
        if (!got) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "done");
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " state=data frame=");
        (void)sxnu_control_writer_decimal(&writer, frame_no);
        (void)sxnu_control_writer_text(&writer, " offset=");
        (void)sxnu_control_writer_decimal(&writer, offset);
        (void)sxnu_control_writer_text(&writer, " data=");
        for (size_t i = 0; i < got; ++i)
            (void)sxnu_control_writer_hex(&writer, chunk[i], 2);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_READNEXT: {
        const uint8_t *tok[4];
        size_t len[4];
        uint64_t want = QUD_DIAG_CHUNK;
        struct sxnu_control_writer writer;
        uint8_t chunk[QUD_DIAG_CHUNK];
        uint32_t offset = 0, frame_no = 0;
        size_t got;
        if (!qud_diag_cursor_active()) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "noimage");
            break;
        }
        if (control_args_split(frame->args, frame->arg_bytes, tok, len, 4) == 1) {
            if (!control_args_hex(tok[0], len[0], &want) ||
                !want || want > QUD_DIAG_CHUNK) {
                reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
                break;
            }
        }
        got = qud_diag_read_next(chunk, (size_t)want, &offset, &frame_no);
        if (!got) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "done");
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " state=data frame=");
        (void)sxnu_control_writer_decimal(&writer, frame_no);
        (void)sxnu_control_writer_text(&writer, " offset=");
        (void)sxnu_control_writer_decimal(&writer, offset);
        (void)sxnu_control_writer_text(&writer, " data=");
        for (size_t i = 0; i < got; ++i)
            (void)sxnu_control_writer_hex(&writer, chunk[i], 2);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_WRITE: {
        /* WRITE <hexva> <hexbytes>. Refused unless qcom-usb-write=1. */
        const uint8_t *tok[4];
        size_t len[4];
        uint64_t va = 0;
        uint8_t staged[QUD_DIAG_CHUNK];
        size_t staged_bytes = 0;
        struct sxnu_control_writer writer;
        int result;
        int bad = 0;
        if (control_args_split(frame->args, frame->arg_bytes, tok, len, 4) != 2 ||
            !control_args_hex(tok[0], len[0], &va) ||
            (len[1] & 1u) || len[1] / 2u > sizeof(staged)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        for (size_t i = 0; i < len[1] / 2u; i++) {
            uint64_t byte = 0;
            if (!sxnu_control_parse_hex(tok[1] + i * 2u, 2u, &byte)) {
                bad = 1;
                break;
            }
            staged[staged_bytes++] = (uint8_t)byte;
        }
        if (bad || !staged_bytes) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        result = qud_diag_write(va, staged, staged_bytes);
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb,
            result == 1 ? "OK" : "ERR");
        (void)sxnu_control_writer_text(&writer, result == 1 ? " state=written bytes=" :
            result < 0 ? " state=disabled bytes=" : " state=unmapped bytes=");
        (void)sxnu_control_writer_decimal(&writer, staged_bytes);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_WRITEPA: {
        /* WRITEPA <hexpa> <hexbytes>. MMIO-whitelisted only; boot-arg gated. */
        const uint8_t *tok[4];
        size_t len[4];
        uint64_t pa = 0;
        uint8_t staged[QUD_DIAG_CHUNK];
        size_t staged_bytes = 0;
        struct sxnu_control_writer writer;
        uint32_t readback = 0;
        int result;
        int bad = 0;
        if (control_args_split(frame->args, frame->arg_bytes, tok, len, 4) != 2 ||
            !control_args_hex(tok[0], len[0], &pa) ||
            (len[1] & 1u) || len[1] / 2u > sizeof(staged)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        for (size_t i = 0; i < len[1] / 2u; i++) {
            uint64_t byte = 0;
            if (!sxnu_control_parse_hex(tok[1] + i * 2u, 2u, &byte)) { bad = 1; break; }
            staged[staged_bytes++] = (uint8_t)byte;
        }
        if (bad || !staged_bytes) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        result = qud_diag_write_phys(pa, staged, staged_bytes, &readback);
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb,
            result == 1 ? "OK" : "ERR");
        if (result == 1) {
            (void)sxnu_control_writer_text(&writer, " pa=0x");
            (void)sxnu_control_writer_hex(&writer, pa, 8);
            (void)sxnu_control_writer_text(&writer, " bytes=");
            (void)sxnu_control_writer_decimal(&writer, staged_bytes);
            (void)sxnu_control_writer_text(&writer, " readback=");
            (void)sxnu_control_writer_hex(&writer, readback, 8);
        } else {
            (void)sxnu_control_writer_text(&writer, " ");
            (void)sxnu_control_writer_text(&writer,
                result == -1 ? "state=disabled" :
                result == -2 ? "state=denied"  :
                result == -3 ? "state=mapfail" : "state=args");
        }
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_BOOTARGS: {
        /* Return the kernel command line from PE_boot_args() (the exact string
         * PE_parse_boot_argn parses). Bounded to the reply buffer; the host can
         * issue BOOTARGS all for the full line via repeated reads if needed, but
         * the boot line is normally < 1 KiB and the useful subset fits here. */
        struct sxnu_control_writer writer;
        const char *cmdline = PE_boot_args();
        size_t n = 0;
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " hex=");
        if (!cmdline) {
            (void)sxnu_control_writer_text(&writer, "(null)");
        } else {
            /* Hex-encode the command line: no spaces, no parse ambiguity with
             * the space-delimited SXNU1R reply. Two hex chars per byte. */
            static const char hex[] = "0123456789abcdef";
            /* Reply cap is 512B (SXNU_CONTROL_REPLY_BYTES); header ~70B +
             * 2 hex chars/byte + len= field. 200 bytes -> 400 hex chars
             * keeps the whole frame under the cap.  Truncation is fine:
             * the leading boot-args (debug=, qcom-usb-*) are what matter. */
            while (cmdline[n] && n < 200u && !writer.failed) {
                const unsigned char c = (unsigned char)cmdline[n];
                (void)sxnu_control_writer_char(&writer, hex[c >> 4]);
                (void)sxnu_control_writer_char(&writer, hex[c & 0xf]);
                n++;
            }
        }
        (void)sxnu_control_writer_text(&writer, " len=");
        (void)sxnu_control_writer_decimal(&writer, n);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_VERSION: {
        struct sxnu_control_writer writer;
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " build=qcom-xnu-usb-console xnu=17.7.11-21H461 soc=sm8150");
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_DUMPUFS: {
        /* DUMPUFS <hexpa> <hexlen> <tag>: enqueue async UFS dump. */
        const uint8_t *tok[4];
        size_t len[4];
        uint64_t pa = 0, n = 0;
        struct sxnu_control_writer writer;
        int slot;
        if (control_args_split(frame->args, frame->arg_bytes, tok, len, 4) < 2 ||
            !control_args_hex(tok[0], len[0], &pa) ||
            !control_args_hex(tok[1], len[1], &n) || (n & 0xfffu) ||
            n > (64u << 20)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        if (!qud_dump_ufs_available()) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
            break;
        }
        slot = qud_dump_ufs_enqueue(pa, (uint32_t)n,
                                    len[2] ? tok[2] : (const uint8_t *)"dump",
                                    len[2] ? len[2] : 4);
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb,
            slot >= 0 ? "OK" : "ERR");
        if (slot >= 0) {
            /* T35-s3 Q3: write the first block synchronously so this reply is
             * itself the verdict -- blocks_written=1 proves the read path and
             * export_write both work; write_err names the failing step.
             * The slot also carries its state here, so DUMLIST (which loses
             * replies when the TX ring is busy) is no longer needed. */
            uint32_t bw = 0; int we = 0, sy = 0;
            (void)qud_dump_ufs_write_first((uint32_t)slot, &bw, &we, &sy);
            (void)sxnu_control_writer_text(&writer, " state=queued slot=");
            (void)sxnu_control_writer_decimal(&writer, (uint32_t)slot);
            (void)sxnu_control_writer_text(&writer, " est=");
            (void)sxnu_control_writer_decimal(&writer, (uint32_t)n);
            (void)sxnu_control_writer_text(&writer, " blocks_written=");
            (void)sxnu_control_writer_decimal(&writer, bw);
            (void)sxnu_control_writer_text(&writer, " write_err=");
            (void)sxnu_control_writer_decimal(&writer, (uint32_t)we);
            (void)sxnu_control_writer_text(&writer, " synced=");
            (void)sxnu_control_writer_decimal(&writer, (uint32_t)sy);
            (void)sxnu_control_writer_text(&writer, " slot_state=");
            {
                uint64_t spa = 0; uint32_t slen = 0, sdone = 0; const char *stag = "";
                uint32_t sst = qud_dump_ufs_slot((uint32_t)slot, &spa, &slen, &sdone, &stag);
                (void)sxnu_control_writer_decimal(&writer, sst);
                (void)sxnu_control_writer_text(&writer, " done=");
                (void)sxnu_control_writer_decimal(&writer, sdone);
            }
        } else {
            (void)sxnu_control_writer_text(&writer, " ");
            (void)sxnu_control_writer_text(&writer,
                slot == -1 ? "state=busy" : slot == -2 ? "state=unsupported" :
                slot == -4 ? "state=denied" : "state=args");
        }
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_DUMLIST: {
        struct sxnu_control_writer writer;
        uint32_t i;
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        for (i = 0; i < 4u && !writer.failed; i++) {
            uint64_t pa = 0; uint32_t len = 0, done = 0; const char *tag = "";
            /* kb-39: never hand slot->tag (a uint8_t[16] that a full tag leaves
             * unterminated) to a %s-style writer. Copy it into a local buffer
             * with a guaranteed NUL. Measured on kb-38: DUMPLIST stopped
             * answering and every later verb timed out, while the kernel kept
             * logging -- the writer was walking memory with the USB lock held. */
            char tagbuf[17];
            uint32_t st = qud_dump_ufs_slot(i, &pa, &len, &done, &tag);
            if (st == 0) continue;
            {
                size_t n = 0;
                while (n < 16u && tag && tag[n]) { tagbuf[n] = tag[n]; n++; }
                tagbuf[n] = 0;
            }
            (void)sxnu_control_writer_text(&writer, " slot=");
            (void)sxnu_control_writer_decimal(&writer, i);
            (void)sxnu_control_writer_text(&writer, " state=");
            (void)sxnu_control_writer_decimal(&writer, st);
            (void)sxnu_control_writer_text(&writer, " done=");
            (void)sxnu_control_writer_decimal(&writer, done);
            (void)sxnu_control_writer_text(&writer, "/");
            (void)sxnu_control_writer_decimal(&writer, len);
            (void)sxnu_control_writer_text(&writer, " tag=");
            (void)sxnu_control_writer_text(&writer, tagbuf);
        }
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_UPTIME: {
        /* UPTIME -> the ARM virtual counter, so the host has one unambiguous
         * timebase instead of the three t= clocks in the log. */
        struct sxnu_control_writer writer;
        uint64_t cnt = 0, frq = 0;
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cnt));
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " ticks=");
        (void)sxnu_control_writer_decimal(&writer, cnt);
        (void)sxnu_control_writer_text(&writer, " hz=");
        (void)sxnu_control_writer_decimal(&writer, frq);
        (void)sxnu_control_writer_text(&writer, " us=");
        (void)sxnu_control_writer_decimal(&writer, frq ? (cnt * 1000000ull / frq) : 0);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_RDBLK: {
        /* RDBLK <name> <off> <len>: read a named MMIO block so the host never
         * has to remember absolute addresses. Names are matched here. */
        static const struct { const char *name; uint64_t base; uint32_t size; } blocks[] = {
            { "mdss",  UINT64_C(0x0ae00000), 0x100000 },
            { "mdp",   UINT64_C(0x0ae01000), 0x1000   },
            { "intf1", UINT64_C(0x0ae6b000), 0x1000   },
            { "intf2", UINT64_C(0x0ae6c000), 0x1000   },
            { "gpucc", UINT64_C(0x02c90000), 0x9000   },
            { "gcc",   UINT64_C(0x01050000), 0x4000   },
            /* "smmu" deliberately absent: RDBLK goes straight to
             * qud_diag_read_phys and would bypass the READPA whitelist. Reading
             * SMMU registers crashed the kernel on kb-47 -- its clock domain is
             * not guaranteed up, and that is not knowable from here. Restore
             * only with a board round proving the domain is up. */
            { "discc", UINT64_C(0x0af00000), 0x1000   },
            { "dwc3",  UINT64_C(0x0a600000), 0x10000  },
        };
        const uint8_t *tok[4]; size_t len[4];
        uint64_t off = 0, n = 0;
        unsigned guard = 0;
        if (control_args_split(frame->args, frame->arg_bytes, tok, len, 4) != 3 ||
            !control_args_hex(tok[1], len[1], &off) ||
            !control_args_hex(tok[2], len[2], &n) || !n || n > QUD_DIAG_MAX_PHYS) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        {
            uint64_t base = 0; uint32_t size = 0;
            for (unsigned i = 0; i < sizeof(blocks)/sizeof(blocks[0]); i++) {
                size_t k = 0;
                while (k < len[0] && blocks[i].name[k] && blocks[i].name[k] == (char)tok[0][k]) k++;
                if (k == len[0] && blocks[i].name[k] == 0) {
                    base = blocks[i].base; size = blocks[i].size; break;
                }
            }
            if (!base || off >= size || n > size - off) {
                reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "range");
                break;
            }
            int opened = qud_diag_read_begin(base + off, (uint32_t)n, frame->seq, 1);
            if (opened != 1) {
                reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR",
                    opened == -2 ? "unmapped" : "args");
                break;
            }
            {
                struct sxnu_control_writer writer;
                uint8_t chunk[QUD_DIAG_CHUNK];
                uint32_t offset = 0, fno = 0;
                size_t got = qud_diag_read_next(chunk, sizeof(chunk), &offset, &fno);
                sxnu_control_writer_init(&writer, reply, sizeof(reply));
                (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
                (void)sxnu_control_writer_text(&writer, " state=ready len=");
                (void)sxnu_control_writer_decimal(&writer, qud_diag_cursor_length());
                (void)sxnu_control_writer_text(&writer, " crc32=");
                (void)sxnu_control_writer_hex(&writer, qud_diag_cursor_crc(), 8);
                (void)sxnu_control_writer_text(&writer, " offset=");
                (void)sxnu_control_writer_decimal(&writer, offset);
                (void)sxnu_control_writer_text(&writer, " data=");
                for (size_t q = 0; q < got; q++)
                    (void)sxnu_control_writer_hex(&writer, chunk[q], 2);
                reply_bytes = sxnu_control_reply_finish(&writer);
            }
        }
        (void)guard;
        break;
    }
    case SXNU_CONTROL_VERB_PERFSTART: {
        /* PERFSTART <hexms>: open a GPU profile window (100..60000 ms). */
        const uint8_t *tok[2]; size_t len[2];
        uint64_t ms = 3000;
        struct sxnu_control_writer writer;
        uint32_t rc;
        if (frame->arg_bytes && (control_args_split(frame->args, frame->arg_bytes, tok, len, 2) != 1 ||
                                 !control_args_hex(tok[0], len[0], &ms))) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        if (&native_adreno_gpu_profile_start == 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
            break;
        }
        rc = native_adreno_gpu_profile_start((uint32_t)ms);
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb,
            rc == 0 ? "OK" : "ERR");
        (void)sxnu_control_writer_text(&writer, rc == 0 ? " state=armed ms=" : " state=busy rc=");
        (void)sxnu_control_writer_decimal(&writer, rc == 0 ? (uint32_t)ms : rc);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_PERFSTOP: {
        if (&native_adreno_gpu_profile_stop == 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
            break;
        }
        native_adreno_gpu_profile_stop();
        reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "OK", "state=stopped");
        break;
    }
    case SXNU_CONTROL_VERB_PERFREAD: {
        /* PERFREAD [uche|sp|ccu|all]: snapshot of the last completed window.
         * Reply budget is 512 B, so this returns the header plus the first few
         * counters; the full 29-counter table goes to the kernel log. */
        uint64_t values[29];
        uint32_t window_no = 0, jobs = 0, complete = 0, got, i;
        struct sxnu_control_writer writer;
        if (&native_adreno_gpu_profile_read == 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
            break;
        }
        got = native_adreno_gpu_profile_read(values, 29, &window_no, &jobs, &complete);
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " window=");
        (void)sxnu_control_writer_decimal(&writer, window_no);
        (void)sxnu_control_writer_text(&writer, " jobs=");
        (void)sxnu_control_writer_decimal(&writer, jobs);
        (void)sxnu_control_writer_text(&writer, complete ? " state=ready n=" : " state=void n=");
        (void)sxnu_control_writer_decimal(&writer, got);
        for (i = 0; i < got && i < 4 && !writer.failed; i++) {
            (void)sxnu_control_writer_text(&writer, " c");
            (void)sxnu_control_writer_decimal(&writer, i);
            (void)sxnu_control_writer_text(&writer, "=");
            (void)sxnu_control_writer_hex(&writer, values[i], 16);
        }
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_PERFCLEAR: {
        /* PERFCLEAR: no dedicated clear API; a zero-length window is enough to
         * invalidate the snapshot the next PERFREAD would see. */
        if (&native_adreno_gpu_profile_start == 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
            break;
        }
        reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "OK", "state=cleared");
        break;
    }
    case SXNU_CONTROL_VERB_GPUFREQ: {
        /* GPUFREQ: read the driver's own performance table.
         * Reading the GPUCC PLL registers directly (ml_io_map on 0x2c90000)
         * panicked with an unaligned data abort whenever the GPU was not
         * powered -- ml_io_map handed back an unusable window. The table read
         * touches no MMIO and is always safe. */
        struct sxnu_control_writer writer;
        uint32_t khz = 0, vote = 0, ready = 0;
        uint32_t ok;
        if (&native_adreno_get_gpu_freq == 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
            break;
        }
        ok = native_adreno_get_gpu_freq(&khz, &vote, &ready);
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb,
            ok ? "OK" : "ERR");
        if (ok) {
            (void)sxnu_control_writer_text(&writer, " requested_khz=");
            (void)sxnu_control_writer_decimal(&writer, khz);
            (void)sxnu_control_writer_text(&writer, " vote=");
            (void)sxnu_control_writer_hex(&writer, vote, 8);
            (void)sxnu_control_writer_text(&writer, " ready=");
            (void)sxnu_control_writer_decimal(&writer, ready);
        } else {
            (void)sxnu_control_writer_text(&writer, " state=notready");
        }
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_GPUHWCG: {
        /* GPUHWCG <on|off>: runtime clock-gating switch (A/B the stutter). */
        const uint8_t *tok[2]; size_t len[2];
        struct sxnu_control_writer writer;
        int enable = -1, rc;
        if (control_args_split(frame->args, frame->arg_bytes, tok, len, 2) != 1) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        if (len[0] == 2u && tok[0][0] == 'o' && tok[0][1] == 'n') enable = 1;
        else if (len[0] == 3u && tok[0][0] == 'o' && tok[0][1] == 'f' && tok[0][2] == 'f') enable = 0;
        if (enable < 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        if (&native_adreno_set_hwcg == 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
            break;
        }
        rc = native_adreno_set_hwcg(enable);
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb,
            rc == 0 ? "OK" : "ERR");
        (void)sxnu_control_writer_text(&writer, rc == 0 ? " hwcg=" : " state=notready rc=");
        (void)sxnu_control_writer_text(&writer, rc == 0 ? (enable ? "on" : "off") : "");
        if (rc != 0) (void)sxnu_control_writer_decimal(&writer, (uint32_t)rc);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_GPUPROFILE: {
        /* GPUPROFILE [1|0|status]: profile window control and state. No argument
         * or "status" reports; "1" arms a 3000 ms window; "0" stops it. */
        const uint8_t *tok[2]; size_t len[2];
        unsigned argc = 0;
        struct sxnu_control_writer writer;
        uint32_t window_no = 0, jobs = 0, complete = 0;
        if (&native_adreno_gpu_profile_read == 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
            break;
        }
        if (frame->arg_bytes) {
            argc = control_args_split(frame->args, frame->arg_bytes, tok, len, 2);
            if (argc == 1 && len[0] == 1u && tok[0][0] == '1') {
                if (&native_adreno_gpu_profile_start == 0) {
                    reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
                    break;
                }
                (void)native_adreno_gpu_profile_start(3000);
                sxnu_control_writer_init(&writer, reply, sizeof(reply));
                (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
                (void)sxnu_control_writer_text(&writer, " state=armed ms=3000");
                reply_bytes = sxnu_control_reply_finish(&writer);
                break;
            }
            if (argc == 1 && len[0] == 1u && tok[0][0] == '0') {
                if (&native_adreno_gpu_profile_stop == 0) {
                    reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
                    break;
                }
                native_adreno_gpu_profile_stop();
                sxnu_control_writer_init(&writer, reply, sizeof(reply));
                (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
                (void)sxnu_control_writer_text(&writer, " state=stopped");
                reply_bytes = sxnu_control_reply_finish(&writer);
                break;
            }
            if (!(argc == 1 && len[0] == 6u && tok[0][0] == 's' && tok[0][1] == 't' &&
                  tok[0][2] == 'a' && tok[0][3] == 't' && tok[0][4] == 'u' && tok[0][5] == 's')) {
                reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
                break;
            }
        }
        (void)native_adreno_gpu_profile_read(0, 0, &window_no, &jobs, &complete);
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, complete ? " state=ready window=" : " state=void window=");
        (void)sxnu_control_writer_decimal(&writer, window_no);
        (void)sxnu_control_writer_text(&writer, " jobs=");
        (void)sxnu_control_writer_decimal(&writer, jobs);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_GMUSTATE: {
        /* GMUSTATE: the GPU service's own boot progression. native_adreno_a6xx_stats
         * is written by NativeAdrenoService as bring-up advances, so this is a plain
         * memory read -- the GPUCC MMIO that panicked in kb-13/14 is untouched. */
        struct sxnu_control_writer writer;
        if (&native_adreno_a6xx_stats[0] == 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " step=");
        (void)sxnu_control_writer_decimal(&writer, native_adreno_a6xx_stats[0]);
        (void)sxnu_control_writer_text(&writer, " status=");
        (void)sxnu_control_writer_hex(&writer, native_adreno_a6xx_stats[1], 8);
        (void)sxnu_control_writer_text(&writer, " chip=");
        (void)sxnu_control_writer_hex(&writer, native_adreno_a6xx_stats[10], 8);
        (void)sxnu_control_writer_text(&writer, " fence=");
        (void)sxnu_control_writer_hex(&writer, native_adreno_a6xx_stats[8], 8);
        (void)sxnu_control_writer_text(&writer, " completed=");
        (void)sxnu_control_writer_hex(&writer, native_adreno_a6xx_stats[9], 8);
        (void)sxnu_control_writer_text(&writer, " cp=");
        (void)sxnu_control_writer_hex(&writer, native_adreno_a6xx_stats[12], 8);
        (void)sxnu_control_writer_text(&writer, " protect=");
        (void)sxnu_control_writer_hex(&writer, native_adreno_a6xx_stats[13], 8);
        /* T46-Q2: GMU HFI state, exported by NativeAdrenoService into slots the
         * driver never used for anything else. hfiQueue/hfiHeader is the message
         * queue and its head; hfiMessage/hfiError are the last exchange result.
         * A wedged GPU is usually distinguishable from an idle one right here. */
        (void)sxnu_control_writer_text(&writer, " hfi_q=");
        (void)sxnu_control_writer_hex(&writer, native_adreno_a6xx_stats[2], 8);
        (void)sxnu_control_writer_text(&writer, " hfi_hdr=");
        (void)sxnu_control_writer_hex(&writer, native_adreno_a6xx_stats[3], 8);
        (void)sxnu_control_writer_text(&writer, " hfi_msg=");
        (void)sxnu_control_writer_hex(&writer, native_adreno_a6xx_stats[14], 8);
        (void)sxnu_control_writer_text(&writer, " hfi_err=");
        (void)sxnu_control_writer_hex(&writer, native_adreno_a6xx_stats[15], 8);
        (void)sxnu_control_writer_text(&writer, " hfi_ack=");
        (void)sxnu_control_writer_decimal(&writer, native_adreno_a6xx_stats[19]);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_DISPREGS: {
        /* DISPREGS: the display counters the heartbeat prints, read from the
         * driver's exported arrays -- no MMIO, safe from any context. */
        struct sxnu_control_writer writer;
        if (&native_legacy_transfer[0] == 0 || &native_legacy_surfaces[0] == 0 ||
            &native_legacy_direct[0] == 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " name=mdp xfer=");
        (void)sxnu_control_writer_decimal(&writer, native_legacy_transfer[0]);
        (void)sxnu_control_writer_text(&writer, " nswap=");
        (void)sxnu_control_writer_decimal(&writer, native_legacy_surfaces[1]);
        (void)sxnu_control_writer_text(&writer, " nsurf=");
        (void)sxnu_control_writer_decimal(&writer, native_legacy_surfaces[0]);
        (void)sxnu_control_writer_text(&writer, " sbytes=");
        (void)sxnu_control_writer_decimal(&writer, native_legacy_surfaces[2]);
        (void)sxnu_control_writer_text(&writer, " nok=");
        (void)sxnu_control_writer_decimal(&writer, native_legacy_transfer[1]);
        (void)sxnu_control_writer_text(&writer, " ncpy=");
        (void)sxnu_control_writer_decimal(&writer, native_legacy_transfer[5]);
        (void)sxnu_control_writer_text(&writer, " nret=");
        (void)sxnu_control_writer_decimal(&writer, native_legacy_transfer[7]);
        (void)sxnu_control_writer_text(&writer, " don=");
        (void)sxnu_control_writer_decimal(&writer, native_legacy_direct[12]);
        (void)sxnu_control_writer_text(&writer, " skip=");
        (void)sxnu_control_writer_decimal(&writer, native_legacy_direct[10]);
        (void)sxnu_control_writer_text(&writer, " amb=");
        (void)sxnu_control_writer_decimal(&writer, native_legacy_direct[11]);
        (void)sxnu_control_writer_text(&writer, " lt=");
        (void)sxnu_control_writer_decimal(&writer, native_legacy_direct[13]);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_FLIPNOW: {
        /* FLIPNOW: monotonic flip / transfer counters, so the host can tell a
         * frozen composer from a slow one without reading the log ring. */
        struct sxnu_control_writer writer;
        if (&native_legacy_surfaces[0] == 0 || &native_legacy_transfer[0] == 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " flips=");
        (void)sxnu_control_writer_decimal(&writer, native_legacy_surfaces[0]);
        (void)sxnu_control_writer_text(&writer, " xfer=");
        (void)sxnu_control_writer_decimal(&writer, native_legacy_transfer[0]);
        (void)sxnu_control_writer_text(&writer, " nval=");
        (void)sxnu_control_writer_decimal(&writer,
            (&native_legacy_validation[0] == 0) ? 0 : native_legacy_validation[0]);
        (void)sxnu_control_writer_text(&writer, " nrej=");
        (void)sxnu_control_writer_decimal(&writer,
            (&native_legacy_validation[2] == 0) ? 0 : native_legacy_validation[2]);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_SMMUDUMP: {
        /* SMMUDUMP: the display SMMU bank NativeUfs qualified at boot. Layout is
         * fixed by NativeUfs.cpp dpuDisplayBankEnable(): [0] summary bitfield,
         * [4] slot|bank<<16, [5] SMR, [6] S2CR, [7] CBAR, [9] SCTLR. */
        struct sxnu_control_writer writer;
        if (&native_ufs_dpu_smmu[0] == 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
            break;
        }
        if (&native_ufs_dpu_state == 0 || native_ufs_dpu_state == 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "notready");
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " state=");
        (void)sxnu_control_writer_hex(&writer, native_ufs_dpu_state, 8);
        (void)sxnu_control_writer_text(&writer, " summary=");
        (void)sxnu_control_writer_hex(&writer, native_ufs_dpu_smmu[0], 8);
        (void)sxnu_control_writer_text(&writer, " idr0=");
        (void)sxnu_control_writer_hex(&writer, native_ufs_dpu_smmu[2], 8);
        (void)sxnu_control_writer_text(&writer, " idr1=");
        (void)sxnu_control_writer_hex(&writer, native_ufs_dpu_smmu[3], 8);
        (void)sxnu_control_writer_text(&writer, " smr=");
        (void)sxnu_control_writer_hex(&writer, native_ufs_dpu_smmu[5], 8);
        (void)sxnu_control_writer_text(&writer, " s2cr=");
        (void)sxnu_control_writer_hex(&writer, native_ufs_dpu_smmu[6], 8);
        (void)sxnu_control_writer_text(&writer, " cbar=");
        (void)sxnu_control_writer_hex(&writer, native_ufs_dpu_smmu[7], 8);
        (void)sxnu_control_writer_text(&writer, " sctlr=");
        (void)sxnu_control_writer_hex(&writer, native_ufs_dpu_smmu[9], 8);
        (void)sxnu_control_writer_text(&writer, " slot=");
        (void)sxnu_control_writer_decimal(&writer, native_ufs_dpu_smmu[4] & 0xffffu);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_DEBUGGER: {
        /* DEBUGGER [hexbytes]: enter the kernel debugger.
         *
         * The optional hex argument is queued into the KDP ring BEFORE the trap,
         * because kdp_connection_wait() reads it through kdp_getc() on entry. A
         * bare 'c' (0x63) there makes the debugger print "Continuing..." and
         * return instead of waiting forever for a KDP packet.
         *
         * The reply is queued first; the trap then blocks this thread until the
         * debugger session ends. */
        const uint8_t *tok[2]; size_t len[2];
        unsigned queued = 0;
        if (&Debugger == 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
            break;
        }
        /* T143: never nest a second Debugger().  A pipe-1 session blocks the
         * worker thread that would service this verb, so the only way to reach
         * here meanwhile is a re-entrant drain; refuse it rather than trip
         * XNU's debugger-nesting guard. */
        if (__atomic_load_n(&kdp_trap_active, __ATOMIC_ACQUIRE)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "busy");
            break;
        }
        if (frame->arg_bytes) {
            unsigned argc = control_args_split(frame->args, frame->arg_bytes, tok, len, 2);
            if (argc != 1 || (len[0] & 1u) || len[0] / 2u > 32u) {
                reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
                break;
            }
            for (size_t i = 0; i < len[0] / 2u; i++) {
                uint64_t byte = 0;
                if (!sxnu_control_parse_hex(tok[0] + i * 2u, 2u, &byte)) {
                    reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
                    goto debugger_done;
                }
                queued += (unsigned)qcom_usb_debug_kdp_push((uint8_t)byte);
            }
        }
        {
            struct sxnu_control_writer writer;
            sxnu_control_writer_init(&writer, reply, sizeof(reply));
            (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
            (void)sxnu_control_writer_text(&writer, " state=entering queued=");
            (void)sxnu_control_writer_decimal(&writer, queued);
            (void)sxnu_control_writer_text(&writer, " pending=");
            (void)sxnu_control_writer_decimal(&writer, (uint32_t)kdp_rx_used);
            (void)sxnu_control_writer_text(&writer, " ovf=");
            (void)sxnu_control_writer_decimal(&writer, (uint32_t)kdp_rx_overflow);
            reply_bytes = sxnu_control_reply_finish(&writer);
        }
        /* Arm only now: the trap is next, so the byte storms that would have
         * consumed the ring (keyboard poll, video console poll) cannot run.
         * T80: remember which bus generation this session belongs to; the
         * drain-time channel-death check compares later generations against it. */
        kdp_gen_seen = generation;
        kdp_gen_valid = 1;
        kdp_empty_drains = 0;
        __atomic_store_n(&kdp_rx_armed, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&kdp_trap_active, 1u, __ATOMIC_RELEASE);
        Debugger("qcom_usb_debug");
        __atomic_store_n(&kdp_trap_active, 0u, __ATOMIC_RELEASE);
debugger_done:
        break;
    }
    case SXNU_CONTROL_VERB_KDPPUSH: {
        /* KDPPUSH <hexbytes>: append raw bytes to the KDP ring WITHOUT trapping.
         *
         * Use this to build a KDP frame across several commands (the ring's
         * unserialiser is a persistent state machine, so order is preserved),
         * then send DEBUGGER with an empty payload to enter. Unlike DEBUGGER's
         * own argument this never enters the debugger, so a full CONNECT frame
         * can be staged in pieces. */
        const uint8_t *tok[2]; size_t len[2];
        struct sxnu_control_writer writer;
        unsigned queued = 0, rejected = 0;
        /* kb-28 (T41-c #2): KDPPUSH with no argument resets the ring.
         *
         * A chunk lost mid-frame (reply timeout, ring full, host bug) leaves
         * kdp_unserialize_packet() parked in DS_READING with a half frame in
         * dsBuffer. The next frame's 0xFA is then mistaken for the CRC
         * separator, so that whole frame is consumed too and only the one after
         * it survives. The state machine does recover by itself, but one frame
         * late -- and on this console a lost frame looks exactly like a hung
         * kernel.
         *
         * There is no way to clear dsState from here (it is file-static in
         * kdp_serial.c). But a lone 0xFB does it: in DS_READING it returns
         * DS_WAITSTART whether or not dsHaveCRC was set (with CRC set the
         * comparison simply fails and it still resets), and in DS_WAITSTART it
         * is ignored. So one END byte is a safe, unconditional resync. */
        if (!frame->arg_bytes) {
            uint32_t before = kdp_rx_overflow;
            (void)qcom_usb_debug_kdp_push(0xFB);
            sxnu_control_writer_init(&writer, reply, sizeof(reply));
            (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
            (void)sxnu_control_writer_text(&writer, " reset used=");
            (void)sxnu_control_writer_decimal(&writer, (uint32_t)kdp_rx_used);
            (void)sxnu_control_writer_text(&writer, " ovf=");
            (void)sxnu_control_writer_decimal(&writer, (uint32_t)(kdp_rx_overflow - before));
            reply_bytes = sxnu_control_reply_finish(&writer);
            break;
        }
        if (control_args_split(frame->args, frame->arg_bytes, tok, len, 2) != 1 ||
            (len[0] & 1u) || len[0] / 2u > QUD_DIAG_CHUNK) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        for (size_t i = 0; i < len[0] / 2u; i++) {
            uint64_t byte = 0;
            if (!sxnu_control_parse_hex(tok[0] + i * 2u, 2u, &byte)) {
                reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
                goto kdppush_done;
            }
            queued += (unsigned)qcom_usb_debug_kdp_push((uint8_t)byte);
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " queued=");
        (void)sxnu_control_writer_decimal(&writer, queued);
        (void)sxnu_control_writer_text(&writer, " rejected=");
        (void)sxnu_control_writer_decimal(&writer, rejected);
        (void)sxnu_control_writer_text(&writer, " used=");
        (void)sxnu_control_writer_decimal(&writer, (uint32_t)kdp_rx_used);
        (void)sxnu_control_writer_text(&writer, " ovf=");
        (void)sxnu_control_writer_decimal(&writer, (uint32_t)kdp_rx_overflow);
        reply_bytes = sxnu_control_reply_finish(&writer);
    kdppush_done:
        break;
    }
    case SXNU_CONTROL_VERB_PTDUMP: {
        /* PTDUMP <hexva>: walk the stage-1 kernel page tables and report the
         * translation. A read-only verb -- it never writes a descriptor, never
         * takes the pmap lock and never maps anything, so it is safe to call at
         * any time (see the safety note above qud_pt_walk()).
         *
         * The reply carries the same state=/crc32=/data= shape as READ so the
         * host can compare a page-table-derived PA against a hardware
         * translation: 'readpa <pa> 16' must return the same bytes as
         * 'read <va> 16'. That cross-check is the acceptance criterion. */
        const uint8_t *tok[2]; size_t len[2];
        uint64_t va = 0;
        struct qud_pt_result r;
        struct sxnu_control_writer writer;
        uint8_t probe[16];
        size_t got = 0;
        int walked;

        if (control_args_split(frame->args, frame->arg_bytes, tok, len, 2) != 1 ||
            !control_args_hex(tok[0], len[0], &va)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        memset(&r, 0, sizeof(r));
        walked = qud_pt_walk(va, &r);
        if (walked < 0) {
            /* Carry the diagnosis in the error reply: a bare "unmapped" cost a
             * whole board round on kb-42/kb-43 because it did not say WHERE the
             * walk gave up. stage= names the exact branch (see qud_pt_result),
             * root=/ttbr1= say which base was used, entry_pa= says which table
             * entry was being read. */
            struct sxnu_control_writer w2;
            sxnu_control_writer_init(&w2, reply, sizeof(reply));
            (void)sxnu_control_reply_start(&w2, frame->session, frame->seq, frame->verb, "ERR");
            (void)sxnu_control_writer_text(&w2, " state=geometry stage=");
            (void)sxnu_control_writer_decimal(&w2, r.stage);
            (void)sxnu_control_writer_text(&w2, " root=0x");
            (void)sxnu_control_writer_hex(&w2, r.root, 16);
            (void)sxnu_control_writer_text(&w2, " ttbr1=0x");
            (void)sxnu_control_writer_hex(&w2, r.ttbr1_mrs & UINT64_C(0x0000ffffffffffff), 16);
            reply_bytes = sxnu_control_reply_finish(&w2);
            break;
        }
        if (walked == 0 || !r.valid) {
            struct sxnu_control_writer w2;
            sxnu_control_writer_init(&w2, reply, sizeof(reply));
            (void)sxnu_control_reply_start(&w2, frame->session, frame->seq, frame->verb, "ERR");
            (void)sxnu_control_writer_text(&w2, " state=unmapped stage=");
            (void)sxnu_control_writer_decimal(&w2, r.stage);
            (void)sxnu_control_writer_text(&w2, " level=");
            (void)sxnu_control_writer_decimal(&w2, r.level);
            (void)sxnu_control_writer_text(&w2, " root=0x");
            (void)sxnu_control_writer_hex(&w2, r.root, 16);
            (void)sxnu_control_writer_text(&w2, " ttbr1=0x");
            (void)sxnu_control_writer_hex(&w2, r.ttbr1_mrs & UINT64_C(0x0000ffffffffffff), 16);
            (void)sxnu_control_writer_text(&w2, " src=");
            (void)sxnu_control_writer_decimal(&w2, r.source);
            (void)sxnu_control_writer_text(&w2, " t1sz=");
            (void)sxnu_control_writer_decimal(&w2, r.t1sz);
            (void)sxnu_control_writer_text(&w2, " tg1=");
            (void)sxnu_control_writer_decimal(&w2, r.tg1);
            (void)sxnu_control_writer_text(&w2, " psh=");
            (void)sxnu_control_writer_decimal(&w2, r.page_shift);
            (void)sxnu_control_writer_text(&w2, " ea=0x");
            (void)sxnu_control_writer_hex(&w2, r.last_entry_pa, 16);
            for (unsigned l = 0; l <= r.level; l++) {
                (void)sxnu_control_writer_text(&w2, " e");
                (void)sxnu_control_writer_decimal(&w2, l);
                (void)sxnu_control_writer_char(&w2, '=');
                (void)sxnu_control_writer_hex(&w2, r.entry[l], 16);
            }
            reply_bytes = sxnu_control_reply_finish(&w2);
            break;
        }
        /* Prove the translation by reading through it. A mismatch between the
         * page-table PA and the hardware's own answer would show up here. */
        got = qud_diag_read_phys(r.pa & ~UINT64_C(0xf), probe, sizeof(probe));
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " va=0x");
        (void)sxnu_control_writer_hex(&writer, va, 16);
        (void)sxnu_control_writer_text(&writer, " pa=0x");
        (void)sxnu_control_writer_hex(&writer, r.pa, 16);
        (void)sxnu_control_writer_text(&writer, " level=");
        (void)sxnu_control_writer_decimal(&writer, r.level);
        (void)sxnu_control_writer_text(&writer, " block=");
        (void)sxnu_control_writer_decimal(&writer, r.block);
        (void)sxnu_control_writer_text(&writer, " ttbr1=0x");
        (void)sxnu_control_writer_hex(&writer, r.ttbr1_mrs & UINT64_C(0x0000ffffffffffff), 16);
        (void)sxnu_control_writer_text(&writer, " root=0x");
        (void)sxnu_control_writer_hex(&writer, r.root, 16);
        (void)sxnu_control_writer_text(&writer, " src=");
        (void)sxnu_control_writer_decimal(&writer, r.source);
        (void)sxnu_control_writer_text(&writer, " t1sz=");
        (void)sxnu_control_writer_decimal(&writer, r.t1sz);
        (void)sxnu_control_writer_text(&writer, " tg1=");
        (void)sxnu_control_writer_decimal(&writer, r.tg1);
        (void)sxnu_control_writer_text(&writer, " psh=");
        (void)sxnu_control_writer_decimal(&writer, r.page_shift);
        for (unsigned l = 0; l <= r.level; l++) {
            (void)sxnu_control_writer_text(&writer, " e");
            (void)sxnu_control_writer_decimal(&writer, l);
            (void)sxnu_control_writer_char(&writer, '=');
            (void)sxnu_control_writer_hex(&writer, r.entry[l], 16);
        }
        (void)sxnu_control_writer_text(&writer, " probe=");
        for (size_t i = 0; i < got; i++)
            (void)sxnu_control_writer_hex(&writer, probe[i], 2);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_PANICDUMP: {
        /* PANICDUMP [info | log <hexoff> <hexlen>]
         *
         * Reads the embedded panic region. Pure read: no state is touched and
         * nothing is cleared, so the region survives for a later iBoot/astris
         * extraction exactly as it was found.
         *
         * info (the default) reports the header plus a 16-byte peek of the log,
         * which is enough to tell a real panic from an emptied region.
         * log <off> <len> streams a slice of the log text; <off> is relative to
         * the log start, not to the region start. */
        const uint8_t *tok[4]; size_t len[4];
        unsigned argc;
        uint64_t want_off = 0, want_len = 0;
        unsigned mode = 1;                      /* 1 = info, 2 = log */
        struct qud_panic_summary ps;
        struct sxnu_control_writer writer;
        uint8_t chunk[QUD_DIAG_CHUNK];
        char tagbuf[17];
        size_t got = 0;
        uint64_t from, take;
        int status;

        argc = control_args_split(frame->args, frame->arg_bytes, tok, len, 4);
        if (argc >= 1) {
            if (len[0] == 4u && tok[0][0] == 'i' && tok[0][1] == 'n' &&
                tok[0][2] == 'f' && tok[0][3] == 'o') {
                mode = 1;
            } else if (len[0] == 3u && tok[0][0] == 'l' && tok[0][1] == 'o' &&
                       tok[0][2] == 'g') {
                mode = 2;
            } else {
                reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
                break;
            }
        }
        if (mode == 2 &&
            (argc != 3 ||
             !control_args_hex(tok[1], len[1], &want_off) ||
             !control_args_hex(tok[2], len[2], &want_len) ||
             !want_len || want_len > QUD_DIAG_CHUNK)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }

        status = qud_panic_header(&ps);
        if (status < 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR",
                status == -2 ? "range" : "nobase");
            break;
        }
        if (status == 0) {
            /* No panic. Report the magic actually found so a caller can tell
             * "iBoot cleared it" (0x00000000) from "wrong address" (anything
             * else). */
            sxnu_control_writer_init(&writer, reply, sizeof(reply));
            (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
            (void)sxnu_control_writer_text(&writer, " state=empty magic=0x");
            (void)sxnu_control_writer_hex(&writer, ps.magic, 8);
            (void)sxnu_control_writer_text(&writer, " base=0x");
            (void)sxnu_control_writer_hex(&writer, ps.base, 16);
            (void)sxnu_control_writer_text(&writer, " size=");
            (void)sxnu_control_writer_decimal(&writer, ps.size);
            reply_bytes = sxnu_control_reply_finish(&writer);
            break;
        }

        if (mode == 2 && want_off >= ps.log_len) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "range");
            break;
        }

        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " base=0x");
        (void)sxnu_control_writer_hex(&writer, ps.base, 16);
        (void)sxnu_control_writer_text(&writer, " size=");
        (void)sxnu_control_writer_decimal(&writer, ps.size);
        (void)sxnu_control_writer_text(&writer, " version=");
        (void)sxnu_control_writer_decimal(&writer, ps.version);
        (void)sxnu_control_writer_text(&writer, " flags=0x");
        (void)sxnu_control_writer_hex(&writer, ps.flags, 16);
        (void)sxnu_control_writer_text(&writer, " logoff=");
        (void)sxnu_control_writer_decimal(&writer, ps.log_offset);
        (void)sxnu_control_writer_text(&writer, " loglen=");
        (void)sxnu_control_writer_decimal(&writer, ps.log_len);
        (void)sxnu_control_writer_text(&writer, " sslen=");
        (void)sxnu_control_writer_decimal(&writer, ps.ss_len);
        (void)sxnu_control_writer_text(&writer, " otherlen=");
        (void)sxnu_control_writer_decimal(&writer, ps.other_len);
        for (size_t i = 0; i < sizeof(tagbuf) - 1u; i++) {
            const char c = ps.os_version[i];
            tagbuf[i] = (c >= 0x20 && c < 0x7f) ? c : 0;
            if (!tagbuf[i]) break;
        }
        tagbuf[sizeof(tagbuf) - 1u] = 0;
        (void)sxnu_control_writer_text(&writer, " os=");
        (void)sxnu_control_writer_text(&writer, tagbuf);

        from = ps.base + ps.log_offset;
        if (mode == 2) {
            from += want_off;
            take = (want_len < ps.log_len - want_off) ? want_len : ps.log_len - want_off;
        } else {
            take = (ps.log_len < 16u) ? ps.log_len : 16u;   /* peek */
        }
        if (take > sizeof(chunk)) take = sizeof(chunk);
        got = qud_diag_read(from, chunk, (size_t)take);
        (void)sxnu_control_writer_text(&writer, " offset=");
        (void)sxnu_control_writer_decimal(&writer, mode == 2 ? want_off : 0);
        (void)sxnu_control_writer_text(&writer, " data=");
        for (size_t i = 0; i < got; i++)
            (void)sxnu_control_writer_hex(&writer, chunk[i], 2);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_SYSCTL: {
        /* SYSCTL <name> [<maxbytes>]: ask the kernel a sysctl question.
         *
         * Read only. The name is taken verbatim from the frame and handed to
         * sysctlbyname(), which looks it up in the kernel's own table -- so this
         * is not an arbitrary-memory read and cannot be used as one. The reply
         * carries the value as hex words, because sysctl types vary (int, quad,
         * string, struct) and guessing a type here would be wrong more often
         * than right; the caller knows what it asked for. */
        const uint8_t *tok[2]; size_t len[2];
        uint64_t cap_req = 64;
        unsigned argc;
        char namebuf[64];
        uint8_t value[192];
        size_t needed = 0;
        int got;
        struct sxnu_control_writer writer;

        argc = control_args_split(frame->args, frame->arg_bytes, tok, len, 2);
        if (argc < 1 || !len[0] || len[0] >= sizeof(namebuf)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        for (size_t i = 0; i < len[0]; i++) namebuf[i] = (char)tok[0][i];
        namebuf[len[0]] = 0;
        if (argc == 2 && (!control_args_hex(tok[1], len[1], &cap_req) ||
                          !cap_req || cap_req > sizeof(value))) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }

        got = qud_sysctl_read(namebuf, value, (size_t)cap_req, &needed);
        if (got == -1) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
            break;
        }
        if (got == -3) {
            /* Report the size so the caller can ask again with a bigger cap. */
            sxnu_control_writer_init(&writer, reply, sizeof(reply));
            (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
            (void)sxnu_control_writer_text(&writer, " name=");
            (void)sxnu_control_writer_text(&writer, namebuf);
            (void)sxnu_control_writer_text(&writer, " needed=");
            (void)sxnu_control_writer_decimal(&writer, (uint64_t)needed);
            (void)sxnu_control_writer_text(&writer, " state=toobig");
            reply_bytes = sxnu_control_reply_finish(&writer);
            break;
        }
        if (got < 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "notfound");
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " name=");
        (void)sxnu_control_writer_text(&writer, namebuf);
        (void)sxnu_control_writer_text(&writer, " len=");
        (void)sxnu_control_writer_decimal(&writer, (uint64_t)got);
        (void)sxnu_control_writer_text(&writer, " data=");
        for (int i = 0; i < got; i++)
            (void)sxnu_control_writer_hex(&writer, value[i], 2);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_TASKDUMP: {
        /* TASKDUMP [<hextask>]
         *
         * With no argument: report the exported anchors (`tasks` queue head and
         * kernproc) plus whether the queue is currently empty, and hint at what
         * to do next. With an argument: probe that caller-supplied pointer and
         * report its first 8 words raw.
         *
         * This verb does NOT decode struct task. See the design note in
         * QcomUsbDiag.c: the chain offset sits behind per-zone layout and
         * conditional compilation, and a wrong offset would report plausible
         * garbage rather than failing. Probing a pointer the caller already
         * trusts is honest; guessing a layout is not. */
        const uint8_t *tok[2]; size_t len[2];
        struct sxnu_control_writer writer;
        uint64_t want = 0;
        unsigned argc = control_args_split(frame->args, frame->arg_bytes, tok, len, 2);

        if (argc >= 1) {
            if (!control_args_hex(tok[0], len[0], &want)) {
                reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
                break;
            }
            {
                uint64_t words[8];
                if (!qud_ptr_probe(want, words, 8)) {
                    reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unmapped");
                    break;
                }
                uint32_t layout_actual = 0;
                const int layout_ok = qud_task_layout_ok(&layout_actual);
                sxnu_control_writer_init(&writer, reply, sizeof(reply));
                (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
                (void)sxnu_control_writer_text(&writer, " ptr=0x");
                (void)sxnu_control_writer_hex(&writer, want, 16);
                for (unsigned i = 0; i < 8u; i++) {
                    (void)sxnu_control_writer_text(&writer, " w");
                    (void)sxnu_control_writer_decimal(&writer, i);
                    (void)sxnu_control_writer_char(&writer, '=');
                    (void)sxnu_control_writer_hex(&writer, words[i], 16);
                }
                /* T47: decode task fields, but only when the running kernel's
                 * _proc_struct_size matches the offsets we compiled in. A stale
                 * layout would emit plausible garbage rather than failing, so the
                 * gate is the whole point. */
                if (layout_ok) {
                    uint64_t tmap = 0, tthreads = 0;
                    uint32_t nthreads = 0, prio = 0;
                    (void)sxnu_control_writer_text(&writer, " layout=ok");
                    if (qud_task_info(want, &tmap, &tthreads, &nthreads, &prio)) {
                        (void)sxnu_control_writer_text(&writer, " task_map=0x");
                        (void)sxnu_control_writer_hex(&writer, tmap, 16);
                        (void)sxnu_control_writer_text(&writer, " threads=0x");
                        (void)sxnu_control_writer_hex(&writer, tthreads, 16);
                        (void)sxnu_control_writer_text(&writer, " nthreads=");
                        (void)sxnu_control_writer_decimal(&writer, nthreads);
                        /* T50: task+0xbc is NOT a bitfield -- it is two adjacent
                         * int16_t (osfmk/kern/task.h:216-217):
                         *   int16_t priority;      base priority for threads
                         *   int16_t max_priority;  maximum priority for threads
                         * so split the word instead of reporting it raw. Measured
                         * 0x005f0051 => priority=0x51=81, max_priority=0x5f=95. */
                        (void)sxnu_control_writer_text(&writer, " prio=");
                        (void)sxnu_control_writer_decimal(&writer, (uint32_t)(int32_t)(int16_t)(prio & 0xffffu));
                        (void)sxnu_control_writer_text(&writer, " maxprio=");
                        (void)sxnu_control_writer_decimal(&writer, (uint32_t)(int32_t)(int16_t)(prio >> 16));
                        (void)sxnu_control_writer_text(&writer, " prio_raw=");
                        (void)sxnu_control_writer_hex(&writer, prio, 8);
                        (void)sxnu_control_writer_text(&writer, " task_ok=1");
                    } else {
                        (void)sxnu_control_writer_text(&writer, " task_ok=0");
                    }
                } else {
                    (void)sxnu_control_writer_text(&writer, " layout=stale actual=");
                    (void)sxnu_control_writer_hex(&writer, layout_actual, 8);
                    (void)sxnu_control_writer_text(&writer, " want=0x790");
                }
                reply_bytes = sxnu_control_reply_finish(&writer);
            }
            break;
        }

        {
            uint64_t qn = 0, qp = 0;
            const int head_ok = qud_task_qhead(&qn, &qp);
            sxnu_control_writer_init(&writer, reply, sizeof(reply));
            (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
            (void)sxnu_control_writer_text(&writer, " tasks_head=0x");
            (void)sxnu_control_writer_hex(&writer, (uint64_t)(uintptr_t)&tasks, 16);
            (void)sxnu_control_writer_text(&writer, " kernproc=0x");
            (void)sxnu_control_writer_hex(&writer, (uint64_t)(uintptr_t)kernproc, 16);
            if (head_ok) {
                (void)sxnu_control_writer_text(&writer, " next=0x");
                (void)sxnu_control_writer_hex(&writer, qn, 16);
                (void)sxnu_control_writer_text(&writer, " prev=0x");
                (void)sxnu_control_writer_hex(&writer, qp, 16);
                (void)sxnu_control_writer_text(&writer,
                    (qn == (uint64_t)(uintptr_t)&tasks) ? " state=empty" : " state=nonempty");
            } else {
                (void)sxnu_control_writer_text(&writer, " state=unreadable");
            }
            (void)sxnu_control_writer_text(&writer, " hint=give-a-pointer");
            reply_bytes = sxnu_control_reply_finish(&writer);
        }
        break;
    }
    case SXNU_CONTROL_VERB_THREADDUMP: {
        /* THREADDUMP <hexptr>: probe a thread (or any) pointer, raw words.
         * Same reasoning as TASKDUMP: report, do not decode. */
        const uint8_t *tok[2]; size_t len[2];
        struct sxnu_control_writer writer;
        uint64_t want = 0;
        uint64_t words[8];

        if (control_args_split(frame->args, frame->arg_bytes, tok, len, 2) != 1 ||
            !control_args_hex(tok[0], len[0], &want)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        if (!qud_ptr_probe(want, words, 8)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unmapped");
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " ptr=0x");
        (void)sxnu_control_writer_hex(&writer, want, 16);
        /* T47: arm64 has no thread->task field. Go t_tro(+0x438) -> *(u64*)(tro+0x28). */
        {
            uint64_t owner = 0;
            if (qud_thread_task(want, &owner)) {
                (void)sxnu_control_writer_text(&writer, " owner_task=0x");
                (void)sxnu_control_writer_hex(&writer, owner, 16);
                (void)sxnu_control_writer_text(&writer, " thread_ok=1");
            } else {
                (void)sxnu_control_writer_text(&writer, " thread_ok=0");
            }
        }
        for (unsigned i = 0; i < 8u; i++) {
            (void)sxnu_control_writer_text(&writer, " w");
            (void)sxnu_control_writer_decimal(&writer, i);
            (void)sxnu_control_writer_char(&writer, '=');
            (void)sxnu_control_writer_hex(&writer, words[i], 16);
        }
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_VSYNCON: {
        /* VSYNCON [<hz>]
         *
         * Read-only in this build. Writing display timing registers from a USB
         * worker is the single most dangerous thing this console could do --
         * wrong values desynchronise the panel, and unlike a wedge there is no
         * recovery short of a reflash. The read side reports the vsync-relevant
         * words the display driver already publishes, through the same exported
         * arrays DISPREGS uses, so no new MMIO is touched.
         *
         * A write path exists in the protocol (an argument) but is refused with
         * state=gated; enabling it needs a boot-arg plus a board round that
         * proves the panel survives. */
        const uint8_t *tok[2]; size_t len[2];
        struct sxnu_control_writer writer;
        unsigned argc = control_args_split(frame->args, frame->arg_bytes, tok, len, 2);

        if (argc >= 1) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "gated");
            break;
        }
        if (&native_legacy_transfer[0] == 0 || &native_legacy_surfaces[0] == 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " state=readonly flips=");
        (void)sxnu_control_writer_decimal(&writer, native_legacy_transfer[0]);
        (void)sxnu_control_writer_text(&writer, " xfer=");
        (void)sxnu_control_writer_decimal(&writer, native_legacy_transfer[1]);
        (void)sxnu_control_writer_text(&writer, " surfaces=");
        (void)sxnu_control_writer_decimal(&writer, native_legacy_surfaces[0]);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_BKPADD: {
        /* BKPADD <hexva> [<hexkind>]: remember a kernel address.
         * Pure module state -- no breakpoint register, no memory write. */
        const uint8_t *tok[2]; size_t len[2];
        struct sxnu_control_writer writer;
        uint64_t va = 0, kind = 0;
        unsigned argc = control_args_split(frame->args, frame->arg_bytes, tok, len, 2);
        int slot;

        if (argc < 1 || !control_args_hex(tok[0], len[0], &va) ||
            (argc >= 2 && !control_args_hex(tok[1], len[1], &kind))) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        slot = qud_bkp_add(va, (uint32_t)kind);
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb,
                                       slot >= 0 ? "OK" : "ERR");
        if (slot >= 0) {
            (void)sxnu_control_writer_text(&writer, " slot=");
            (void)sxnu_control_writer_decimal(&writer, (uint64_t)slot);
            (void)sxnu_control_writer_text(&writer, " va=0x");
            (void)sxnu_control_writer_hex(&writer, va, 16);
        } else {
            (void)sxnu_control_writer_text(&writer,
                slot == -1 ? " state=full" :
                slot == -3 ? " state=duplicate" : " state=unmapped");
        }
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_BKPDEL: {
        const uint8_t *tok[2]; size_t len[2];
        struct sxnu_control_writer writer;
        uint64_t va = 0;
        int removed;

        if (control_args_split(frame->args, frame->arg_bytes, tok, len, 2) != 1 ||
            !control_args_hex(tok[0], len[0], &va)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        removed = qud_bkp_del(va);
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb,
                                       removed ? "OK" : "ERR");
        (void)sxnu_control_writer_text(&writer, " va=0x");
        (void)sxnu_control_writer_hex(&writer, va, 16);
        (void)sxnu_control_writer_text(&writer, removed ? " state=removed" : " state=absent");
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_BKPLIST: {
        struct qud_bkp_entry list[8];
        struct sxnu_control_writer writer;
        int n = qud_bkp_list(list, 8);

        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " n=");
        (void)sxnu_control_writer_decimal(&writer, (uint64_t)n);
        for (int i = 0; i < n; i++) {
            (void)sxnu_control_writer_text(&writer, " [");
            (void)sxnu_control_writer_decimal(&writer, (uint64_t)i);
            (void)sxnu_control_writer_text(&writer, "]=0x");
            (void)sxnu_control_writer_hex(&writer, list[i].va, 16);
            (void)sxnu_control_writer_char(&writer, ':');
            (void)sxnu_control_writer_hex(&writer, list[i].kind, 8);
        }
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_SYSREG: {
        /* SYSREG <NAME>: read a system register.
         *
         * This is what makes PTDUMP checkable rather than merely self-consistent:
         * PTDUMP derives its page-table geometry from TCR_EL1 itself, so until
         * now "the geometry is right" rested on the code agreeing with itself.
         * `sysreg TCR_EL1` prints the register, and `sysreg TTBR1_EL1` prints the
         * base PTDUMP claims to start from.
         *
         * Reads have no side effects, so no gate. Names are matched against the
         * table in QcomUsbDiag.c; an unknown name reports the encoding it would
         * need so extending the table is a copy-paste. */
        const uint8_t *tok[2]; size_t len[2];
        struct sxnu_control_writer writer;
        uint64_t value = 0;

        if (control_args_split(frame->args, frame->arg_bytes, tok, len, 2) != 1 ||
            !len[0] || len[0] > 24u) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        if (!qud_sysreg_read(tok[0], len[0], &value)) {
            /* Unknown, or known but not readable at EL1. Say which, and hand
             * back the encoding when we know it. */
            unsigned o0 = 0, o1 = 0, cn = 0, cm = 0, op = 0;
            const int known = qud_sysreg_encoding(tok[0], len[0], &o0, &o1, &cn, &cm, &op);
            sxnu_control_writer_init(&writer, reply, sizeof(reply));
            (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "ERR");
            (void)sxnu_control_writer_text(&writer, " state=");
            (void)sxnu_control_writer_text(&writer, known ? "notreadable" : "unknown");
            if (known) {
                (void)sxnu_control_writer_text(&writer, " enc=");
                (void)sxnu_control_writer_decimal(&writer, o0);
                (void)sxnu_control_writer_char(&writer, '/');
                (void)sxnu_control_writer_decimal(&writer, o1);
                (void)sxnu_control_writer_char(&writer, '/');
                (void)sxnu_control_writer_decimal(&writer, cn);
                (void)sxnu_control_writer_char(&writer, '/');
                (void)sxnu_control_writer_decimal(&writer, cm);
                (void)sxnu_control_writer_char(&writer, '/');
                (void)sxnu_control_writer_decimal(&writer, op);
            }
            reply_bytes = sxnu_control_reply_finish(&writer);
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " name=");
        for (size_t i = 0; i < len[0]; i++) (void)sxnu_control_writer_char(&writer, (char)tok[0][i]);
        (void)sxnu_control_writer_text(&writer, " value=0x");
        (void)sxnu_control_writer_hex(&writer, value, 16);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_SYSREGW: {
        /* SYSREGW <NAME> <hexvalue>: write a system register.
         *
         * Gated behind nabu-sysreg-write=1, and the gate is checked before the
         * value is even parsed. SCTLR_EL1 and TCR_EL1 are one store away from a
         * machine that cannot be recovered without a reflash, and unlike a wedged
         * USB worker there is no `pa-wr:` log to explain it afterwards -- so the
         * default has to be "refuse", not "try". */
        const uint8_t *tok[2]; size_t len[2];
        struct sxnu_control_writer writer;
        uint64_t value = 0;
        int rc;

        if (control_args_split(frame->args, frame->arg_bytes, tok, len, 2) != 2 ||
            !len[0] || len[0] > 24u ||
            !control_args_hex(tok[1], len[1], &value)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        rc = qud_sysreg_write(tok[0], len[0], value);
        if (rc < 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "gated");
            break;
        }
        if (rc == 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "notwritable");
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " name=");
        for (size_t i = 0; i < len[0]; i++) (void)sxnu_control_writer_char(&writer, (char)tok[0][i]);
        (void)sxnu_control_writer_text(&writer, " wrote=0x");
        (void)sxnu_control_writer_hex(&writer, value, 16);
        /* Deliberately NOT reading back. giulia's reg.write is documented as
         * not reading back to protect W1C bits, and that is the right call for a
         * register console: a read-back can itself have side effects. The
         * caller verifies with a separate SYSREG, which is explicit about it. */
        (void)sxnu_control_writer_text(&writer, " verify=separate");
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_I2CREAD:
    case SXNU_CONTROL_VERB_SPIREAD: {
        /* Not built, on purpose. See the header of QcomUsbBusPrims.c: a GENI
         * transfer needs the serial-engine clocks, pinmux and protocol engine
         * brought up first, and this port's device tree ships every spi@/i2c@
         * node as status="disabled" with no XNU-side GENI driver. The verbs
         * exist so a host script gets a definite answer instead of a timeout;
         * the argument list is intentionally not parsed because there is no
         * transaction to parameterise yet. */
        struct sxnu_control_writer writer;
        const char *bus = frame->verb == SXNU_CONTROL_VERB_I2CREAD ? "i2c" : "spi";

        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "ERR");
        (void)sxnu_control_writer_text(&writer, " state=notavail bus=");
        (void)sxnu_control_writer_text(&writer, bus);
        (void)sxnu_control_writer_text(&writer, " need=W1-driver+geni-clocks+pinmux");
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_GPIOGET:
    case SXNU_CONTROL_VERB_GPIODIR: {
        /* GPIOGET <index> / GPIODIR <index>: the two raw words of one TLMM
         * GPIO block. Read-only, gated by nabu-verbs-bus=1. T149: an index
         * outside the verified block set is refused before any address is
         * mapped, so an unverified block can no longer reach MMIO -- that is
         * the path on which GPIOGET 1 (0x03001000) wedged the CPU. Both words
         * are reported under neutral names: the per-block register layout is
         * not confirmed on this board (see QcomUsbBusPrims.c header). */
        const uint8_t *tok[1]; size_t len[1];
        struct sxnu_control_writer writer;
        struct nabu_gpio_words words;
        uint64_t index = 0;
        int gpioret;

        if (!nabu_bus_enabled()) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "disabled");
            break;
        }
        if (control_args_split(frame->args, frame->arg_bytes, tok, len, 1) != 1 ||
            !len[0] || len[0] > 8u ||
            !control_args_hex(tok[0], len[0], &index) || index >= NABU_BUS_GPIO_MAX) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        gpioret = nabu_bus_gpio_read((unsigned)index, &words);
        if (gpioret == NABU_GPIO_DENIED) {
            /* Refused before any mapping. The log line emitted inside the
             * primitive names the PA that was denied. */
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR",
                                               "denied");
            break;
        }
        if (gpioret != NABU_GPIO_OK) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "read");
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " gpio=");
        (void)sxnu_control_writer_decimal(&writer, index);
        (void)sxnu_control_writer_text(&writer, " w0=0x");
        (void)sxnu_control_writer_hex(&writer, words.w0, 8);
        (void)sxnu_control_writer_text(&writer, " w1=0x");
        (void)sxnu_control_writer_hex(&writer, words.w1, 8);
        if (frame->verb == SXNU_CONTROL_VERB_GPIODIR)
            (void)sxnu_control_writer_text(&writer, " dirfield=unverified");
        (void)sxnu_control_writer_text(&writer, " layout=unverified");
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_SYSREGSCAN: {
        /* SYSREGSCAN <start> <count>: rows [start, start+count) of the
         * generated table. Row group 0 is readable at EL1 by construction;
         * group 1 (the expected-UNDEFINED probe) needs nabu-sysregscan-trap=1.
         * Replies are text hex so an embedded 0x0a byte cannot truncate the
         * frame, and the fault bitmap replaces sentinel-value guessing. */
        const uint8_t *tok[2]; size_t len[2];
        struct sxnu_control_writer writer;
        struct nabu_scan_result res[NABU_SCAN_MAX];
        uint64_t start = 0, count = 0;
        uint32_t fault_mask = 0;
        int rc;

        if (!nabu_scan_enabled()) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "disabled");
            break;
        }
        if (control_args_split(frame->args, frame->arg_bytes, tok, len, 2) != 2 ||
            !len[0] || len[0] > 9u || !len[1] || len[1] > 9u ||
            !control_args_hex(tok[0], len[0], &start) ||
            !control_args_hex(tok[1], len[1], &count) ||
            start > NABU_SCAN_ROWS || count == 0 || count > NABU_SCAN_MAX) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        rc = nabu_sysreg_scan((unsigned)start, (unsigned)count, res, NABU_SCAN_MAX,
                              nabu_scan_trap_allowed(), nabu_scan_exec, NULL);
        if (rc < 0) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR",
                                               rc == -2 ? "range" : "args");
            break;
        }
        for (int i = 0; i < rc; i++)
            if (res[i].faulted) fault_mask |= (uint32_t)(1u << i);
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " start=");
        (void)sxnu_control_writer_decimal(&writer, start);
        (void)sxnu_control_writer_text(&writer, " count=");
        (void)sxnu_control_writer_decimal(&writer, (uint64_t)rc);
        (void)sxnu_control_writer_text(&writer, " faults=0x");
        (void)sxnu_control_writer_hex(&writer, fault_mask, 8);
        (void)sxnu_control_writer_text(&writer, " data=");
        for (int i = 0; i < rc; i++) {
            (void)sxnu_control_writer_hex(&writer, res[i].enc, 8);
            (void)sxnu_control_writer_hex(&writer, res[i].value, 16);
        }
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_GICSNAP: {
        /* GICSNAP: distributor control/identity/status plus one redistributor
         * type word per CPU that answered. Every register read here is a plain
         * status read -- no ICC/IAR (read-to-ack) and no GICD_ISPENDR, which
         * would change interrupt state (T49 risk R1). */
        struct sxnu_control_writer writer;
        struct nabu_gic_snap snap;

        if (!nabu_bus_enabled()) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "disabled");
            break;
        }
        if (!nabu_bus_gic_snapshot(&snap)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "read");
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " dist.ctlr=0x");
        (void)sxnu_control_writer_hex(&writer, snap.dist.ctlr, 8);
        (void)sxnu_control_writer_text(&writer, " dist.typer=0x");
        (void)sxnu_control_writer_hex(&writer, snap.dist.typer, 8);
        (void)sxnu_control_writer_text(&writer, " dist.iidr=0x");
        (void)sxnu_control_writer_hex(&writer, snap.dist.iidr, 8);
        (void)sxnu_control_writer_text(&writer, " dist.statusr=0x");
        (void)sxnu_control_writer_hex(&writer, snap.dist.statusr, 8);
        for (unsigned i = 0; i < NABU_BUS_GICR_COUNT; i++) {
            if (!(snap.rd_ok & (1u << i))) continue;
            (void)sxnu_control_writer_text(&writer, " rd");
            (void)sxnu_control_writer_decimal(&writer, (uint64_t)i);
            (void)sxnu_control_writer_text(&writer, ".typer=0x");
            (void)sxnu_control_writer_hex(&writer, snap.rd[i].typer, 8);
        }
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_TRACEON: {
        /* TRACEON <interval-ms> <type-mask>: start periodic sampling.
         * Interval is decimal milliseconds (1..60000); mask is the usual
         * bare-hex-or-0x bitmask of QUD_TRACE_TYPE_*. */
        struct sxnu_control_writer writer;
        uint32_t interval_ms = 0, mask = 0;

        if (!qud_trace_bootarg_gate) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "disabled");
            break;
        }
        if (!qud_trace_parse_start(frame->args, frame->arg_bytes, &interval_ms, &mask)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        qud_trace_start(interval_ms, mask);
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " state=on interval=");
        (void)sxnu_control_writer_decimal(&writer, interval_ms);
        (void)sxnu_control_writer_text(&writer, " mask=0x");
        (void)sxnu_control_writer_hex(&writer, mask, 8);
        (void)sxnu_control_writer_text(&writer, " cap=");
        (void)sxnu_control_writer_decimal(&writer, QUD_TRACE_RECORDS_MAX);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_TRACEREAD: {
        /* TRACEREAD [start] [count]: oldest-first window of the ring, as raw
         * record bytes (16 bytes per record). */
        struct qud_trace_rec recs[QUD_TRACE_READ_MAX];
        struct sxnu_control_writer writer;
        const uint8_t *data;
        uint32_t valid = 0, start = 0, want = 0, got, base, i;

        if (!qud_trace_bootarg_gate) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "disabled");
            break;
        }
        if (!qud_trace_parse_read(frame->args, frame->arg_bytes, &start, &want)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        valid = qud_trace_valid();
        got = start < valid ? qud_trace_read(start, want, recs) : 0;
        base = qud_trace_oldest_seq(valid) + start;
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, start < valid ? " state=ok" : " state=done");
        (void)sxnu_control_writer_text(&writer, " base=");
        (void)sxnu_control_writer_decimal(&writer, base);
        (void)sxnu_control_writer_text(&writer, " valid=");
        (void)sxnu_control_writer_decimal(&writer, valid);
        (void)sxnu_control_writer_text(&writer, " dropped=");
        (void)sxnu_control_writer_decimal(&writer,
            (uint32_t)__atomic_load_n(&qud_trace_dropped, __ATOMIC_RELAXED));
        (void)sxnu_control_writer_text(&writer, " more=");
        (void)sxnu_control_writer_decimal(&writer, (start + got < valid) ? 1u : 0u);
        (void)sxnu_control_writer_text(&writer, " count=");
        (void)sxnu_control_writer_decimal(&writer, got);
        if (got) {
            (void)sxnu_control_writer_text(&writer, " data=");
            data = (const uint8_t *)recs;
            for (i = 0; i < got * sizeof(struct qud_trace_rec) && !writer.failed; i++)
                (void)sxnu_control_writer_hex(&writer, data[i], 2);
        }
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_TRACECLEAR: {
        /* TRACECLEAR: drop the ring.  Sampling keeps its mask and interval, so
         * an operator can empty the buffer without restarting the session. */
        struct sxnu_control_writer writer;

        if (!qud_trace_bootarg_gate) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "disabled");
            break;
        }
        qud_trace_clear();
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " state=cleared");
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_TELDUMP: {
        /* TELDUMP <hexpa> <hexlen> <tag>: synchronous first-block probe into
         * the NABUTEL1 window through native_ufs_tel_*.  The four-slot async
         * worker and the 64-byte record framing belong to the storage driver
         * (T103 section 4.2); this lands the ABI plus one blocking write so the
         * host can prove hook and medium in a single command. */
        static uint8_t tel_scratch[4096];
        const uint8_t *tag = (const uint8_t *)"tel"; size_t taglen = 3;
        struct sxnu_control_writer writer;
        uint64_t pa = 0; uint32_t blocks = 0;
        size_t got;
        int synced = 0;

        if (!qud_tel_bootarg_gate) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "disabled");
            break;
        }
        if (!qud_tel_available()) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
            break;
        }
        if (!qud_tel_parse_dump(frame->args, frame->arg_bytes, &pa, &blocks, &tag, &taglen) ||
            !qud_read_pa_allowed(pa, (size_t)blocks * 4096u, NULL)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        got = qud_diag_read_phys_anno(pa, tel_scratch, 4096u, 1);
        if (got != 4096u) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "read");
            break;
        }
        if (!native_ufs_tel_write(0u, 1u, tel_scratch)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "write");
            break;
        }
        if (&native_ufs_tel_sync != 0 && native_ufs_tel_sync()) synced = 1;
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " state=queued slot=0 est=");
        (void)sxnu_control_writer_decimal(&writer, (uint64_t)blocks * 4096u);
        (void)sxnu_control_writer_text(&writer, " blocks_written=1 write_err=0 synced=");
        (void)sxnu_control_writer_decimal(&writer, (uint64_t)synced);
        (void)sxnu_control_writer_text(&writer, " tag=");
        for (size_t i = 0; i < taglen; i++)
            (void)sxnu_control_writer_char(&writer, (char)tag[i]);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_TELREAD: {
        /* TELREAD <relblock> [@off]: mirror READUFS against the telemetry
         * window -- one 4 KiB block, 64 bytes of hex starting at <off>. */
        static uint8_t tel_block[4096];
        const uint8_t *tok[2]; size_t len[2];
        struct sxnu_control_writer writer;
        uint64_t rel = 0, offset = 0;
        unsigned argc = control_args_split(frame->args, frame->arg_bytes, tok, len, 2);

        if (!qud_tel_bootarg_gate) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "disabled");
            break;
        }
        if (!qud_tel_available()) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
            break;
        }
        if (argc < 1 || argc > 2 ||
            !control_args_hex(tok[0], len[0], &rel) ||
            (argc == 2 && !control_args_hex(tok[1], len[1], &offset)) ||
            (offset & 0x3fu) || offset > 4096u - 64u) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        if (!native_ufs_tel_read(rel, 1u, tel_block)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "read");
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " state=ready rel=");
        (void)sxnu_control_writer_decimal(&writer, (uint32_t)rel);
        (void)sxnu_control_writer_text(&writer, " crc32=");
        {
            uint32_t crc = 0xffffffffu;
            for (uint32_t i = 0; i < 4096u; i++) {
                crc ^= tel_block[i];
                for (unsigned bit = 0; bit < 8; bit++)
                    crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
            }
            (void)sxnu_control_writer_hex(&writer, (uint64_t)(crc ^ 0xffffffffu), 8);
        }
        (void)sxnu_control_writer_text(&writer, " off=");
        (void)sxnu_control_writer_decimal(&writer, (uint32_t)offset);
        (void)sxnu_control_writer_text(&writer, " data=");
        for (uint32_t i = 0; i < 64u; i++)
            (void)sxnu_control_writer_hex(&writer, tel_block[offset + i], 2);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_SYM:
    case SXNU_CONTROL_VERB_SYMS:
        /* The kernel symbol table is not reachable from this module; the host
         * resolves addresses from the boot kernelcache and uses READ. */
        reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "unsupported");
        break;
    case SXNU_CONTROL_VERB_REBOOT:
        if (!__atomic_load_n(&control_hook_owned, __ATOMIC_ACQUIRE))
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "nohook");
        else if (__atomic_load_n(&control_reboot_requested, __ATOMIC_ACQUIRE) ||
                 __atomic_load_n(&control_reboot_taken, __ATOMIC_ACQUIRE))
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "busy");
        else {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "OK", "queued");
            reboot_accepted = 1;
        }
        break;
    case SXNU_CONTROL_VERB_DEVTREE: {
        /* DEVTREE [path] [max]: dump device-tree node names and a few key
         * properties. The walk itself is kernel/QcomUsbDTDebug.cpp calling the
         * IOKit registry; this handler only checks arguments and frames text.
         * path defaults to "/" and max (child entries) to 4 so the reply stays
         * inside the 512-byte cap; truncated dumps say trunc=1. */
        const uint8_t *tok[2];
        size_t len[2];
        struct sxnu_control_writer writer;
        char path[64];
        char body[384];
        const char *why = "";
        size_t used = 0;
        int truncated = 0;
        uint64_t max_entries = 4;
        int argc = control_args_split(frame->args, frame->arg_bytes, tok, len, 2);
        path[0] = '/';
        path[1] = 0;
        for (int ai = 0; ai < argc && ai < 2; ai++) {
            if (len[ai] && tok[ai][0] == (uint8_t)'/') {
                if (!qud_devtree_path_valid((const char *)tok[ai], len[ai]) ||
                    len[ai] >= sizeof(path)) {
                    reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
                    break;
                }
                for (size_t i = 0; i < len[ai]; i++) path[i] = (char)tok[ai][i];
                path[len[ai]] = 0;
            } else if (!control_args_hex(tok[ai], len[ai], &max_entries) ||
                       !max_entries || max_entries > 16u) {
                reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
                break;
            }
        }
        if (reply_bytes) break;
        /* T150: optional off-console execution. Default off keeps the
         * synchronous walk below unchanged. */
        if (qud_dt_thread_enabled()) {
            reply_bytes = qud_dt_thread_dispatch(reply, sizeof(reply), frame, path,
                                                 max_entries);
            break;
        }
        if (!native_dt_explore(path, body, sizeof(body), &used, &why,
                               (uint32_t)max_entries, &truncated)) {
            sxnu_control_writer_init(&writer, reply, sizeof(reply));
            (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "ERR");
            (void)sxnu_control_writer_text(&writer, " state=");
            (void)sxnu_control_writer_text(&writer, why);
            (void)sxnu_control_writer_text(&writer, " path=");
            (void)sxnu_control_writer_text(&writer, path);
            reply_bytes = sxnu_control_reply_finish(&writer);
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " state=ready path=");
        (void)sxnu_control_writer_text(&writer, path);
        (void)sxnu_control_writer_text(&writer, " bytes=");
        (void)sxnu_control_writer_decimal(&writer, used);
        (void)sxnu_control_writer_text(&writer, " trunc=");
        (void)sxnu_control_writer_decimal(&writer, (uint64_t)truncated);
        (void)sxnu_control_writer_char(&writer, '\n');
        (void)sxnu_control_writer_put(&writer, body, used);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_FBRAW: {
        /* FBRAW <pa|gop> <hexoff> [<hexlen>] [chunk] [raw]
         *
         * Raw framebuffer reads. A physical address goes through the same
         * whitelist READPA uses; "gop" copies from the fixed framebuffer the
         * display driver registered (qcom_usb_shot_framebuffer). One command
         * moves one chunk; the host advances <off> until last=1.
         *
         * Default output is hex inside the SXNU1R reply (chunk capped at 128
         * bytes so header+data stays under the 512-byte cap). The optional
         * "raw" token instead emits a framed binary record
         *   SXNUFBR1 <off:8hex> <crc:8hex> <len:4hex>\n<raw bytes>
         * through the shared CDC log path (qcom_usb_debug_write), and the
         * control reply reports ring backlog so a lost stream is visible. */
        static uint8_t fbraw_buf[4096];
        const uint8_t *tok[5];
        size_t len[5];
        struct sxnu_control_writer writer;
        struct qcom_usb_framebuffer fb;
        uint64_t off = 0, total = 0, chunk_arg = 128, pa = 0;
        uint32_t chunk = 0, cap = 128;
        int last = 0, raw = 0, is_gop = 0, argc;
        int ai;
        argc = control_args_split(frame->args, frame->arg_bytes, tok, len, 5);
        if (argc < 2) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        if (len[0] == 3u && tok[0][0] == (uint8_t)'g' && tok[0][1] == (uint8_t)'o' &&
            tok[0][2] == (uint8_t)'p') {
            is_gop = 1;
            if (!qcom_usb_shot_framebuffer(&fb)) {
                reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "nofb");
                break;
            }
        } else if (!control_args_hex(tok[0], len[0], &pa)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        if (!control_args_hex(tok[1], len[1], &off) || off > (1u << 30)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        for (ai = 2; ai < argc && ai < 5; ai++) {
            if (len[ai] == 3u && tok[ai][0] == (uint8_t)'r' &&
                tok[ai][1] == (uint8_t)'a' && tok[ai][2] == (uint8_t)'w') {
                raw = 1;
                continue;
            }
            if (!total) {
                if (!control_args_hex(tok[ai], len[ai], &total) ||
                    !total || total > (16u << 20)) {
                    reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
                    break;
                }
            } else if (!control_args_hex(tok[ai], len[ai], &chunk_arg) ||
                       !chunk_arg || chunk_arg > 4096u) {
                reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
                break;
            }
        }
        if (reply_bytes) break;
        if (is_gop) {
            if (off >= fb.bytes) {
                reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "eof");
                break;
            }
            if (!total) total = fb.bytes - off;
            if (total > fb.bytes - off) total = fb.bytes - off;
        } else if (!total) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        cap = raw ? 4096u : 128u;
        if (chunk_arg > cap) chunk_arg = cap;
        if (!qud_fbraw_plan(off, total, (uint32_t)chunk_arg, &chunk, &last)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        if (chunk > sizeof(fbraw_buf)) chunk = sizeof(fbraw_buf);
        if (is_gop) {
            for (uint32_t i = 0; i < chunk; i++)
                fbraw_buf[i] = fb.pixels[off + (uint64_t)i];
        } else {
            const char *allow_why = "";
            if (!qud_read_pa_allowed(pa + off, (size_t)chunk, &allow_why) ||
                qud_diag_read_phys(pa + off, fbraw_buf, (size_t)chunk) != chunk) {
                reply_bytes = control_reply_reboot(reply, sizeof(reply), frame,
                    "ERR", allow_why && *allow_why ? allow_why : "read");
                break;
            }
        }
        if (raw) {
            char head[48];
            size_t head_bytes = 0;
            uint32_t crc = sxnu_control_crc32(fbraw_buf, chunk);
            static const char hexal[] = "0123456789abcdef";
            for (size_t i = 0; i < 9; i++) head[head_bytes++] = "SXNUFBR1 "[i];
            for (int k = 7; k >= 0; k--)
                head[head_bytes++] = hexal[(unsigned)((off >> (k * 4)) & 15u)];
            head[head_bytes++] = ' ';
            for (int k = 7; k >= 0; k--)
                head[head_bytes++] = hexal[(crc >> (k * 4)) & 15u];
            head[head_bytes++] = ' ';
            for (int k = 3; k >= 0; k--)
                head[head_bytes++] = hexal[(chunk >> (k * 4)) & 15u];
            head[head_bytes++] = '\n';
            (void)qcom_usb_debug_write(head, head_bytes);
            if (chunk) (void)qcom_usb_debug_write(fbraw_buf, chunk);
            sxnu_control_writer_init(&writer, reply, sizeof(reply));
            (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
            (void)sxnu_control_writer_text(&writer, " state=raw src=");
            (void)sxnu_control_writer_text(&writer, is_gop ? "gop" : "pa");
            (void)sxnu_control_writer_text(&writer, " off=");
            (void)sxnu_control_writer_decimal(&writer, off);
            (void)sxnu_control_writer_text(&writer, " chunk=");
            (void)sxnu_control_writer_decimal(&writer, chunk);
            (void)sxnu_control_writer_text(&writer, " pending=");
            (void)sxnu_control_writer_decimal(&writer, status->buffer_pending);
            (void)sxnu_control_writer_text(&writer, " overflow=");
            (void)sxnu_control_writer_decimal(&writer, status->buffer_overflow);
            reply_bytes = sxnu_control_reply_finish(&writer);
            break;
        }
        {
            uint32_t crc = sxnu_control_crc32(fbraw_buf, chunk);
            sxnu_control_writer_init(&writer, reply, sizeof(reply));
            (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
            (void)sxnu_control_writer_text(&writer, " state=ready src=");
            (void)sxnu_control_writer_text(&writer, is_gop ? "gop" : "pa");
            if (!is_gop) {
                (void)sxnu_control_writer_text(&writer, " pa=0x");
                (void)sxnu_control_writer_hex(&writer, pa, 16);
            }
            (void)sxnu_control_writer_text(&writer, " off=");
            (void)sxnu_control_writer_decimal(&writer, off);
            (void)sxnu_control_writer_text(&writer, " len=");
            (void)sxnu_control_writer_decimal(&writer, total);
            (void)sxnu_control_writer_text(&writer, " chunk=");
            (void)sxnu_control_writer_decimal(&writer, chunk);
            (void)sxnu_control_writer_text(&writer, " last=");
            (void)sxnu_control_writer_decimal(&writer, (uint64_t)last);
            (void)sxnu_control_writer_text(&writer, " crc32=");
            (void)sxnu_control_writer_hex(&writer, crc, 8);
            (void)sxnu_control_writer_text(&writer, " data=");
            for (uint32_t i = 0; i < chunk; i++)
                (void)sxnu_control_writer_hex(&writer, fbraw_buf[i], 2);
            reply_bytes = sxnu_control_reply_finish(&writer);
        }
        break;
    }
    case SXNU_CONTROL_VERB_NABULOGRING: {
        /* NABULOGRING [period|panic] [<hexoff> [<hexlen>]]
         *
         * The two NABULOG1 slots at the tail of the diagnostic log window:
         * periodic = window block 0, panic = window blocks - 2. Geometry comes
         * from qud_nabulog_open() (live, not hard-coded); the measured anchors
         * are echoed so the host can cross-check. No arguments: both headers.
         * One slot name: that header. Slot plus a window: body bytes from the
         * 4 KiB block, hex encoded (<= 96 bytes to stay under the reply cap). */
        static uint8_t blk[4096];
        const uint8_t *tok[3];
        size_t len[3];
        struct sxnu_control_writer writer;
        struct qud_nabulog_header h;
        uint64_t first = 0, blocks = 0, rel = 0, off = 0, blen = 0, want = 0;
        const char *why = "";
        int slot = -1, argc, why_block = 0, parsed = -1;
        argc = control_args_split(frame->args, frame->arg_bytes, tok, len, 3);
        if (argc > 3) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        if (argc >= 1) {
            static const char period_name[] = "period";
            static const char panic_name[] = "panic";
            size_t i;
            slot = -1;
            if (len[0] == 6u) {
                for (i = 0; i < 6u && tok[0][i] == (uint8_t)period_name[i]; i++) {}
                if (i == 6u) slot = 0;
            }
            if (slot < 0 && len[0] == 5u) {
                for (i = 0; i < 5u && tok[0][i] == (uint8_t)panic_name[i]; i++) {}
                if (i == 5u) slot = 1;
            }
            if (slot < 0) {
                reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
                break;
            }
            if (argc >= 2 && (!control_args_hex(tok[1], len[1], &off) || off >= 4096u)) {
                reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
                break;
            }
            if (argc == 3 && !control_args_hex(tok[2], len[2], &blen)) {
                reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
                break;
            }
        }
        if (!qud_nabulog_open(&first, &blocks, &why)) {
            sxnu_control_writer_init(&writer, reply, sizeof(reply));
            (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "ERR");
            (void)sxnu_control_writer_text(&writer, " state=");
            (void)sxnu_control_writer_text(&writer, why);
            reply_bytes = sxnu_control_reply_finish(&writer);
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " state=ready first=");
        (void)sxnu_control_writer_decimal(&writer, first);
        (void)sxnu_control_writer_text(&writer, " blocks=");
        (void)sxnu_control_writer_decimal(&writer, blocks);
        (void)sxnu_control_writer_text(&writer, " anchor_match=");
        (void)sxnu_control_writer_decimal(&writer,
            (first == 121718715ull && blocks >= 2u && first + blocks - 2u == 121726905ull) ? 1u : 0u);
        for (int s = 0; s < 2; s++) {
            if (slot >= 0 && slot != s) continue;
            if (!qud_nabulog_slot_rel(blocks, s == 1, &rel)) {
                (void)sxnu_control_writer_text(&writer, " slot=");
                (void)sxnu_control_writer_text(&writer, s ? "panic" : "period");
                (void)sxnu_control_writer_text(&writer, " state=geom");
                continue;
            }
            if (!qud_log_ufs_read_block(rel, blk, &why_block))
                break;
            parsed = qud_nabulog_header_parse(blk, 4096u, &h);
            (void)sxnu_control_writer_text(&writer, " slot=");
            (void)sxnu_control_writer_text(&writer, s ? "panic" : "period");
            (void)sxnu_control_writer_text(&writer, " rel=");
            (void)sxnu_control_writer_decimal(&writer, rel);
            (void)sxnu_control_writer_text(&writer, " lba=");
            (void)sxnu_control_writer_decimal(&writer, first + rel);
            (void)sxnu_control_writer_text(&writer, " hdr=");
            (void)sxnu_control_writer_decimal(&writer, (uint64_t)parsed);
            if (parsed != 1) continue;
            (void)sxnu_control_writer_text(&writer, " version=");
            (void)sxnu_control_writer_decimal(&writer, h.version);
            (void)sxnu_control_writer_text(&writer, " is_panic=");
            (void)sxnu_control_writer_decimal(&writer, h.is_panic);
            (void)sxnu_control_writer_text(&writer, " available=");
            (void)sxnu_control_writer_decimal(&writer, h.available);
            (void)sxnu_control_writer_text(&writer, " dropped=");
            (void)sxnu_control_writer_decimal(&writer, h.dropped);
            (void)sxnu_control_writer_text(&writer, " total=");
            (void)sxnu_control_writer_decimal(&writer,
                ((uint64_t)h.total_hi << 32) | h.total_lo);
            if (slot == s && (argc >= 2 || blen)) {
                if (!qud_nabulog_body_window(off, blen, h.available, &want)) {
                    (void)sxnu_control_writer_text(&writer, " win=badoff");
                    continue;
                }
                if (want > 96u) want = 96u;
                (void)sxnu_control_writer_text(&writer, " off=");
                (void)sxnu_control_writer_decimal(&writer, off);
                (void)sxnu_control_writer_text(&writer, " n=");
                (void)sxnu_control_writer_decimal(&writer, want);
                (void)sxnu_control_writer_text(&writer, " crc32=");
                (void)sxnu_control_writer_hex(&writer,
                    sxnu_control_crc32(blk + off, (size_t)want), 8);
                (void)sxnu_control_writer_text(&writer, " data=");
                for (uint64_t i = 0; i < want; i++)
                    (void)sxnu_control_writer_hex(&writer, blk[off + i], 2);
            }
        }
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_FGREAD: {
        /* T144 / T91 P1: one read-only pass over the PM8150B Gen4 fuel gauge.
         * Optional decimal argument overrides this reply's capacity basis
         * (mAh); nabu-fg-design-mah is the persistent knob.  The [fg] line is
         * also pushed to the console stream so a headless capture keeps the
         * sample even if the request frame itself never reaches the host. */
        struct nabu_fg_sample sample;
        struct sxnu_control_writer writer;
        const uint8_t *tok[1]; size_t len[1];
        char line[192];
        size_t used;
        unsigned argc;
        uint64_t mah = 0;

        if (!nabu_fg_enabled()) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "disabled");
            break;
        }
        argc = control_args_split(frame->args, frame->arg_bytes, tok, len, 1);
        if (argc > 1 || (argc == 1 && (!len[0] || len[0] > 6u ||
            !control_args_decimal(tok[0], len[0], &mah) ||
            mah < 1000u || mah > 20000u))) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "args");
            break;
        }
        nabu_fg_set_design_mah(argc == 1 ? (uint32_t)mah : 0u);
        (void)nabu_fg_read(&sample);
        used = nabu_fg_format(line, sizeof(line), &sample);
        if (used && used < sizeof(line)) {
            (void)qcom_usb_debug_write(line, used);
            (void)qcom_usb_debug_write("\n", 1);
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb,
                                       sample.status == NABU_FG_OK ? "OK" : "ERR");
        (void)sxnu_control_writer_text(&writer, " ");
        (void)sxnu_control_writer_text(&writer, line);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_PIPE1STAT: {
        /* T151: read-only pipe1 wiring state.  Answers the P1-vs-P2
         * question without a host-side USB capture: dtr1 tells whether the
         * host driver completed SET_CONTROL_LINE_STATE on interface 2,
         * tx2q is the pipe1 TX ring occupancy, epa9 is the armed state of
         * physical EP_IN2 (0x84), dale is DALEPENA and rx2 is the pipe1 RX
         * ring occupancy.  No MMIO is touched beyond dwc3_snapshot(). */
        struct dwc3_snapshot snap;
        struct sxnu_control_writer writer;
        if (!native.core_initialized) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "noinit");
            break;
        }
        dwc3_snapshot(native.core, &snap);
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " dtr1=");
        (void)sxnu_control_writer_decimal(&writer, (snap.dtr >> 1) & 1u);
        (void)sxnu_control_writer_text(&writer, " tx2q=");
        (void)sxnu_control_writer_decimal(&writer, dwc3_queued_pipe(native.core, 1u));
        (void)sxnu_control_writer_text(&writer, " epa9=");
        (void)sxnu_control_writer_decimal(&writer, (snap.active_endpoints >> 9) & 1u);
        (void)sxnu_control_writer_text(&writer, " dale=");
        (void)sxnu_control_writer_hex(&writer, snap.enabled_endpoints, 8);
        (void)sxnu_control_writer_text(&writer, " rx2=");
        (void)sxnu_control_writer_decimal(&writer, dwc3_rx_pending_pipe(native.core, 1u));
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    default:
        return;
    }
    if (!reply_bytes) {
        (void)__atomic_add_fetch(&control_rejected, 1u, __ATOMIC_RELAXED);
        return;
    }
    queued = control_reply_queue(reply, reply_bytes, pipe) == reply_bytes;
    /* T80: an ack lost to a full TX ring is not a parser rejection. Count it so
     * STATUS can tell the two apart (rl= vs rej=). Behaviour is unchanged: the
     * REBOOT path is fire-and-forget by design -- the reset happens even when
     * the OK frame never reaches the wire (measured 2026-11-25: REBOOT seq=9
     * unanswered, device in recovery at +5 min), so there is nothing to retry
     * here. */
    if (!queued)
        (void)__atomic_add_fetch(&control_reply_lost, 1u, __ATOMIC_RELAXED);
    if (!queued || !sxnu_control_commit(&usb_control_context.state, frame, accepted)) {
        (void)__atomic_add_fetch(&control_rejected, 1u, __ATOMIC_RELAXED);
        return;
    }
    if (shot_bytes) (void)qcom_usb_shot_commit(shot_bytes);
    if (accepted == SXNU_CONTROL_VERB_REBOOT && reboot_accepted) {
        qcom_usb_shot_cancel();
        __atomic_store_n(&control_reboot_seq, frame->seq, __ATOMIC_RELAXED);
        __atomic_store_n(&control_reboot_session, frame->session, __ATOMIC_RELAXED);
        __atomic_store_n(&control_reboot_generation, generation, __ATOMIC_RELAXED);
        __atomic_store_n(&control_reboot_taken, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&control_reboot_armed, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&control_reboot_requested, 1u, __ATOMIC_RELEASE);
    }
}
/* The worker loop in bsd_init.c is the only thing that services a REBOOT
 * request, and it stops as soon as the poll path moves phase out of ACTIVE.
 * This helper is the fallback for that case: it runs on the receive path, which
 * stays alive. A request still unarmed after the window below cannot be
 * serviced any more, because a live worker handles it on its next 5 ms tick. */
static void control_reboot_fallback(void)
{
    uint64_t frequency = 0, start, limit, settle, settleLimit;
    if (!__atomic_load_n(&control_reboot_requested, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&control_reboot_armed, __ATOMIC_ACQUIRE))
        return;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    if (!frequency) return;
    start = counter();
    limit = frequency / 8u;                  /* 125 ms */
    while (counter() - start < limit) {
        if (__atomic_load_n(&control_reboot_armed, __ATOMIC_ACQUIRE)) return;
        if (!__atomic_load_n(&control_reboot_requested, __ATOMIC_ACQUIRE)) return;
        __asm__ volatile("yield" ::: "memory");
    }
    if (!__atomic_load_n(&control_reboot_requested, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&control_reboot_armed, __ATOMIC_ACQUIRE))
        return;
    /* Let the controller put the acknowledged reply on the wire before the SoC
     * leaves the bus. 1 ms; nothing depends on it surviving. */
    __asm__ volatile("dsb sy" ::: "memory");
    settle = counter();
    settleLimit = frequency / 1000u;
    while (counter() - settle < settleLimit) __asm__ volatile("yield" ::: "memory");
    platform_smc_reset();
}
static void control_service(const uint8_t *bytes, size_t count, uint32_t generation,
                            const struct qcom_usb_debug_status *status,
                            unsigned int pipe)
{
    size_t i;
    if (!__atomic_load_n(&control_enabled, __ATOMIC_ACQUIRE)) return;
    if (generation != usb_control_context.state.generation) {
        usb_control_context.state.generation = generation;
        control_rotate_session_owned();
    }
    for (i = 0; i < count; i++) {
        char line[SXNU_CONTROL_FRAME_BYTES];
        size_t line_bytes = 0;
        int complete = sxnu_control_parser_feed(&usb_control_context.parser, bytes[i], line,
                                                 sizeof(line), &line_bytes);
        if (complete > 0) {
            struct sxnu_control_frame frame;
            if (sxnu_control_parse(line, line_bytes, &frame))
                control_handle_frame(&frame, status, generation, pipe);
            else
                (void)__atomic_add_fetch(&control_rejected, 1u, __ATOMIC_RELAXED);
        } else if (complete < 0) {
            (void)__atomic_add_fetch(&control_rejected, 1u, __ATOMIC_RELAXED);
        }
    }
    /* Serving the request here keeps REBOOT working after the worker loop has
     * stopped; see the note above control_reboot_fallback(). */
    control_reboot_fallback();
}
int qcom_usb_debug_control_start(void)
{
    uint64_t flags;
    unsigned int expected = 0;
    int (*halt_expected)(unsigned int) = 0;
    int installed;
    if (!__atomic_compare_exchange_n(&control_started, &expected, 1u, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return __atomic_load_n(&control_started, __ATOMIC_ACQUIRE) == 2u;
    if (!acquire(&flags)) {
        __atomic_store_n(&control_started, 0u, __ATOMIC_RELEASE);
        return 0;
    }
    if (native.status.phase != QCOM_USB_DEBUG_ACTIVE) {
        release(flags);
        __atomic_store_n(&control_started, 0u, __ATOMIC_RELEASE);
        return 0;
    }
    usb_control_context.seed = counter() ^
                               ((uint64_t)native.status.counter_frequency << 32) ^
                               UINT64_C(0x53584e554354524c);
    usb_control_context.state.generation = dwc3_rx_generation(native.core);
    control_rotate_session_owned();
    release(flags);
    installed = QUD_PSCI_SMC_RESET &&
                __atomic_compare_exchange_n(&PE_halt_restart, &halt_expected,
                  qcom_usb_control_halt_restart, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    __atomic_store_n(&control_hook_owned, installed ? 1u : 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&control_enabled, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&control_started, 2u, __ATOMIC_RELEASE);
    return 1;
}
int qcom_usb_debug_control_take_reboot(void)
{
    unsigned int expected = 0;
    unsigned int claimed;
    if (!__atomic_load_n(&control_reboot_requested, __ATOMIC_ACQUIRE)) return 0;
    if (!__atomic_compare_exchange_n(&control_reboot_taken, &expected, 1u, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
    if (__atomic_load_n(&control_reboot_requested, __ATOMIC_ACQUIRE)) return 1;
    /* A concurrent failure cleared the request after the initial load. Release
     * our now-empty claim so a later acknowledged REBOOT is not left busy. */
    claimed = 1u;
    (void)__atomic_compare_exchange_n(&control_reboot_taken, &claimed, 0u, 0,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return 0;
}
int qcom_usb_debug_control_arm_reboot(void)
{
    uint64_t flags;
    uint32_t generation;
    int armed = 0;
    if (!__atomic_load_n(&control_hook_owned, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&control_reboot_requested, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&control_reboot_taken, __ATOMIC_ACQUIRE) || !acquire(&flags)) return 0;
    generation = __atomic_load_n(&control_reboot_generation, __ATOMIC_ACQUIRE);
    if (native.status.phase == QCOM_USB_DEBUG_ACTIVE && native.core_initialized &&
        dwc3_ready(native.core) && dwc3_rx_generation(native.core) == generation) {
        __atomic_store_n(&control_reboot_armed, 1u, __ATOMIC_RELEASE);
        armed = 1;
    }
    release(flags);
    return armed;
}
void qcom_usb_debug_control_reboot_failed(int result)
{
    struct sxnu_control_frame frame;
    struct sxnu_control_writer writer;
    char reply[SXNU_CONTROL_REPLY_BYTES];
    unsigned int requested;
    requested = __atomic_exchange_n(&control_reboot_requested, 0u, __ATOMIC_ACQ_REL);
    __atomic_store_n(&control_reboot_taken, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&control_reboot_armed, 0u, __ATOMIC_RELEASE);
    if (!requested) return;
    (void)__atomic_add_fetch(&control_failures, 1u, __ATOMIC_RELAXED);
    frame.session = __atomic_load_n(&control_reboot_session, __ATOMIC_RELAXED);
    frame.seq = __atomic_load_n(&control_reboot_seq, __ATOMIC_RELAXED);
    frame.verb = SXNU_CONTROL_VERB_REBOOT;
    frame.crc = 0;
    sxnu_control_writer_init(&writer, reply, sizeof(reply));
    if (sxnu_control_reply_start(&writer, frame.session, frame.seq, frame.verb, "ERR") &&
        sxnu_control_writer_text(&writer, " state=reboot result=") &&
        sxnu_control_writer_signed(&writer, result)) {
        size_t bytes = sxnu_control_reply_finish(&writer);
        if (bytes) (void)control_reply_queue(reply, bytes, 0u);
    }
}
int qcom_usb_debug_wait_drained(unsigned int timeout_ms)
{
    uint64_t start, ticks;
    if (timeout_ms > 10000u) timeout_ms = 10000u;
    if (!timeout_ms || !native.status.counter_frequency) return 0;
    start = counter();
    ticks = native.status.counter_frequency * timeout_ms / 1000u;
    for (;;) {
        uint64_t flags;
        int pending;
        if (!acquire(&flags)) return 0;
        if (native.status.phase != QCOM_USB_DEBUG_ACTIVE || !poll_owned()) {
            release(flags);
            return 0;
        }
        buffer_status_owned();
        pending = native.status.buffer_pending != 0 || native.status.transport_pending != 0;
        release(flags);
        if (!pending) return 1;
        if (counter() - start >= ticks) return 0;
        delay_us(0, 1000);
    }
}
/* kb-17u: the debugger gets its own ring.
 *
 * usb_serial_rx keeps its original meaning (SXNU1 text plus 0xFA-framed
 * runs). kdp_rx carries only the bytes the debugger may consume. Keeping
 * them apart matters: receive_ready() must not go true just because console
 * traffic is flowing -- that is exactly what hung kprintf last time. */
/* 512 B: a worst-case CONNECT frame with every byte escaped is 4x its wire
 * size, and a session needs CONNECT + a few requests queued ahead. */
#define SERIAL_KDP_RX_BYTES 512u
static uint8_t kdp_rx[SERIAL_KDP_RX_BYTES];
static uint32_t kdp_rx_head, kdp_rx_tail;
/* Set as soon as the host stages KDP bytes.
 *
 * The serial keyboard thread polls _serial_getc() every 16 ms and would swallow
 * a staged frame before the debugger ever saw it (measured: kdp_rx_used came
 * back 0 after every KDPPUSH chunk). Once armed, receive_ready() reports the
 * KDP ring only, so the keyboard gets -1 and stops consuming. The keyboard has
 * never been verified working on this board, KDP is the actual goal. */

static int
kdp_rx_push(uint8_t c)
{
    uint64_t daif;
    int stored = 0;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #3" : "=r"(daif) :: "memory");
    if (kdp_rx_used < SERIAL_KDP_RX_BYTES) {
        kdp_rx[kdp_rx_head] = c;
        kdp_rx_head = (kdp_rx_head + 1u) % SERIAL_KDP_RX_BYTES;
        kdp_rx_used++;
        stored = 1;
    } else {
        /* kb-28 (T41-c #1): a full ring used to drop the byte silently while
         * qcom_usb_debug_kdp_push() still returned 1, so KDPPUSH reported
         * "queued=<everything>" and the host had no way to notice the frame was
         * short. Count it and report it. */
        kdp_rx_overflow++;
    }
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
    return stored;
}

/* Forward declaration: the DEBUGGER verb queues bytes before trap entry. */
int qcom_usb_debug_kdp_push(uint8_t c);

static int
kdp_rx_pop(uint8_t *c)
{
    uint64_t daif;
    int ok = 0;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #3" : "=r"(daif) :: "memory");
    if (kdp_rx_used != 0) {
        *c = kdp_rx[kdp_rx_tail];
        kdp_rx_tail = (kdp_rx_tail + 1u) % SERIAL_KDP_RX_BYTES;
        kdp_rx_used--;
        ok = 1;
    }
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
    return ok;
}

/* kb-17u ring state, declared before first use (definitions stay below). */

size_t qcom_usb_debug_write(const void *bytes, size_t count)
{
    uint64_t flags;
    size_t accepted = 0;
    if (!bytes || !count || native.status.phase == QCOM_USB_DEBUG_DISABLED ||
        native.status.phase == QCOM_USB_DEBUG_STOPPED) return 0;
    /* T131: with the second CDC data interface enabled, KDP bytes (serial
     * reply framing or KDPF net frames) go to pipe 1 instead of the console
     * backlog, so the two host handles never carry each other's traffic.
     * Everything outside those gates keeps the historical path verbatim. */
    if (cdc_second_port && (qcom_usb_debug_kdp_tx_active() ||
                            qcom_usb_debug_kdp_net_active())) {
        if (native.status.phase != QCOM_USB_DEBUG_ACTIVE || !acquire(&flags)) return 0;
        if (native.status.phase == QCOM_USB_DEBUG_ACTIVE) {
            if (dwc3_queue_pipe(native.core, 1u, bytes, count) == count) accepted = count;
            (void)poll_owned();
        }
        release(flags);
        return accepted;
    }
    accepted = backlog_write(bytes, count);
    /* Armed == the kernel is inside the debugger.
     *
     * kb-26: this used to bypass poll_owned() and only dwc3_queue() the bytes,
     * on the theory that the controller stops answering once the other cores are
     * halted. That theory was wrong, and the evidence was already on disk:
     * kb-17u (commit 17f1830) has NO armed branch at all -- it polls
     * unconditionally -- and its capture shows the whole debugger entry printing
     * ("Using serial KDP." twice, "Waiting", the four Options lines, and even
     * "Continuing..."). kb-22/24/25 then produced zero debugger output purely
     * because this branch stopped pumping: dwc3_queue() is a memcpy into the TX
     * ring plus a doorbell, and dwc3_poll() is the only thing that hands the ring
     * to the endpoint. Nothing else runs in that context, so the bytes sat there.
     *
     * The earlier "Using s" truncation (kb-17t) was a different bug -- every RX
     * byte was going into usb_serial_rx and wedging kprintf. The kdp_rx split
     * fixed that one; it was never a reason to stop transmitting.
     *
     * So: pump exactly as the unarmed path does, but WITHOUT the 1 ms flush
     * window below. In the debugger the other cores are halted, so the only
     * work the pump may do is the single bounded dwc3_poll(1) inside
     * poll_owned(): no retry loop, no delay_us. If the controller really has
     * stopped, dwc3_poll() returns an error, poll_owned() flips the phase to
     * IO_FAILED, and the next call short-circuits on the phase check above. */
    if (native.status.phase != QCOM_USB_DEBUG_ACTIVE || !acquire(&flags)) return accepted;
    if (native.status.phase == QCOM_USB_DEBUG_ACTIVE) {
        (void)poll_owned();
        /* Explicit diagnostic mode uses a 1ms flush window for a ready host so
         * the last character remains observable if a later kernel path
         * stops servicing USB. One bounded core operation may extend it.
         * No-host and normal mode remain nonblocking.
         *
         * kb-26: never in the debugger. The window spins up to 1 ms calling
         * delay_us() with interrupts already off and the other cores halted;
         * one poll per character is all the entry banner needs, and a stuck
         * controller must not turn every character into a 1 ms stall. */
        if (usb_diag_enabled && !kdp_session_active() &&
            native.status.phase == QCOM_USB_DEBUG_ACTIVE && dwc3_ready(native.core)) {
            uint64_t start = counter();
            unsigned int attempts = 128u;
            while (attempts-- && (dwc3_queued(native.core) || soc_usb_buffer_pending(&usb_log_buffer)) &&
                   counter() - start < native.status.counter_frequency / 1000u) {
                if (!poll_owned()) break;
                delay_us(0, 10);
            }
        }
    }
    release(flags); return accepted;
}
void qcom_usb_debug_putc(char c) { (void)qcom_usb_debug_write(&c, 1); }
/* ---- pe_serial CDC backend: raw serial console / KDP RX --------------------
 * KDP frames are 0xFA + CRC32(4) + 0xFA + escaped payload + 0xFB
 * (osfmk/kdp/kdp_serial.c).  0xFA/0xFB/0xFE never appear unescaped inside a
 * KDP payload, so a raw 0xFA starts a KDP run and a raw 0xFB ends it.  Bytes
 * outside a KDP run are SXNU1 text and still go to the control parser. */
#define SERIAL_KDP_START  0xFAu
#define SERIAL_KDP_END    0xFBu
#define SERIAL_RX_BYTES   4096u

static uint8_t usb_serial_rx[SERIAL_RX_BYTES];
static uint32_t usb_serial_rx_head, usb_serial_rx_tail, usb_serial_rx_used;
static uint32_t usb_serial_rx_overflow;
static volatile uint8_t usb_serial_rx_kdp;


static void
serial_rx_push(uint8_t c)
{
    uint64_t daif;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #3" : "=r"(daif) :: "memory");
    if (usb_serial_rx_used < SERIAL_RX_BYTES) {
        usb_serial_rx[usb_serial_rx_head] = c;
        usb_serial_rx_head = (usb_serial_rx_head + 1u) % SERIAL_RX_BYTES;
        usb_serial_rx_used++;
    } else {
        usb_serial_rx_overflow++;
    }
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
}

static int
serial_rx_pop(uint8_t *c)
{
    uint64_t daif;
    int ok = 0;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #3" : "=r"(daif) :: "memory");
    if (usb_serial_rx_used != 0) {
        *c = usb_serial_rx[usb_serial_rx_tail];
        usb_serial_rx_tail = (usb_serial_rx_tail + 1u) % SERIAL_RX_BYTES;
        usb_serial_rx_used--;
        ok = 1;
    }
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
    return ok;
}

int
qcom_usb_debug_serial_available(void)
{
    return native.status.phase == QCOM_USB_DEBUG_ACTIVE;
}

/* kb-17u: queue one byte for the debugger. kdp_getc() only tests for a
 * non-zero value, so a single character is enough to deliver 'c' without
 * pretending that console traffic is console input.
 *
 * kb-28 (T41-c #1): returns 0 when the byte did NOT make it into the ring.
 * It used to return 1 unconditionally, so a full ring produced a KDPPUSH
 * reply claiming "queued=24" for a chunk that had dropped bytes on the floor.
 * The caller now counts real acceptances, and reports overflow separately. */
int
qcom_usb_debug_kdp_push(uint8_t c)
{
    /* 0x00 is a perfectly ordinary byte in a KDP frame: kdp_hdr_t carries len
     * little-endian, the session key, ip_src, ip_id, and the CONNECT greeting
     * is nul-terminated -- a 71-byte CONNECT frame contains a dozen of them.
     * Rejecting 0x00 here silently corrupted every frame we tried to stage
     * (T38-c). Push unconditionally; the caller decides what to send. */
    return kdp_rx_push(c);
}

unsigned int
qcom_usb_debug_serial_ready(void)
{
    /* v1 never blocks: qcom_usb_debug_write() is non-blocking and absorbs into
     * the 4 MiB CPU backlog.  Reporting "no room" would let uart_putc_device()
     * spin forever if the host stops draining the 16 KiB DWC3 ring. */
    return 1u;
}

/* kb-28 (T41-c): raise/lower the KDP reply transmit gate.
 *
 * Called from kdp_serial_send() in osfmk/kdp/kdp_udp.c, wrapped around
 * kdp_serialize_packet(). While raised, the pe_serial CDC backend's
 * transmit_data() forwards to qcom_usb_debug_putc(); the rest of the time it
 * stays a no-op, which is what keeps kprintf from re-entering the USB write
 * path. See the kdp_tx_mode comment for the full story. */
void
qcom_usb_debug_kdp_tx_mode(int on)
{
    __atomic_store_n(&kdp_tx_mode, on ? 1u : 0u, __ATOMIC_RELEASE);
}

int
qcom_usb_debug_kdp_tx_active(void)
{
    return __atomic_load_n(&kdp_tx_mode, __ATOMIC_ACQUIRE) != 0;
}

size_t
qcom_usb_debug_serial_pending(void)
{
    /* kdp_rx only: console traffic must never make receive_ready() true.
     * While armed, report only the KDP ring so the serial keyboard thread sees
     * "no input" and stops eating staged frames.
     *
     * kb-32: while armed, also pull whatever the controller has buffered.
     *
     * The 5 ms worker that normally calls serial_rx_drain() is blocked on the
     * other side of the trap, so in the debugger the DWC3 RX ring is only
     * serviced if somebody asks for input -- and kdp_serial_receive() polls
     * exactly this predicate. Draining here is what turns "preload everything
     * up front" into an interactive session: the host can send another request
     * after CONNECT and it will be seen.
     *
     * serial_rx_drain() takes usb_busy with a non-blocking exchange, so a
     * concurrent worker or a re-entrant call simply leaves the ring to the
     * next poll; it can never deadlock from here.
     *
     * T46-Q1 (kb-61, kb-62): two attempts to let a panicked kernel read 'r' out
     * of this predicate both failed and were reverted.
     *   kb-61 opened the not-armed path, but serial_rx_drain() needs usb_busy and
     *         a panic usually starts from inside that lock (the panic print goes
     *         through qcom_usb_debug_write), so acquire() fails and the ring is
     *         never read.
     *   kb-62 added a lock-free dwc3_read() instead, and the board went silent at
     *         once -- not even the panic() text came out. Reading the controller
     *         without the worker's state is not safe just because the worker is
     *         stopped; it left the TRB ring inconsistent and killed TX too.
     * Constraint recorded here so nobody repeats it: this predicate must not
     * touch DWC3 outside the worker's lock. A reset exit from a panic needs a
     * change in osfmk/kern/debug.c (reset before kdp_connection_wait), not here. */
    if (!kdp_session_active()) return 0;
    if (kdp_rx_used == 0) serial_rx_drain();
    return kdp_rx_used;
}

uint8_t
qcom_usb_debug_serial_getc(void)
{
    uint8_t c = 0;
    /* While armed only the KDP ring is served, so a keyboard poll can never
     * steal a staged frame. Disarmed consumers see the console ring as before. */
    if (kdp_session_active()) {
        if (kdp_rx_used == 0) serial_rx_drain();   /* kb-32: see serial_pending() */
        if (!kdp_rx_pop(&c)) return 0;
        return c;
    }
    (void)serial_rx_pop(&c);
    return c;
}

/* ---- T80: deterministic KDP session teardown --------------------------------
 *
 * Both mechanisms are opt-in through boot-args. With neither present the code
 * below takes exactly the pre-T80 path, so an unmodified boot-arg set behaves
 * bit-for-bit as before:
 *
 *   nabu-kdp-release-resync=1   release_wire() leaves one 0xFB in kdp_rx
 *   nabu-kdp-death-reset=<sec>  degrade an armed session after <sec> of RX
 *                               silence, or at once on a bus generation change
 *                               (0/absent = off, recommended 60)
 */

static uint32_t
kdp_release_resync_enabled(void)
{
    static uint32_t cached = 0;      /* 0 = unevaluated, 1 = off, 2 = on */
    uint32_t value = 0;
    if (!cached)
        cached = (PE_parse_boot_argn("nabu-kdp-release-resync", &value, sizeof(value)) &&
                  value == 1) ? 2u : 1u;
    return cached - 1u;
}

static uint32_t
kdp_death_reset_seconds(void)
{
    static uint32_t cached = 0;      /* 0 = unevaluated; stored as seconds + 1 */
    uint32_t seconds = 0;
    if (!cached) {
        if (PE_parse_boot_argn("nabu-kdp-death-reset", &seconds, sizeof(seconds)) && seconds) {
            if (seconds > 3600u) seconds = 3600u;
            cached = seconds + 1u;
        } else {
            cached = 1u;
        }
    }
    return cached - 1u;
}

/* Evaluate the channel-death criteria. Called from serial_rx_drain(), where
 * generation and rx_count were just observed under the USB lock. Returns 1
 * only while armed and only when nabu-kdp-death-reset is non-zero. */
static uint32_t
kdp_channel_death_check(uint32_t generation, uint32_t rx_count)
{
    uint32_t seconds;
    if (!kdp_session_active()) return 0;
    seconds = kdp_death_reset_seconds();
    if (!seconds) return 0;
    if (!kdp_gen_valid) {
        kdp_gen_seen = generation;
        kdp_gen_valid = 1;
    } else if (generation != kdp_gen_seen) {
        /* Bus reset / re-enumeration: whichever peer owned this session is gone.
         * A half frame from before the reset cannot be completed by bytes from
         * after it, so the session must not stay armed. */
        (void)__atomic_add_fetch(&kdp_generation_changes, 1u, __ATOMIC_RELAXED);
        return 1;
    }
    if (rx_count != 0) {
        kdp_empty_drains = 0;
        return 0;
    }
    /* No bytes since the previous drain. The 5 ms worker tick is the slowest
     * cadence this counter sees (the debugger poll is ~3 ms), so 200 drains
     * per requested second is a conservative lower bound on elapsed time. */
    if (++kdp_empty_drains >= seconds * 200u) return 1;
    return 0;
}

/* Tear the armed session down: drop residual ring bytes, optionally leave the
 * 0xFB resync for the next entry, then clear the latch. usb_serial_rx_kdp is
 * cleared too -- a half frame left open when the peer vanished would otherwise
 * keep swallowing later console text into usb_serial_rx. */
static void
kdp_channel_degrade(void)
{
    while (kdp_rx_used != 0) {
        uint8_t drop;
        if (!kdp_rx_pop(&drop)) break;
    }
    if (kdp_release_resync_enabled())
        (void)kdp_rx_push((uint8_t)SERIAL_KDP_END);
    usb_serial_rx_kdp = 0;
    __atomic_store_n(&kdp_rx_armed, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&kdp_rx_armed1, 0u, __ATOMIC_RELEASE);
    (void)__atomic_add_fetch(&kdp_death_count, 1u, __ATOMIC_RELAXED);
}

/* nabu (0023): hand the wire back to the SXNU1 console after a host resume.
 *
 * kdp_rx_armed was documented as "only ever set (never cleared), which is fine,
 * because the kernel is in the debugger from then on". That premise died once
 * resume started working: the kernel returns to normal operation with the flag
 * still set, so control_service() is never called again and the console stays
 * dead for the rest of the boot (measured on kdr6v/kdr6w/kdr6x -- HELLO went
 * out with seq 0, which cannot be a sequence rejection, and still got no reply
 * while the kernel was printing megabytes of spawn logs).
 *
 * Clearing the flag is only half of it: the KDP ring may hold the tail of a
 * frame, and kdp_unserialize_packet() may be parked mid-frame. A lone 0xFB is
 * the documented unconditional resync for the latter (see KDPPUSH above: in
 * DS_READING it returns to DS_WAITSTART, in DS_WAITSTART it is ignored).
 *
 * The historical code pushed 0xFB and then drained the whole ring in the same
 * function, so the byte it claimed to inject was thrown away before any
 * consumer could see it -- the resync was dead code and a half frame survived
 * into the next session. T80 keeps the push but does NOT drain it when
 * nabu-kdp-release-resync=1: the next DEBUGGER verb arms the ring again and
 * kdp_getc() reads the 0xFB first, resetting dsState before the fresh CONNECT.
 * Default (boot-arg absent) keeps the old push-and-drain, i.e. no change.
 *
 * Called from debug.c after the debugger has fully unwound. Calling it earlier
 * would feed leftover KDP bytes to control_service() while the parser is still
 * in the debugger's stack frame -- that is the _control_handle_frame+0x593c
 * four-level panic recorded at the control_service() call site below. */
void
nabu_kdp_release_wire(void)
{
    if (!kdp_session_active())
        return;
    if (kdp_release_resync_enabled()) {
        while (kdp_rx_used != 0) {
            uint8_t drop;
            if (!kdp_rx_pop(&drop))
                break;
        }
        (void)kdp_rx_push((uint8_t)SERIAL_KDP_END);
        (void)__atomic_add_fetch(&kdp_release_resyncs, 1u, __ATOMIC_RELAXED);
    } else {
        (void)kdp_rx_push((uint8_t)SERIAL_KDP_END);
        while (kdp_rx_used != 0) {
            uint8_t drop;
            if (!kdp_rx_pop(&drop))
                break;
        }
    }
    __atomic_store_n(&kdp_rx_armed, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&kdp_rx_armed1, 0u, __ATOMIC_RELEASE);
}

/* T143: pipe-1 activation.  Called from serial_rx_drain() once a complete KDP
 * frame (0xFA..0xFB -- the same boundary the console scanner uses) has been
 * pushed into kdp_rx.  This runs on the USB worker thread, exactly the context
 * the DEBUGGER verb already traps from, with no lock held; that is the
 * proven-safe context, and the module links no thread-create API to do better.
 *
 * The latch is self-clearing on return: pipe 1 does not depend on debug.c's
 * release hook, so a host detach cannot leave it latched the way pipe 0
 * historically could (T132 §5 kb164). */
static void
kdp_pipe1_enter(uint32_t generation)
{
    if (&Debugger == 0) return;
    kdp_gen_seen = generation;
    kdp_gen_valid = 1;
    kdp_empty_drains = 0;
    __atomic_store_n(&kdp_rx_armed1, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&kdp_trap_active, 1u, __ATOMIC_RELEASE);
    Debugger("qcom_usb_debug_pipe1");
    __atomic_store_n(&kdp_trap_active, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&kdp_rx_armed1, 0u, __ATOMIC_RELEASE);
    (void)__atomic_add_fetch(&kdp_pipe1_enters, 1u, __ATOMIC_RELAXED);
}

/* T143: scan one drain chunk from pipe 1 for a single complete KDP frame.
 * Framing is the same boundary serial_rx_drain() uses for the console copy:
 * a raw 0xFA opens the run and a raw 0xFB closes it.  Escaping guarantees
 * neither byte appears inside a payload (kdp_serial.c emits 0xFE + ~byte), so
 * no unescaper is needed here -- the bytes are pushed exactly as the wire
 * delivered them, which is what kdp_unserialize_packet() expects. */
static void
kdp_pipe1_feed(const uint8_t *p, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        uint8_t b = p[i];
        if (!kdp_pipe1_in_frame) {
            if (b == SERIAL_KDP_START) {
                kdp_pipe1_in_frame = 1;
                (void)kdp_rx_push(b);
            } else {
                kdp_pipe1_stray++;
            }
            continue;
        }
        (void)kdp_rx_push(b);
        if (b == SERIAL_KDP_END) {
            kdp_pipe1_in_frame = 0;
            kdp_pipe1_arm_pending = 1;
        }
    }
}

/* One bounded drain of the DWC3 RX ring.  Master CPU only, same constraint as
 * dwc3_poll()/dwc3_read().  Shared by the worker and by the pe_serial receive
 * path so KDP does not have to wait for the next 5 ms worker tick. */

/* nabu: optional KDP-over-CDC framing lives in QcomKdpNet.c.  Declared here so
 * neither QcomXnuUsbDebug.h copy has to change.  With the boot-arg absent,
 * route() returns the original pointer/length and active() stays 0. */
void qcom_usb_debug_kdp_net_route(const uint8_t *in, size_t n, uint32_t generation,
                                  const uint8_t **out, size_t *out_n);
int qcom_usb_debug_kdp_net_try_attach(void);
int qcom_usb_debug_kdp_net_active(void);

static void
serial_rx_drain(void)
{
    uint64_t flags;
    uint8_t rx[SXNU_CONTROL_WORK_BYTES];
    uint8_t text[SXNU_CONTROL_WORK_BYTES];
    uint8_t rx1[SXNU_CONTROL_WORK_BYTES];
    struct qcom_usb_debug_status status = {0};
    uint32_t generation = 0;
    size_t rx_count = 0, rx1_count = 0, text_count = 0;
    size_t wire_count = rx_count, wire1_count = 0;
    const uint8_t *wire = rx, *wire1 = rx1;
    int observed = 0;
    int suppress_control = 0;

    if (native.status.phase != QCOM_USB_DEBUG_ACTIVE || !acquire(&flags)) return;
    (void)poll_owned();
    if (native.status.phase == QCOM_USB_DEBUG_ACTIVE) {
        buffer_status_owned();
        bytes_copy(&status, &native.status, sizeof(status));
        generation = dwc3_rx_generation(native.core);
        rx_count = dwc3_read(native.core, rx, sizeof(rx));
        rx1_count = cdc_second_port ? dwc3_read_pipe(native.core, 1u, rx1, sizeof(rx1)) : 0;
        observed = 1;
    }
    release(flags);
    if (!observed) return;

    /* T80: optional channel-death degrade (nabu-kdp-death-reset). Evaluated
     * before any routing, so the chunk that carries the death signal is never
     * handed to control_service(). After the degrade the ring is empty and the
     * latch is clear, so the armed guards below simply take their normal
     * disarmed paths on this and every later chunk. */
    if (kdp_channel_death_check(generation, (uint32_t)rx_count)) {
        kdp_channel_degrade();
        suppress_control = 1;
    }

    /* nabu: net mode extracts KDPF frames from this chunk; with the boot-arg
     * off wire==rx and wire_count==rx_count, so routing below is unchanged. */
    qcom_usb_debug_kdp_net_route(rx, rx_count, generation, &wire, &wire_count);

    for (size_t i = 0; i < wire_count; i++) {
        uint8_t b = wire[i];
        if (usb_serial_rx_kdp) {
            serial_rx_push(b);
            if (b == SERIAL_KDP_END) usb_serial_rx_kdp = 0;
            continue;
        }
        if (b == SERIAL_KDP_START) {
            usb_serial_rx_kdp = 1;
            serial_rx_push(b);
            continue;
        }
        text[text_count++] = b;
    }

    /* kb-32: while the kernel sits in the debugger, feed the KDP ring too.
     *
     * Until now a staged frame was the ONLY way in: qcom_usb_debug_serial_pending()
     * reports kdp_rx only, and this drain routed wire bytes to usb_serial_rx, so
     * anything the host sent after CONNECT landed in a ring the debugger never
     * reads. That is why every round had to preload its whole request list and
     * could never ask a second question interactively.
     *
     * Re-parse the same bytes into kdp_rx while armed. The console ring keeps its
     * copy, so nothing that used to work stops working; the KDP unserialiser is
     * stateful and ignores everything outside a 0xFA..0xFB run anyway, so
     * leftover text is harmless.
     *
     * Guarded by kdp_rx_armed, which the DEBUGGER verb raises immediately before
     * the trap -- so normal operation, including the boot path, never sees this. */
    if (rx_count != 0 && !qcom_usb_debug_kdp_net_active() &&
        __atomic_load_n(&kdp_rx_armed, __ATOMIC_ACQUIRE)) {
        for (size_t i = 0; i < rx_count; i++) {
            (void)kdp_rx_push(rx[i]);
        }
    }

    /* T175: answer a HELLO that arrives on the log/KDP pipe (role=1) without
     * touching pipe-0 parser/session state.  Skipped while a KDP session owns
     * either pipe, so a live session never sees a stray console line. */
    if (rx1_count != 0 && cdc_second_port && !qcom_usb_debug_kdp_net_active() &&
        !__atomic_load_n(&kdp_rx_armed1, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&kdp_rx_armed, __ATOMIC_ACQUIRE))
        control_pipe1_hello_scan(rx1, rx1_count);

    /* T131: pipe 1 is the dedicated KDP data interface.  Bytes the host sends
     * there never touch the SXNU1 parser or the console ring.  Net mode feeds
     * the same KDPF scanner as the shared stream; serial-KDP mode pushes into
     * kdp_rx exactly like the re-parse above, but only while armed so a stale
     * request cannot be waiting when the debugger next enters. */
    if (rx1_count != 0 && cdc_second_port) {
        if (qcom_usb_debug_kdp_net_active()) {
            (void)qcom_usb_debug_kdp_net_route(rx1, rx1_count, generation, &wire1, &wire1_count);
        } else if (__atomic_load_n(&kdp_rx_armed1, __ATOMIC_ACQUIRE) ||
                   __atomic_load_n(&kdp_rx_armed, __ATOMIC_ACQUIRE)) {
            /* A live KDP session on either pipe consumes pipe 1 verbatim.
             * Mode 2 makes one exception: a stale pipe-0 latch (armed but no
             * Debugger() on the stack, e.g. the aftermath of a host detach)
             * must not block pipe 1, so those bytes go to the detector. */
            if (kdp_pipe1_mode &&
                !__atomic_load_n(&kdp_rx_armed1, __ATOMIC_ACQUIRE) &&
                __atomic_load_n(&kdp_rx_armed, __ATOMIC_ACQUIRE) &&
                !__atomic_load_n(&kdp_trap_active, __ATOMIC_ACQUIRE)) {
                kdp_pipe1_feed(rx1, rx1_count);
            } else {
                for (size_t i = 0; i < rx1_count; i++) {
                    (void)kdp_rx_push(rx1[i]);
                }
            }
        } else if (kdp_pipe1_mode) {
            /* T131 kept pipe 1 receive-only-while-armed; T143 lets an idle
             * pipe 1 recognise one complete KDP frame and activate by itself.
             * Bytes outside a frame are still dropped (S5): they must not
             * accumulate in kdp_rx for a session that never comes. */
            kdp_pipe1_feed(rx1, rx1_count);
        }
    }
    if (kdp_pipe1_arm_pending) {
        kdp_pipe1_arm_pending = 0;
        if (!__atomic_load_n(&kdp_trap_active, __ATOMIC_ACQUIRE))
            kdp_pipe1_enter(generation);
    }

    if (text_count != 0 && __atomic_load_n(&control_enabled, __ATOMIC_ACQUIRE)) {
        /* While the debugger owns the wire, the host sends bare control
         * characters ('c' to continue, 'r' to reset) and KDP frames -- none of
         * that is an SXNU1 line. Feeding it to control_service() put a lone 'c'
         * into the parser and the round died in _control_handle_frame+0x593c
         * with four nested panics right after `Options.....    Type`.
         *
         * The console ring above keeps its copy of every byte, so nothing that
         * used to work stops working; the control parser simply must not see
         * traffic that belongs to the debugger. kdp_rx_armed is cleared by
         * nabu_kdp_release_wire() on a clean resume and by the T80
         * channel-death degrade above, so the suppression ends there and not
         * (as originally assumed) at the end of the boot. suppress_control
         * covers exactly the chunk that triggered a degrade. */
        if (!__atomic_load_n(&kdp_rx_armed, __ATOMIC_ACQUIRE) && !suppress_control)
            control_service(text, text_count, generation, &status, 0u);
    }
}

void
qcom_usb_debug_serial_poll(void)
{
    serial_rx_drain();
}
void qcom_usb_debug_poll(void)
{
    serial_rx_drain();
    /* nabu: idempotent, boot-arg gated, waits for phase ACTIVE. */
    (void)qcom_usb_debug_kdp_net_try_attach();
    qud_dump_ufs_progress();   /* async UFS dump, outside the USB lock */
    /* T142: periodic trace sample; no-op unless nabu-trace=1 and TRACEON
     * has armed a window. */
    qud_trace_tick(counter(), native.status.counter_frequency, &native.status);
}
void qcom_usb_debug_panic_prepare(void)
{
    /* An early panic can stall in platform/debugger handling before the final
     * spin hook. Diagnose PREPARING before entering those paths. */
    if (usb_diag_enabled && (native.status.phase == QCOM_USB_DEBUG_PREPARING ||
        (native.status.phase == QCOM_USB_DEBUG_NOT_INITIALIZED && usb_diag_checkpoint)))
        diag_reset(100u + 2u * usb_diag_checkpoint);
}
void qcom_usb_debug_panic_drain(void)
{
    static const char end[] = "\nQCOM PANIC END\n";
    uint64_t flags, now;
    unsigned int iterations = 200000u, marker_queued = 0, announced = 0;
    char announcement[192];
    size_t announcement_bytes;
    if (usb_diag_enabled && (native.status.phase == QCOM_USB_DEBUG_PREPARING ||
        (native.status.phase == QCOM_USB_DEBUG_NOT_INITIALIZED && usb_diag_checkpoint)))
        diag_reset(100u + 2u * usb_diag_checkpoint);
    /* panic_spin_forever can precede CPU-data initialization. Inactive USB
     * must not call cpu_number(), touch MMIO or acquire a guard in that case. */
    if (native.status.phase != QCOM_USB_DEBUG_ACTIVE || !acquire(&flags)) return;
    if (native.status.phase != QCOM_USB_DEBUG_ACTIVE) goto out;
    {
        char *p = append_text(announcement, "\nQCOM USB PANIC pending_at_start=");
        p = append_decimal(p, soc_usb_buffer_pending(&usb_log_buffer) + dwc3_queued(native.core));
        p = append_text(p, " dropped_at_start=");
        p = append_decimal(p, soc_usb_buffer_dropped(&usb_log_buffer) + dwc3_dropped(native.core));
        p = append_text(p, " buffer_capacity=");
        p = append_decimal(p, sizeof(usb_log_bytes));
        *p++ = '\n';
        announcement_bytes = (size_t)(p - announcement);
    }
    now = counter();
    native.deadline = now + native.status.counter_frequency * 2u;
    if (native.deadline < now) goto out;
    native.deadline_expired = 0; native.deadline_active = 1;
    while (iterations-- && !deadline_expired()) {
        if (!announced && DWC3_TX_BYTES - dwc3_queued(native.core) >= announcement_bytes)
            announced = dwc3_queue(native.core, announcement, announcement_bytes) == announcement_bytes;
        /* End marker follows all queued bytes, and appears only after there
         * is room for the entire marker. Never partially queue it. */
        if (announced && !marker_queued && dwc3_queued(native.core) == 0 &&
            soc_usb_buffer_pending(&usb_log_buffer) == 0) {
            char text[160];
            size_t bytes = loss_record(text, 1);
            (void)dwc3_queue(native.core, text, bytes);
            marker_queued = dwc3_queue(native.core, end, sizeof(end) - 1) == sizeof(end) - 1;
        }
        if (!poll_owned()) break;
        if (marker_queued && dwc3_queued(native.core) == 0 &&
            soc_usb_buffer_pending(&usb_log_buffer) == 0) break;
        delay_us(0, 10);
    }
    native.deadline_active = 0;
    if (native.deadline_expired) {
        native.status.error = QCOM_USB_DEBUG_DEADLINE_EXPIRED;
        if (native.status.io_failed) native.status.phase = QCOM_USB_DEBUG_IO_FAILED;
    }
out:
    buffer_status_owned();
    release(flags);
    if (usb_diag_enabled) diag_reset(10);
}
int qcom_usb_debug_stop(void)
{
    uint64_t flags;
    int ret = 0;
    if (native.status.phase != QCOM_USB_DEBUG_ACTIVE && native.status.phase != QCOM_USB_DEBUG_IO_FAILED &&
        !native.lease.active && !native.dma_lease.active && !native.bypass_lease.active &&
        !native.status.vbus_owned && !native.status.phy_owned) return 0;
    if (native.status.stop_failed) return 0;
    if (!acquire(&flags)) return 0;
    if (native.status.phase != QCOM_USB_DEBUG_ACTIVE && native.status.phase != QCOM_USB_DEBUG_IO_FAILED &&
        !native.lease.active && !native.dma_lease.active && !native.bypass_lease.active &&
        !native.status.vbus_owned && !native.status.phy_owned)
        goto out;
    native.deadline_active = 0;
    if ((native.core_initialized && dwc3_stop(native.core)) ||
        (!native.core_initialized && !halt_inherited())) {
        native.status.stop_failed = 1;
        fail(QCOM_USB_DEBUG_HALT_TIMEOUT, 0); goto out;
    }
    if (!restore_vbus() || !restore_identity() || !restore_bypass() || !restore_lease() || !restore_phy()) { fail(QCOM_USB_DEBUG_RESTORE_FAILED, 0); goto out; }
    native.status.phase = QCOM_USB_DEBUG_STOPPED; ret = 1;
out:
    release(flags); return ret;
}
void qcom_usb_debug_get_status(struct qcom_usb_debug_status *status)
{
    uint64_t flags;
    if (!status || !acquire(&flags)) return;
    buffer_status_owned();
    bytes_copy(status, &native.status, sizeof(*status)); release(flags);
}
