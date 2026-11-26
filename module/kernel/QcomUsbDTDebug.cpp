/* SPDX-License-Identifier: MIT
 * T140 DEVTREE: read-only exploration of the IOKit device-tree plane.
 *
 * This unit deliberately contains no device-tree parsing of its own. It calls
 * the same public IOKit APIs the in-tree drivers use -- IORegistryEntry::
 * fromPath() on gIODTPlane plus getProperty()/getName()/getChildIterator() --
 * and copies node names and a few well-known properties into a plain text
 * buffer for the USB control reply. Anything it cannot represent is skipped,
 * never fatal.
 *
 * The verb is gated by nabu-verbs-forensics=1; this code only runs when the
 * control handler accepts the request.
 */
#include <stddef.h>
#include <stdint.h>

/* Two in-tree headers disagree about printf's return type under C++
 * (libkern/libkern.h says int, pexpert/protos.h says void). Load the former
 * first, then rename the latter's spelling for this translation unit so the
 * two declarations cannot collide. Nothing here calls printf. */
#include <libkern/libkern.h>
#define printf qdt_printf_unused

#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IORegistryEntry.h>
/* T150: IOLog carries the entry/exit markers that say whether a walk ever
 * started and whether it finished. */
#include <IOKit/IOLib.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSPtr.h>
#include <libkern/c++/OSSymbol.h>

struct qdt_writer {
    char *out;
    size_t cap;
    size_t used;
    int failed;
};

static int
qdt_put(struct qdt_writer *w, const char *text, size_t bytes)
{
    size_t i;
    if (w->failed || !text) return 0;
    if (bytes > w->cap - w->used) { w->failed = 1; return 0; }
    for (i = 0; i < bytes; i++) w->out[w->used + i] = text[i];
    w->used += bytes;
    if (w->used < w->cap) w->out[w->used] = 0;
    return 1;
}

static int
qdt_puts(struct qdt_writer *w, const char *text)
{
    size_t n = 0;
    if (!text) return 0;
    while (text[n]) n++;
    return qdt_put(w, text, n);
}

/* Append one property value. OSData is emitted verbatim when it is printable
 * ASCII (compatible/device_type/model are strings on this tree); OSSymbol
 * through its C string. Other types are skipped. */
static int
qdt_put_value(struct qdt_writer *w, OSObject *value)
{
    const OSData *data = OSDynamicCast(OSData, value);
    const OSSymbol *symbol = OSDynamicCast(OSSymbol, value);
    const char *text;
    size_t n, i;

    if (data && data->getBytesNoCopy() && data->getLength()) {
        text = (const char *)data->getBytesNoCopy();
        n = data->getLength();
        for (i = 0; i < n; i++)
            if (text[i] < 0x20 || text[i] > 0x7e) return 1; /* binary: skip */
        return qdt_put(w, text, n);
    }
    if (symbol && symbol->getCStringNoCopy())
        return qdt_puts(w, symbol->getCStringNoCopy());
    return 1;
}

static int
qdt_put_entry(struct qdt_writer *w, IORegistryEntry *entry)
{
    const char *name = entry->getName();
    if (!qdt_puts(w, " name=")) return 0;
    if (name && !qdt_puts(w, name)) return 0;
    if (!qdt_puts(w, " compatible=")) return 0;
    if (!qdt_put_value(w, entry->getProperty(gIODTCompatibleKey))) return 0;
    if (!qdt_puts(w, " device_type=")) return 0;
    if (!qdt_put_value(w, entry->getProperty(gIODTTypeKey))) return 0;
    if (!qdt_puts(w, " model=")) return 0;
    if (!qdt_put_value(w, entry->getProperty(gIODTModelKey))) return 0;
    return qdt_put(w, "\n", 1u);
}

/* T150: the walk itself is unchanged; native_dt_explore() below only adds a
 * logging wrapper so a board log can distinguish "never entered" from
 * "entered and never returned". */
static int
qdt_explore_impl(const char *path, char *out, size_t cap, size_t *used,
                 const char **why, uint32_t max_entries, int *truncated)
{
    struct qdt_writer w;
    IORegistryEntry *root;
    auto iterator = (OSIterator *)0;
    unsigned entries = 0;
    int more = 0;

    if (used) *used = 0;
    if (truncated) *truncated = 0;
    if (why) *why = "";
    if (!path || !out || !cap || !max_entries) { if (why) *why = "args"; return 0; }

    w.out = out; w.cap = cap; w.used = 0; w.failed = 0;
    out[0] = 0;

    root = IORegistryEntry::fromPath(path, gIODTPlane);
    if (!root) { if (why) *why = "notfound"; return 0; }
    if (!qdt_puts(&w, "path=") || !qdt_puts(&w, path) ||
        !qdt_puts(&w, " nodes=0\n")) {
        if (why) *why = "nomem";
        root->release();        /* T150: fromPath retains; balance it. */
        return 0;
    }

    /* Without IOKIT_ENABLE_SHARED_PTR, getChildIterator() vends a raw
     * OSIterator* that this call owns and must release. */
    iterator = root->getChildIterator(gIODTPlane);
    while (iterator && entries < max_entries) {
        OSObject *object = iterator->getNextObject();
        IORegistryEntry *child;
        if (!object) break;
        child = OSDynamicCast(IORegistryEntry, object);
        if (!child) continue;
        if (!qdt_put_entry(&w, child)) break;
        entries++;
    }
    if (iterator && entries >= max_entries && iterator->getNextObject())
        more = 1;
    if (iterator) iterator->release();
    if (w.failed) { if (why) *why = "truncated"; root->release(); return 0; }
    if (truncated) *truncated = more;
    if (used) *used = w.used;
    root->release();            /* T150: balance fromPath's retain. */
    return 1;
}

extern "C" int
native_dt_explore(const char *path, char *out, size_t cap, size_t *used,
                  const char **why, uint32_t max_entries, int *truncated)
{
    int ok;

    IOLog("nabu dt enter path=%s max=%u\n", path ? path : "(null)",
          (unsigned)max_entries);
    ok = qdt_explore_impl(path, out, cap, used, why, max_entries, truncated);
    IOLog("nabu dt exit path=%s rc=%d used=%u why=%s trunc=%d\n",
          path ? path : "(null)", ok, (unsigned)(used ? *used : 0),
          (why && *why) ? *why : "", truncated ? *truncated : 0);
    return ok;
}
