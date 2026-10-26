/* SPDX-License-Identifier: MIT
 * Host model for the bypass-bank mapping: geometry, bank selection, the exact
 * write set, the callback allow-list and restore.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "soc/QcomUsbBypass.h"

struct cell { QUSB_U64 address; QUSB_U32 value; };
struct model {
  struct cell cells[64];
  unsigned int count, writes;
};

static struct cell *find(struct model *m, QUSB_U64 address, int create)
{
  unsigned int i;
  for (i = 0; i < m->count; ++i) if (m->cells[i].address == address) return &m->cells[i];
  if (!create) return NULL;
  assert(m->count < 64);
  m->cells[m->count].address = address;
  m->cells[m->count].value = 0;
  return &m->cells[m->count++];
}
static void set(struct model *m, QUSB_U64 address, QUSB_U32 value)
{
  find(m, address, 1)->value = value;
}
static QUSB_U32 get(struct model *m, QUSB_U64 address)
{
  return find(m, address, 1)->value;
}
static int read32(void *cookie, QUSB_U64 address, QUSB_U32 *value)
{
  *value = get(cookie, address);
  return 1;
}
static int write32(void *cookie, QUSB_U64 address, QUSB_U32 value)
{
  struct model *m = cookie;
  ++m->writes;
  set(m, address, value);
  return 1;
}
static void barrier(void *cookie) { (void)cookie; }

static QUSB_U64 cb(unsigned int bank) { return QUSB_SMMU_BASE + (QUSB_U64)(4u + bank) * 4096u; }
static QUSB_U64 cbar(unsigned int bank) { return QUSB_SMMU_BASE + 0x1000u + 4u * bank; }
static QUSB_U64 cba2r(unsigned int bank) { return QUSB_SMMU_BASE + 0x1800u + 4u * bank; }
static QUSB_U64 s2cr(unsigned int slot) { return QUSB_SMMU_BASE + 0xc00u + 4u * slot; }
static QUSB_U64 smr(unsigned int slot) { return QUSB_SMMU_BASE + 0x800u + 4u * slot; }

/* Four 4K groups, four banks: our stream on bank 3 (already the ready bypass
 * bank), UFS on bank 1, bank 2 free but unconfigured. */
static void init(struct model *m)
{
  memset(m, 0, sizeof(*m));
  set(m, QUSB_SMMU_BASE + 0x20u, (1u << 30) | (1u << 27) | 4u);
  set(m, QUSB_SMMU_BASE + 0x24u, (1u << 28) | 4u);
  set(m, QUSB_SMMU_BASE + 0x28u, (1u << 12) | (2u << 4) | 2u);
  set(m, QUSB_SMMU_BASE + 0x00u, 0u);
  set(m, smr(0), 0x80000000u | QUSB_STREAM_ID);
  set(m, s2cr(0), 3u);
  set(m, smr(1), 0x80000000u | 0x200u);
  set(m, s2cr(1), 1u);
  set(m, cb(1), 1u);              /* UFS bank is translating */
  set(m, cb(3), 0u);
  set(m, cbar(3), 0x10000u);
  set(m, cba2r(3), 1u);
}

int main(void)
{
  struct model m;
  QUSB_BYPASS_LEASE l;

  /* Already the configured bypass bank: plan succeeds without any write. */
  init(&m);
  assert(QcomUsbBypassPlan(read32, &m, 0xc7f4c000u, 32768u, &l) == QusbQualified);
  assert(l.slot == 0u && l.bank == 3u && l.changed == 0u && l.active == 0u);
  assert(QcomUsbBypassEnter(read32, write32, barrier, &m, &l) == QusbQualified);
  assert(m.writes == 0);

  /* A stale CBAR on the chosen bank is the only write, and restore puts it back. */
  init(&m);
  set(&m, cbar(3), 0x20000u);
  assert(QcomUsbBypassPlan(read32, &m, 0xc7f4c000u, 32768u, &l) == QusbQualified);
  assert(l.bank == 3u && l.changed == 2u && l.active == 1u);
  assert(QcomUsbBypassWriteAllowed(&l, cbar(3), 0x10000u));
  assert(!QcomUsbBypassWriteAllowed(&l, cbar(3), 0x20000u));
  assert(!QcomUsbBypassWriteAllowed(&l, s2cr(0), 3u));
  assert(QcomUsbBypassEnter(read32, write32, barrier, &m, &l) == QusbQualified);
  assert(get(&m, cbar(3)) == 0x10000u);
  assert(l.readback == 3u);                       /* S2CR untouched */
  l.phase = 2u;
  assert(QcomUsbBypassWriteAllowed(&l, cbar(3), 0x20000u));
  assert(QcomUsbBypassRestore(read32, write32, barrier, &m, &l) == QusbQualified);
  assert(get(&m, cbar(3)) == 0x20000u && !l.active);

  /* Bank 3 taken by another stream: move to bank 2, configure it, restore. */
  init(&m);
  set(&m, smr(2), 0x80000000u | 0x300u);
  set(&m, s2cr(2), 3u);
  assert(QcomUsbBypassPlan(read32, &m, 0xc7f4c000u, 32768u, &l) == QusbQualified);
  assert(l.bank == 2u && (l.changed & 8u) && l.slot == 0u);
  assert(QcomUsbBypassEnter(read32, write32, barrier, &m, &l) == QusbQualified);
  assert(get(&m, s2cr(0)) == 2u);
  assert(get(&m, cb(2)) == 0u && get(&m, cbar(2)) == 0x10000u && get(&m, cba2r(2)) == 1u);
  l.phase = 2u;
  assert(QcomUsbBypassRestore(read32, write32, barrier, &m, &l) == QusbQualified);
  assert(get(&m, s2cr(0)) == 3u && get(&m, cbar(2)) == 0u && get(&m, cba2r(2)) == 0u);

  /* Every bank held: refuse without writing. */
  init(&m);
  set(&m, smr(2), 0x80000000u | 0x300u); set(&m, s2cr(2), 3u);
  set(&m, smr(3), 0x80000000u | 0x400u); set(&m, s2cr(3), 2u);
  set(&m, cb(0), 1u);
  assert(QcomUsbBypassPlan(read32, &m, 0xc7f4c000u, 32768u, &l) == QusbNoFreeStream);
  assert(m.writes == 0);

  /* Ambiguous or shared matches and missing entries are refused. */
  init(&m);
  set(&m, smr(2), 0x80000000u | QUSB_STREAM_ID);
  assert(QcomUsbBypassPlan(read32, &m, 0xc7f4c000u, 32768u, &l) == QusbMultipleStreams);
  init(&m);
  set(&m, smr(0), 0x80000000u | (0xffu << 16) | QUSB_STREAM_ID);
  assert(QcomUsbBypassPlan(read32, &m, 0xc7f4c000u, 32768u, &l) == QusbSharedStream);
  init(&m);
  set(&m, smr(0), 0u);
  assert(QcomUsbBypassPlan(read32, &m, 0xc7f4c000u, 32768u, &l) == QusbNoExplicitStream);

  /* The DMA range must fit the SMMU's declared widths. */
  init(&m);
  assert(QcomUsbBypassPlan(read32, &m, 1ULL << 40, 32768u, &l) == QusbUnknownSmmu);

  /* CLIENTPD set: the non-secure SMMU already bypasses, so nothing to change. */
  init(&m);
  set(&m, QUSB_SMMU_BASE + 0x00u, 1u);
  assert(QcomUsbBypassPlan(read32, &m, 0xc7f4c000u, 32768u, &l) == QusbQualified);
  assert(l.active == 0u && l.changed == 0u);

  puts("QcomUsbBypass: plan, bank choice, allow-list, commit and restore modelled");
  return 0;
}
