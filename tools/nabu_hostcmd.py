"""Host-side pseudo-commands for nabu_mux.

The console offers a handful of commands that are not device verbs at all: the
host issues one or more READs and decodes the exported arrays itself. FRAMESTAT,
GPUSTATE, SUBMIT and IBCLASS are all in that category. nabu_mux.py only forwards
verb lines, so those names went to the device and were rejected -- which reads as
"the command stopped working" when in fact it was never a verb.

Keeping one copy: this module holds the decode logic and nothing else. It takes a
`read_words(addr, n)` callable rather than owning a serial port, so it can be
driven by the mux, by a test, or by a replay from -com.bin. nabu_console.py keeps
its own inline versions; they are the reference and were used to validate the
tables below.

Layout notes are from the kernel side:
  native_adreno_frame_hist[26]   NativeAdrenoSubmit.cpp:253
  native_adreno_gpu_busy[8]      NativeAdrenoService.cpp / Publish / Scheduler
  native_adreno_a6xx_stats[24]   NativeAdrenoService.cpp:120
"""

FRAME_EDGES = ["<5ms", "<10ms", "<20ms", "<30ms", "<40ms", "<60ms",
               "<80ms", "<100ms", "<150ms", "<200ms", "<300ms", ">=300ms"]

# native_adreno_gpu_busy indices, from the increments in the driver.
GPU_BUSY_NAMES = [
    "gpuInterrupt",      # A640State::gpuInterrupt entries
    "carriedFence",      # of those, carried NativeAdrenoIrqFence
    "publishKick",       # publish path kicks
    "completionWorker",  # completionWorker entries
    "workerAdvance",     # advance() driven by the worker
    "clientAdvance",     # advance() driven by anything else
    "pollTick",          # poll() ticks
    "reserved",
]


def decode_frame_hist(w):
    """Turn the 26-word frame histogram into text lines.

    h[0] highest non-empty bucket, h[1] count, h[2] min, h[3] max,
    h[4..15] the twelve buckets, h[16] issued, h[22] overlap, h[23] last latency,
    h[24] last GPU-clock delta.
    """
    out = []
    if not w or len(w) < 26:
        return ["! framestat: short read (%d words)" % (len(w) if w else 0)]
    total = w[1]
    out.append("FRAMESTAT n=%d issued=%d min=%dus max=%dus last=%dus"
               % (total, w[16], w[2], w[3], w[23]))
    if not total:
        out.append("FRAMESTAT no completions since boot")
    else:
        parts = []
        for i, name in enumerate(FRAME_EDGES):
            c = w[4 + i]
            if c:
                parts.append("%s=%d(%.1f%%)" % (name, c, 100.0 * c / total))
        out.append("FRAMESTAT hist " + " ".join(parts))
    out.append("FRAMESTAT overlap=%d last_gpu=%dus" % (w[22], w[24]))
    return out


def decode_gpu_busy(w):
    """Decode native_adreno_gpu_busy[8] (64-bit counters)."""
    out = []
    if not w or len(w) < 8:
        return ["! gpubusy: short read"]
    for i, name in enumerate(GPU_BUSY_NAMES):
        out.append("GPUBUSY %-17s %d" % (name, w[i]))
    irq, fence = w[0], w[1]
    if irq:
        out.append("GPUBUSY fence hit rate = %d/%d (%.3f%%)"
                   % (fence, irq, 100.0 * fence / irq))
    if w[6] != irq:
        out.append("GPUBUSY note: gpuInterrupt(%d) != pollTick(%d) -- they used "
                   "to be equal, which proved gpuInterrupt was poll-driven"
                   % (irq, w[6]))
    return out


def decode_a6xx_stats(w):
    """Decode native_adreno_a6xx_stats[24]; only the fields with known meaning."""
    if not w or len(w) < 24:
        return ["! a6xx: short read"]
    return [
        "A6XX start_phase=%d fence=%d completedFence=%d" % (w[0], w[8], w[9]),
        "A6XX irq_status=0x%08x cp_status=0x%08x protect=0x%08x"
        % (w[11], w[12], w[13]),
        "A6XX zap phase=%d transport=%d result=%d stopped=%d gmuStop=%d"
        % (w[16], w[17], w[18], w[20], w[21]),
    ]


# Addresses are resolved from the kernel image by the caller, because they move
# between builds. These are the symbol names, not values.
SYMBOLS = {
    "frame_hist": "_native_adreno_frame_hist",
    "gpu_busy": "_native_adreno_gpu_busy",
    "a6xx_stats": "_native_adreno_a6xx_stats",
}

# Word counts to read for each.
COUNTS = {"frame_hist": 26, "gpu_busy": 16, "a6xx_stats": 24}
