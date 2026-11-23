/*
 * Copyright (c) 2005-2006 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * @OSF_COPYRIGHT@
 */
/*
 * @APPLE_FREE_COPYRIGHT@
 */

#include <kern/spl.h>
#include <mach/std_types.h>
#include <types.h>
#include <kern/thread.h>
#include <console/serial_protos.h>
#include <libkern/section_keywords.h>

extern void cons_cinput(char ch);               /* The BSD routine that gets characters */

SECURITY_READ_ONLY_LATE(unsigned int) serialmode;                               /* Serial mode keyboard and console control */

/*
 *  This routine will start a thread that polls the serial port, listening for
 *  characters that have been typed.
 */

void
serial_keyboard_init(void)
{
	kern_return_t   result;
	thread_t                thread;

	if (!(serialmode & SERIALMODE_INPUT)) { /* Leave if we do not want a serial console */
		return;
	}

	kprintf("Serial keyboard started\n");
	result = kernel_thread_start_priority((thread_continue_t)serial_keyboard_start, NULL, MAXPRI_KERNEL, &thread);
	if (result != KERN_SUCCESS) {
		panic("serial_keyboard_init");
	}

	thread_deallocate(thread);
}

void
serial_keyboard_start(void)
{
	/* Go see if there are any characters pending now */
	serial_keyboard_poll();
}

__dead2
void
serial_keyboard_poll(void)
{
	int chr;
	uint64_t next;

	while (1) {
		chr = _serial_getc(false); /* Get a character if there is one */
		if (chr < 0) { /* The serial buffer is empty */
			break;
		}
		cons_cinput((char)chr); /* Buffer up the character */
	}

	clock_interval_to_deadline(16, 1000000, &next); /* Get time of pop */

	assert_wait_deadline((event_t)serial_keyboard_poll, THREAD_UNINT, next); /* Show we are "waiting" */
	thread_block((thread_continue_t)serial_keyboard_poll); /* Wait for it */
	panic("serial_keyboard_poll: Shouldn't never ever get here...");
}

boolean_t
console_is_serial(void)
{
	return cons_ops_index == SERIAL_CONS_OPS;
}

int
switch_to_video_console(void)
{
	int old_cons_ops = cons_ops_index;
	cons_ops_index = VC_CONS_OPS;
	return old_cons_ops;
}

int
switch_to_serial_console(void)
{
	extern bool serial_console_enabled;
	int old_cons_ops = cons_ops_index;

	if (serial_console_enabled) {
		cons_ops_index = SERIAL_CONS_OPS;
	}

	return old_cons_ops;
}

/* The switch_to_{video,serial,kgdb}_console functions return a cookie that
 *  can be used to restore the console to whatever it was before, in the
 *  same way that splwhatever() and splx() work.  */
void
switch_to_old_console(int old_console)
{
	static boolean_t squawked;
	uint32_t ops = old_console;

	if ((ops >= nconsops) && !squawked) {
		squawked = TRUE;
		printf("switch_to_old_console: unknown ops %d\n", ops);
	} else {
		cons_ops_index = ops;
	}
}

void
console_printbuf_state_init(struct console_printbuf_state * data, int write_on_newline, int can_block)
{
	if (data == NULL) {
		return;
	}
	bzero(data, sizeof(struct console_printbuf_state));
	if (write_on_newline) {
		data->flags |= CONS_PB_WRITE_NEWLINE;
	}
	if (can_block) {
		data->flags |= CONS_PB_CANBLOCK;
	}
}

/* nabu: panic log capture */
void nabu_log_putc(int ch);

void
console_printbuf_putc(int ch, void * arg)
{
	nabu_log_putc(ch);
	struct console_printbuf_state * info = (struct console_printbuf_state *)arg;
	info->total += 1;
	if (info->pos < (SERIAL_CONS_BUF_SIZE - 1)) {
		info->str[info->pos] = (char)ch;
		info->pos += 1;
	} else {
		/*
		 * when len(line) > SERIAL_CONS_BUF_SIZE, we truncate the message
		 * if boot-arg 'drain_uart_sync=1' is set, then
		 * drain all the buffer right now and append new ch
		 */
		/* USB logging must not inherit newline-free printbuf truncation. */
#if !XNU_QCOM_USB_DEBUG
		if (serialmode & SERIALMODE_SYNCDRAIN)
#endif
		{
			info->str[info->pos] = '\0';
			console_write(info->str, info->pos);
			info->pos            = 0;
			info->str[info->pos] = (char)ch;
			info->pos += 1;
		}
	}

	info->str[info->pos] = '\0';
	/* if newline, then try output to console */
	if (ch == '\n' && info->flags & CONS_PB_WRITE_NEWLINE) {
		console_write(info->str, info->pos);
		info->pos            = 0;
		info->str[info->pos] = '\0';
	}
}

void
console_printbuf_clear(struct console_printbuf_state * info)
{
	if (info->pos != 0) {
		console_write(info->str, info->pos);
	}
	info->pos = 0;
	info->str[info->pos] = '\0';
	info->total = 0;
}

/* nabu-log-version: 42 */
/* ---- nabu: panic log capture -------------------------------------------------
 * Every character that reaches the console is mirrored into a ring. The ring tail
 * is written to the diagnostic window at the end of the userdata partition
 * (native_ufs_log_*): once at driver start, every 2 s from the log thread, and
 * from the UFS read path during the APFS mount -- the mount can crash inside one
 * 2 s interval, and a window that stops at the last explicit write cannot say
 * where it stopped.
 *
 * Header block layout (little endian):
 *   0  8  "NABULOG1"
 *   8  4  version (1)
 *  12  4  flags (1 = written from the panic path)
 *  16  4  bytes that follow the header
 *  20  4  bytes dropped after the ring wrapped
 *  24  8  total bytes ever captured
 */
#define NABU_LOG_RING_BYTES  (4u * 1024u * 1024u)
#define NABU_LOG_RING_MASK   (NABU_LOG_RING_BYTES - 1u)
#define NABU_LOG_CHUNK_BYTES 65536u

static char nabu_log_ring[NABU_LOG_RING_BYTES];
static char nabu_log_bounce[NABU_LOG_CHUNK_BYTES];
static char nabu_log_block[4096];
static unsigned int nabu_log_head;
volatile unsigned long long nabu_log_total;
static unsigned int nabu_log_dropped;
static int nabu_log_opened;
/* l469: how much of the ring has actually reached the medium. The writers below fire far more
 * often than the ring gains bytes, and each firing rewrote the whole tail - measured at 68.5% of
 * every command this boot issues, all of them writes. */
static unsigned long long nabu_log_flushed;
static unsigned long long nabu_log_serviced;
static int nabu_log_servicing;

/* Provided by the storage driver: restarts the watchdog from output volume, so the
 * reset follows evidence instead of wall time. Weak: silent when no driver is present. */
extern void nabu_log_service_4k(void) __attribute__((weak));

extern void nabu_mark_text(const char *s, unsigned int y);
extern int native_ufs_log_open(uint64_t *first_lba, uint64_t *blocks, const char **why);
extern int native_ufs_log_write(uint64_t rel, uint32_t blocks, const void *src);
extern int native_ufs_log_sync(void);

void
nabu_log_putc(int ch)
{
	nabu_log_ring[nabu_log_head] = (char)ch;
	nabu_log_head = (nabu_log_head + 1u) & NABU_LOG_RING_MASK;
	if (nabu_log_total < NABU_LOG_RING_BYTES) {
		nabu_log_total++;
	} else {
		nabu_log_dropped++;
	}
	if (nabu_log_service_4k != 0 && !nabu_log_servicing) {
		unsigned long long produced = nabu_log_total + (unsigned long long)nabu_log_dropped;
		if (produced - nabu_log_serviced >= 4096ull) {
			nabu_log_serviced = produced;
			nabu_log_servicing = 1;
			nabu_log_service_4k();
			nabu_log_servicing = 0;
		}
	}
}

void
nabu_log_puts(const char *s)
{
	if (s == 0) {
		return;
	}
	while (*s) {
		nabu_log_putc(*s++);
	}
}

void
nabu_log_hex(const char *label, unsigned long long value)
{
	char digits[16];
	unsigned i;
	const char *p = label;
	if (p) {
		while (*p) {
			nabu_log_putc(*p++);
		}
	}
	for (i = 0; i < 16; ++i) {
		unsigned nibble = (unsigned)((value >> ((15 - i) * 4)) & 0xf);
		digits[i] = (char)(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
	}
	for (i = 0; i < 16; ++i) {
		nabu_log_putc(digits[i]);
	}
	nabu_log_putc('\n');
}

static void
nabu_log_write32(char *at, unsigned int value){
	at[0] = (char)(value & 0xffu);
	at[1] = (char)((value >> 8) & 0xffu);
	at[2] = (char)((value >> 16) & 0xffu);
	at[3] = (char)((value >> 24) & 0xffu);
}

static unsigned int
nabu_log_read32(const char *at)
{
	return (unsigned int)(unsigned char)at[0] |
	    ((unsigned int)(unsigned char)at[1] << 8) |
	    ((unsigned int)(unsigned char)at[2] << 16) |
	    ((unsigned int)(unsigned char)at[3] << 24);
}

static int
nabu_log_fail(const char *why)
{
	nabu_mark_text(why, 2340);
	/* Push whatever already landed (at minimum the header block) to the medium,
	 * so the host can tell an open failure from a data-write failure. */
	native_ufs_log_sync();
	return 0;
}

/* Single-block tail write without sync: the only form that survives the driver
 * bring-up window (WATCHDOG.md measures 64 KiB and any sync as fatal there).
 * Writes the NABULOG1 header plus the last 4 KiB of the ring. is_panic marks a
 * tail landed from the panic path, so a boot that died does not have to be told
 * apart from one that was interrupted. */
int nabu_log_flush_tail_nosync(int is_panic);

/* Called from the panic path (osfmk/kern/debug.c) before the reset spin so
 * the text nabu_panic_note() just wrote reaches the medium. A separate name
 * keeps the call site readable and gives the panic path a stable symbol. */
int
nabu_log_land_now(void)
{
	return nabu_log_flush_tail_nosync(1);
}

/* Two landing slots, far apart so a normal boot cannot erase a panic tail.
 * Periodic landings own the start of the window (rel 0 = header, 1.. = tail);
 * panic landings own the end (rel 8190 = header, tail below it). kb-56 proved
 * the old single-slot layout lost the panic header on the very next boot. */
#define NABU_LOG_PANIC_SLOT       (8190u)   /* header block for a panic landing */
#define NABU_LOG_PANIC_TAIL_LAST  (8189u)   /* highest block a panic tail may use */
#define NABU_LOG_PANIC_TAIL_MAX   (24u)     /* keep the tail well below the header */

int
nabu_log_flush_tail_nosync(int is_panic)
{
	uint64_t first = 0, window = 0;
	unsigned int nabu_log_slot;
	unsigned int nabu_log_slot_first;
	const char *why = "";
	unsigned long long available;
	unsigned int offset, i, blocks, b;

	if (!nabu_log_opened) {
		/* native_ufs_log_open() returns 0 for every refusal, and the board
		 * opens this window itself during early boot, so the normal answer
		 * here is "already open" -- which is success, not failure. Treating
		 * it as fatal left nabu_log_opened permanently 0 and blocked every
		 * landing (kb-53: total=262144, flushed=0, opened=0). The window is
		 * live either way: native_ufs_log_write() gates on the same flag. */
		if (!native_ufs_log_open(&first, &window, &why)) {
			const char *why_text = (const char *)why;
			int already = 0;
			if (why_text != 0) {
				/* "already open" -- compare without strcmp (no libk here). */
				static const char word[] = "already open";
				unsigned i;
				for (i = 0; word[i] != 0; ++i) {
					if (why_text[i] != word[i]) {
						break;
					}
				}
				already = (word[i] == 0 && why_text[i] == 0) ? 1 : 0;
			}
			if (!already) {
				nabu_log_fail(why);
				return 0;
			}
		}
		nabu_log_opened = 1;
	}
	/* l469: nothing has been produced since the last landing, so the bytes on the medium are
	 * already exactly the bytes we would write here. Say "done" and spend no command. A panic
	 * tail never takes this shortcut - it is the one write that must reach the medium even when
	 * it would be a duplicate. */
	if (!is_panic && nabu_log_total == nabu_log_flushed) {
		return 1;
	}
	/* Tail blocks to land. One block is the historical form that survives the
	 * driver bring-up window; more blocks are separate single-block commands so
	 * each one keeps that property, and a failure stops the sequence. */
	blocks = 24;
	PE_parse_boot_argn("nabu-paniclog-blocks", &blocks, sizeof(blocks));
	if (blocks < 1) {
		blocks = 1;
	}
	if (blocks > 64) {
		blocks = 64;
	}
	if (is_panic && blocks > NABU_LOG_PANIC_TAIL_MAX) {
		blocks = NABU_LOG_PANIC_TAIL_MAX;
	}
	/* Periodic landings grow upward from 1; panic landings grow downward from
	 * NABU_LOG_PANIC_TAIL_LAST, so both stay inside the window and apart. */
	if (is_panic) {
		nabu_log_slot = NABU_LOG_PANIC_SLOT;
		nabu_log_slot_first = NABU_LOG_PANIC_TAIL_LAST - (blocks - 1u);
	} else {
		nabu_log_slot = 0u;
		nabu_log_slot_first = 1u;
	}
	available = (unsigned long long)blocks * 4096ULL;
	if (nabu_log_total < available) {
		available = nabu_log_total;
	}
	for (i = 0; i < sizeof(nabu_log_block); ++i) {
		nabu_log_block[i] = 0;
	}
	nabu_log_block[0] = 'N'; nabu_log_block[1] = 'A'; nabu_log_block[2] = 'B'; nabu_log_block[3] = 'U';
	nabu_log_block[4] = 'L'; nabu_log_block[5] = 'O'; nabu_log_block[6] = 'G'; nabu_log_block[7] = '1';
	nabu_log_write32(nabu_log_block + 8, 1u);
	nabu_log_write32(nabu_log_block + 12, is_panic ? 1u : 0u);
	nabu_log_write32(nabu_log_block + 16, (unsigned int)available);
	nabu_log_write32(nabu_log_block + 20, nabu_log_dropped);
	nabu_log_write32(nabu_log_block + 24, (unsigned int)(nabu_log_total & 0xffffffffu));
	nabu_log_write32(nabu_log_block + 28, (unsigned int)(nabu_log_total >> 32));
	if (!native_ufs_log_write(nabu_log_slot, 1, nabu_log_block)) {
		return nabu_log_fail("nabu:tail-header");
	}
	/* Block 0 is the newest ring tail (the historical single block); block b is the
	 * b-th 4 KiB going back in time, so the on-disk order is header + newest..oldest. */
	for (b = 0; b < blocks; ++b) {
		unsigned int chunk = 4096;
		if ((unsigned long long)b * 4096ULL >= available) {
			break;
		}
		if (available - (unsigned long long)b * 4096ULL < 4096ULL) {
			chunk = (unsigned int)(available - (unsigned long long)b * 4096ULL);
		}
		offset = (unsigned int)((nabu_log_head - (unsigned long long)(b + 1) * 4096ULL) & NABU_LOG_RING_MASK);
		for (i = 0; i < chunk; ++i) {
			nabu_log_bounce[i] = nabu_log_ring[(offset + i) & NABU_LOG_RING_MASK];
		}
		for (i = chunk; i < 4096; ++i) {
			nabu_log_bounce[i] = 0;
		}
		if (!native_ufs_log_write(nabu_log_slot_first + b, 1, nabu_log_bounce)) {
			return nabu_log_fail("nabu:tail-data");
		}
	}
	nabu_log_flushed = nabu_log_total;
	return 1;
}

/* Substring test without libc: only the plain byte compare is needed here. */
static int
nabu_line_matches(const char *line, unsigned len)
{
	static const char *const needles[] = {
		"panic", "Panic", "PANIC", "iabort", "Attempt", "assert", "registry", "caller"
	};
	unsigned n;

	for (n = 0; n < sizeof(needles) / sizeof(needles[0]); ++n) {
		const char *nd = needles[n];
		unsigned nl = 0, i, k;
		while (nd[nl]) {
			nl++;
		}
		if (nl == 0 || nl > len) {
			continue;
		}
		for (i = 0; i + nl <= len; ++i) {
			for (k = 0; k < nl; ++k) {
				if (line[i + k] != nd[k]) {
					break;
				}
			}
			if (k == nl) {
				return 1;
			}
		}
	}
	return 0;
}

/* Print the interesting ring lines through the console. panic() has already proven
 * that printf works in this context; nothing here takes a lock or touches UFS.
 * Scanning 64 KiB costs nothing, and printing only matching lines keeps the first
 * panic message on screen instead of scrolling it away. */
void
nabu_log_print_matches(unsigned bytes)
{
	char line[256];
	unsigned available, offset, i, len = 0, printed = 0;

	if (bytes > NABU_LOG_RING_BYTES) {
		bytes = NABU_LOG_RING_BYTES;
	}
	available = nabu_log_total > bytes ? bytes : (unsigned int)nabu_log_total;
	offset = (unsigned int)((nabu_log_head + NABU_LOG_RING_BYTES - available) & NABU_LOG_RING_MASK);
	printf("\n---- nabu ring matches (%u of %llu bytes) ----\n", available, nabu_log_total);
	for (i = 0; i < available; ++i) {
		char c = nabu_log_ring[(offset + i) & NABU_LOG_RING_MASK];
		if (c == '\r') {
			continue;
		}
		if (c == '\n' || len == sizeof(line) - 1) {
			line[len] = 0;
			if (len && nabu_line_matches(line, len)) {
				printf("%s\n", line);
				printed++;
			}
			len = 0;
			continue;
		}
		line[len++] = c;
	}
	if (len) {
		line[len] = 0;
		if (nabu_line_matches(line, len)) {
			printf("%s\n", line);
			printed++;
		}
	}
	printf("---- end nabu ring matches (%u) ----\n", printed);
}

/* Dump the exception state of a kernel instruction fetch abort before panic()
 * prints anything. The fault is an indirect branch to an impossible address with
 * lr = 0, so the argument registers and the target register say more than the pc:
 * x8 usually holds the callee of a blr, x0..x7 hold its arguments. Ring only:
 * UFS is off limits here, and the panic hook copies this to the DRAM window. */
void
nabu_log_iabort(const unsigned long long *regs, unsigned long long sp,
    unsigned long long pc, unsigned long long lr, unsigned long long far)
{
	char label[16];
	int i;

	/* Every line carries the "iabort" prefix on purpose: the panic hook prints only
	 * ring lines that match its needle list, and a bare "x00=" line would be dropped. */
	nabu_log_puts("iabort regs\n");
	for (i = 0; i < 31; ++i) {
		const char *p = "iabort x";
		int n = 0;
		while (p[n]) {
			label[n] = p[n];
			n++;
		}
		label[n++] = (char)('0' + (i / 10));
		label[n++] = (char)('0' + (i % 10));
		label[n++] = '=';
		label[n] = 0;
		nabu_log_hex(label, regs[i]);
	}
	nabu_log_hex("iabort sp=", sp);
	nabu_log_hex("iabort pc=", pc);
	nabu_log_hex("iabort lr=", lr);
	nabu_log_hex("iabort far=", far);
}

/* Rewrite the window from a healthy kernel thread. panic() must not touch UFS
 * unless the host lock is known free; the periodic copy is at most one interval
 * stale and is what keeps a hung boot readable. */
void
nabu_log_periodic(void)
{
	unsigned nabuLogEnabled = 0;
	PE_parse_boot_argn("nabu-log", &nabuLogEnabled, sizeof(nabuLogEnabled));
	if (!nabuLogEnabled) {
		return;
	}
	/* Do not gate on nabu_log_opened here: that flag is set inside
	 * nabu_log_flush_tail_nosync() itself, so checking it first made this the one
	 * caller that could never open the window. flush_tail_nosync() opens on demand
	 * and is idempotent; the nabu-log gate above is the only thing that should
	 * decide whether the periodic landing runs. */
	(void)nabu_log_flush_tail_nosync(0);
}

unsigned long long
nabu_psci_reset(void){
	register uint64_t x0 __asm__("x0") = 0x84000009ULL;
	register uint64_t x1 __asm__("x1") = 0;
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;
	__asm__ volatile ("smc #0" : "+r" (x0) : "r" (x1), "r" (x2), "r" (x3) : "memory");
	return x0;
}

/* ---- CDC pull channel ---------------------------------------------------
 * The debug transport already has one bounded pull: a provider is registered
 * with qcom_usb_debug_register_screenshot(), SCREENSHOT fills the capture
 * buffer, and SHOTNEXT streams it 192 bytes at a time from a cursor that the
 * host can resume after a disconnect. That provider contract - bounded, no
 * allocation, no USB calls, no logging - is exactly what makes it safe to point
 * at this ring, because once the medium is poisoned or the flush path is stuck
 * the newest bytes exist nowhere else. Nothing below touches storage.
 *
 * One capture = state header, then the newest ring bytes in ascending order, so
 * the last byte the host reads is the newest byte the kernel ever produced.
 * Bytes after that are zero, which keeps the transport's own crc32 over the
 * whole buffer reproducible once the host has undone RLE (the transport picks
 * RLE by size on its own; the padding is what makes the choice deterministic).
 * Boot arg nabu-pull-bytes sizes the tail window; default 8192.
 */
#define NABU_PULL_HEADER_BYTES 512u
#define NABU_PULL_WIDTH 1024u
#define NABU_PULL_MAX_HEIGHT 4096u
#define NABU_PULL_DEFAULT_BYTES 8192u

extern int qcom_usb_debug_register_screenshot(int (*)(void *, uint8_t *, size_t,
    uint32_t *, uint32_t *), void *) __attribute__((weak));
/* The ring counters are static to this file, so they travel in as arguments: the host
 * needs ring_head plus produced (= total + dropped, which keeps counting after total
 * freezes at the 4 MiB cap) to prove the tail it received ends at the current write
 * pointer rather than at some earlier flush. */
extern unsigned native_ufs_state_block(char *dst, unsigned capacity,
    unsigned long long ring_total, unsigned long long ring_dropped,
    unsigned long long ring_head) __attribute__((weak));
/* The Metal discovery counters (name_reads / askers) live in the Adreno publish unit, which is
 * not in every kernel. The dump is weak so the link still closes without it, and it rides this
 * already-throttled snapshot path instead of adding a second schedule or a new counter. */
extern void native_adreno_metal_dump_stats(void) __attribute__((weak));

static unsigned nabu_pull_window;
static unsigned nabu_pull_fb = 0;	/* l498: 0 = off; N = sample every Nth row/column */
static int nabu_pull_fb_tried = 0;

/* Newest `capacity` bytes of the ring, oldest first. Same backward walk the
 * panic matcher uses; reads only - no lock, no head update, no flush. */
static unsigned
nabu_log_copy_tail(char *dst, unsigned capacity)
{
	unsigned available;
	unsigned offset;
	unsigned i;

	if (!dst || !capacity) {
		return 0;
	}
	available = nabu_log_total < (unsigned long long)capacity ?
	    (unsigned)nabu_log_total : capacity;
	if (!available) {
		return 0;
	}
	offset = (unsigned)((nabu_log_head + NABU_LOG_RING_BYTES - available) & NABU_LOG_RING_MASK);
	for (i = 0; i < available; ++i) {
		dst[i] = nabu_log_ring[(offset + i) & NABU_LOG_RING_MASK];
	}
	return available;
}

static int
nabu_pull_capture(void *cookie, uint8_t *rgb, size_t capacity, uint32_t *width,
    uint32_t *height)
{
	unsigned header = 0;
	unsigned body;
	unsigned total;
	unsigned want;
	unsigned rows;
	unsigned limit;
	unsigned i;

	(void)cookie;
	if (!rgb || !width || !height) {
		return 0;
	}
	/* l498: the same bounded provider can carry pixels. The transport already sizes the
	 * image from width*height*3, RLE-compresses a flat picture to almost nothing, and the
	 * host reads back a distinct width/height per mode - so no protocol change and no
	 * second slot (this one is claimed with __atomic_compare_exchange and stays ours).
	 * Off unless nabu-pull-fb=<step>: step is rows/columns to skip, so 8 gives
	 * 200x320 = 192 KB of samples (cheap on the USB worker) and 1 gives the whole
	 * 1600x2560x3 = 12.3 MB frame, which the earlier rounds showed is what wedges the
	 * channel. The source is the boot framebuffer the legacy backend already verified
	 * frame by frame (NativeLegacyDisplay.cpp:513 compares PE_current_console's base with
	 * bootMapping's physical address before it lets a swap through). */
	if (!nabu_pull_fb_tried) {
		nabu_pull_fb_tried = 1;
		PE_parse_boot_argn("nabu-pull-fb", &nabu_pull_fb, sizeof(nabu_pull_fb));
	}
	if (nabu_pull_fb >= 1u && nabu_pull_fb <= 64u) {
		extern volatile uint64_t native_legacy_gop[6];
		uint64_t va = native_legacy_gop[1];
		uint64_t fw = native_legacy_gop[3];
		uint64_t fh = native_legacy_gop[4];
		uint64_t stride = native_legacy_gop[5];
		if (va && fw && fh && stride >= fw * 4u) {
			uint32_t ow = (uint32_t)((fw + nabu_pull_fb - 1u) / nabu_pull_fb);
			uint32_t oh = (uint32_t)((fh + nabu_pull_fb - 1u) / nabu_pull_fb);
			if (ow && oh && (uint64_t) ow * oh * 3u <= capacity) {
				const volatile uint8_t *base = (const volatile uint8_t *) (uintptr_t) va;
				uint8_t *dst = rgb;
				uint32_t y, x;
				for (y = 0; y < oh; ++y) {
					const volatile uint8_t *row = base + (size_t) (y * nabu_pull_fb) * stride;
					for (x = 0; x < ow; ++x) {
						const volatile uint8_t *px = row + (size_t) (x * nabu_pull_fb) * 4u;
						*dst++ = px[2];	/* BGRA -> R */
						*dst++ = px[1];	/* G */
						*dst++ = px[0];	/* B */
					}
				}
				*width = ow;
				*height = oh;
				return 1;
			}
		}
	}
	if (capacity < NABU_PULL_HEADER_BYTES + NABU_PULL_WIDTH * 3u) {
		return 0;
	}
	if (!nabu_pull_window) {
		PE_parse_boot_argn("nabu-pull-bytes", &nabu_pull_window, sizeof(nabu_pull_window));
		if (!nabu_pull_window) {
			nabu_pull_window = NABU_PULL_DEFAULT_BYTES;
		}
	}
	/* The transport sizes the image in RGB triples and derives bytes from
	 * width*height*3, so anything not a whole triple would be silently dropped. */
	/* The transport takes the payload size from width*height*3 and then scans that many
	 * bytes twice (run_length() for the RLE decision, again per chunk while streaming),
	 * all of it on the USB worker thread. Sizing the image to the *buffer* therefore made
	 * every capture cost ~12 MiB of memset plus two full scans - which is why SHOTNEXT
	 * never answered while the console was busy. Size it to the payload instead: whole
	 * pixels only, rounded up so no ring byte is dropped. */
	limit = (unsigned)(capacity - capacity % 3u);
	if (native_ufs_state_block != 0) {
		header = native_ufs_state_block((char *)rgb, NABU_PULL_HEADER_BYTES,
		    nabu_log_total, (unsigned long long)nabu_log_dropped,
		    (unsigned long long)nabu_log_head);
	}
	/* Truncate, never drop. The old form set header = 0, which silently removed the whole
	 * self-identifying banner: the host would then read ring text where it expected
	 * `NABUSTATE1 pull=vN ...` and conclude the provider was an older build. A banner that
	 * overflows is still evidence; a banner that vanishes is a false negative. */
	if (header > NABU_PULL_HEADER_BYTES) {
		header = NABU_PULL_HEADER_BYTES;
	}
	if (header >= limit) {
		header = 0;
	}
	want = nabu_pull_window;
	if (want > limit - header) {
		want = limit - header;
	}
	body = nabu_log_copy_tail((char *)rgb + header, want);
	total = header + body;
	/* Rows are chosen so that width*height*3 covers `total` with the smallest possible
	 * overshoot, and only that overshoot is zeroed. */
	rows = (total + (NABU_PULL_WIDTH * 3u) - 1u) / (NABU_PULL_WIDTH * 3u);
	if (!rows) {
		rows = 1;
	}
	if (rows > NABU_PULL_MAX_HEIGHT) {
		rows = NABU_PULL_MAX_HEIGHT;
	}
	limit = rows * NABU_PULL_WIDTH * 3u;
	if (total > limit) {
		total = limit;	/* window bigger than rows allow: drop the oldest bytes, keep newest */
	}
	for (i = total; i < limit; ++i) {
		rgb[i] = 0;
	}
	*width = NABU_PULL_WIDTH;
	*height = rows;
	return 1;
}

/* l484: the same banner the pull channel returns, written into the ring on a schedule, so a round
 * that loses the late CDC window still leaves a time series behind. It reuses native_ufs_state_block
 * verbatim - one format, one parser - and the caller throttles it to >= 10 s. Never called from the
 * print path or an I/O completion path: it runs on the driver's own writer thread. */
void
nabu_log_state_snapshot (void)
{
	char line[1536];   /* l493: v11 banner measured ~544 chars worst case; the snapshot
	                  * is dropped silently if native_ufs_state_block is handed less */
	unsigned n = 0;

	if (native_ufs_state_block != 0) {
		n = native_ufs_state_block (line, (unsigned) sizeof (line),
		    nabu_log_total, (unsigned long long) nabu_log_dropped,
		    (unsigned long long) nabu_log_head);
	}
	/* The Metal discovery counters ride the same gated path: this thread is already
	 * throttled, so the dump cannot flood the ring on its own. */
	if (native_adreno_metal_dump_stats != 0) {
		native_adreno_metal_dump_stats ();
	}
	if (n == 0 || n >= (unsigned) sizeof (line)) {
		return;
	}
	nabu_log_puts (line);
}

/* Called once from the UFS driver after DWC3 bring-up. The slot is a
 * single-instance compare-exchange, so failing here means somebody else (a
 * display driver) owns it - never that the transport is broken. */
int
nabu_log_attach_pull(void)
{
	if (qcom_usb_debug_register_screenshot == 0) {
		return -1;
	}
	return qcom_usb_debug_register_screenshot(nabu_pull_capture, 0) ? 1 : -2;
}

/* ---- end nabu panic log ---------------------------------------------------- */
