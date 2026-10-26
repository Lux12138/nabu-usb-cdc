# Vendored module: QcomXnuUsbDebug

- Upstream: https://github.com/xiaoleGun/QcomXnuUsbDebug
- Base commit: `83be5782ad7227d7d245862ce1ae3272cae14242` (2026-10-26,
  "usbdebug: Add Qualcomm XNU USB debug transport")
- License of the copied files: MIT (SPDX headers in each source file).

`module/` is the upstream tree with the changes below applied. The full diff
against the base commit is `docs/module-changes.patch`.

## Changed files

| File | Change |
|---|---|
| `soc/Sm8150.h` | new SM8150 profile (GCC 0x100000+0x100000, DWC3, SMMU, SID 0x140, 10 USB clocks, PHY bases, `QUD_DMA_PREFER_IDENTITY=1`, `QUD_DMA_BYPASS_BANK=1`) |
| `soc/QcomUsbPrepare.c/.h` | new prepare stages: GDSC, clocks, resets, ID, femto PHY, DWC3 device mode; `prepare_write` callback policy |
| `soc/QcomUsbBypass.c/.h` | new bypass-bank planner/enter/restore (S2CR = bank, TYPE=TRANSLATE) |
| `include/QcomUsbSoc.h` | `QUD_DMA_BYPASS_BANK` switch and defaults |
| `include/QcomXnuUsbDebug.h` | prepare fields, `dma_mode=18` (bypass bank), status fields reused for diagnostics |
| `kernel/QcomXnuUsbDebug.c` | prepare/qualify/bypass wiring, `physical_write` policy for three leases, restore paths, diagnostic fields |
| `soc/QcomUsbDmaDomain.c` | minor fixes found during bring-up |
| `soc/QcomUsbQualify.c/.h` | identity-lease readback + diagnostics |
| `scripts/build.py` | add `QcomUsbPrepare.c`/`QcomUsbBypass.c` to the unit list; allow the identity audit only when the selected profile defines `QUD_DMA_PREFER_IDENTITY 1` |
| `tests/*` | new prepare/bypass tests and adjusted fixtures |

## Rebuilding the diff

```sh
git clone https://github.com/xiaoleGun/QcomXnuUsbDebug
cd QcomXnuUsbDebug && git checkout 83be578
# copy the files from module/ over the checkout, then:
git add -N <new files>
git diff > module-changes.patch
```
