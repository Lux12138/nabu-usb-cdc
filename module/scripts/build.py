#!/usr/bin/env python3
"""Build the standalone archive with the selected XNU's real compile record.

Never patches/builds/boots the consumer kernel. Writes only a new output dir.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
UNITS = ('kernel/QcomXnuUsbDebug.c', 'kernel/QcomKdpNet.c',
         'kernel/QcomUsbScreenshot.c', 'usb/Dwc3.c', 'soc/QcomUsbQualify.c',
         'soc/QcomUsbPrepare.c', 'soc/QcomUsbDmaDomain.c', 'soc/QcomUsbBypass.c',
         # T140 DEVTREE provider: the IOKit device-tree walk needs C++.
         'kernel/QcomUsbDTDebug.cpp')


def compile_flags(record, cpp=False):
    args = record['arguments']
    if not isinstance(args, list) or not args or not all(isinstance(a, str) for a in args):
        raise ValueError('record must contain an arguments array')
    cwd = Path(record['directory']).resolve()
    source = Path(record['file'])
    source = (source if source.is_absolute() else cwd / source).resolve()
    if not cwd.is_dir() or not source.is_file():
        raise ValueError('reference directory/source unavailable')
    # Strip source, dependency and output operands; never execute a shell.
    consumes = {'-o', '-MF', '-MT', '-MQ', '-MJ', '-dependency-file',
                '--serialize-diagnostics', '-serialize-diagnostics'}
    discard = {'-c', '-fsyntax-only', '-M', '-MM', '-MD', '-MMD', '-MP', '-MG',
               '-save-temps', '-save-temps=obj'}
    flags = [args[0]]
    i = 1
    while i < len(args):
        a = args[i]
        if a in consumes:
            if i + 1 >= len(args):
                raise ValueError('truncated output/dependency option')
            i += 2
            continue
        if a in discard or a == record['file'] or a == str(source):
            i += 1
            continue
        if not a.startswith('-') and (cwd / a).resolve() == source:
            i += 1
            continue
        if any(a.startswith(p) and len(a) > len(p) for p in ('-o', '-MF', '-MT', '-MQ', '-MJ')):
            i += 1
            continue
        flags.append(a)
        i += 1
    if cpp:
        # The reference record forces C (-xc); a .cpp unit compiles as C++ with
        # the same standard the kernel's own C++ units use (-xc++ -std=gnu++2b).
        if '-xc' in flags:
            k = flags.index('-xc')
            flags[k:k + 1] = ['-xc++', '-std=gnu++2b']
        else:
            flags = ['-xc++', '-std=gnu++2b'] + flags
        # A .cpp unit that includes <libkern/libkern.h> reaches sys/cdefs.h, which
        # includes <sys/_symbol_aliasing.h>.  That header lives in the fakeroot's
        # usr/include, and the reference record only lists usr/local/include, so
        # derive usr/include from whatever fakeroot the record points at instead
        # of hard-coding a path.
        want = '.cache/fakeroot/usr/local/include'
        for i, f in enumerate(flags):
            if want not in f:
                continue
            root = f.split(want, 1)[0]
            inc = root + '.cache/fakeroot/usr/include'
            if inc in flags:
                break
            # Headers arrive as the pair '-I', '<path>', so the new directory has
            # to be inserted as a pair too: inserting the path alone leaves the
            # original '-I' bound to it and turns the original path into a bare
            # input file ("cannot specify -o when generating multiple output
            # files").
            if i > 0 and flags[i - 1] == '-I':
                flags[i - 1:i + 1] = ['-I', inc, '-I', f]
            else:
                flags.insert(i, inc)
            break
    return flags, cwd


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compile-record', type=Path, required=True,
                   help='pe_kprintf.o.json for selected ARM64 XNU')
    p.add_argument('--soc', default='soc/Sm8550.h', help='module-relative profile')
    p.add_argument('--output', type=Path, required=True, help='new directory under tmp/ or .cache/')
    p.add_argument('--ar', help='llvm-ar executable')
    p.add_argument('--nm', help='llvm-nm executable for import/export audit')
    a = p.parse_args()
    profile = (ROOT / a.soc).resolve()
    if ROOT not in profile.parents or not profile.is_file():
        p.error('SoC profile must exist within the module')
    out = a.output.resolve()
    if not any(part in ('tmp', '.cache') for part in out.parts) or out == ROOT or ROOT in out.parents:
        p.error('output must be outside source and under tmp/ or .cache/')
    record_bytes = a.compile_record.read_bytes()
    record = json.loads(record_bytes.decode().rstrip().rstrip(','))
    flags, cwd = compile_flags(record)
    if not any('arm64' in f or 'aarch64' in f for f in flags):
        p.error('require ARM64 reference flags')
    ar = a.ar or shutil.which('llvm-ar') or str(Path(flags[0]).parent / 'llvm-ar')
    nm = a.nm or shutil.which('llvm-nm') or str(Path(ar).with_name('llvm-nm'))
    # Never overwrite an archive/evidence directory from a previous run.
    out.mkdir(parents=True, exist_ok=False)
    defines = ['-I' + str(ROOT), '-I' + str(ROOT / 'kernel'), '-I' + str(ROOT / 'include'),
               '-DQUD_SOC_HEADER="' + profile.relative_to(ROOT).as_posix() + '"',
               '-Wno-reserved-identifier', '-Wno-pre-c11-compat']
    report = {'hardware_verified': False, 'kernel_linked': False,
              'compile_record_sha256': hashlib.sha256(record_bytes).hexdigest(),
              'soc': a.soc, 'commands': [], 'source_sha256': {}}
    inputs = [p for p in ROOT.rglob('*') if p.is_file() and p.suffix in ('.c', '.h')]
    before = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    objects = []
    try:
        for unit in UNITS:
            obj = out / (Path(unit).stem + '.o')
            # T140: a .cpp unit must be compiled as C++ (-xc++ -std=gnu++2b) and
            # needs the fakeroot's usr/include for sys/_symbol_aliasing.h; both
            # live in compile_flags() behind its cpp argument, so pass it here or
            # the .cpp is silently compiled as C ("unknown type name 'class'").
            unit_flags, _ = compile_flags(record, cpp=unit.endswith('.cpp'))
            command = unit_flags + defines + ['-Wno-switch-default', '-c', str(ROOT / unit), '-o', str(obj)]
            report['commands'].append(command)
            with (out / (obj.stem + '.log')).open('wb') as log:
                subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
            objects.append(str(obj))
        for name, digest in before.items():
            if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
                raise RuntimeError('source changed during build: ' + name)
        command = [ar, 'rcs', str(out / 'libQcomXnuUsbDebug.a'), *objects]
        subprocess.run(command, check=True)
        report['commands'].append(command)
        archive = out / 'libQcomXnuUsbDebug.a'
        def symbols(mode):
            lines = subprocess.check_output([nm, mode, '--extern-only', str(archive)], text=True).splitlines()
            return {line.split()[-1] for line in lines if line.strip() and not line.endswith(':')}
        defined = symbols('--defined-only')
        external = symbols('--undefined-only') - defined
        allowed = {'_PE_halt_restart', '_PE_parse_boot_argn', '_PE_boot_args', '_cpu_number', '_master_cpu',
                   '_ml_io_map', '_ml_static_vtop', '___stack_chk_guard', '___stack_chk_fail',
                   '_bzero', '_memcpy', '_memset', '_memmove',
                   '_mmu_kvtop', '_phystokv', '_ml_io_unmap',
                   '_native_ufs_export_write', '_native_ufs_export_blocks',
                   '_native_ufs_export_first', '_native_ufs_log_open', '_native_ufs_log_sync',
                   '_native_ufs_log_read',
                   '_native_adreno_gpu_profile_start', '_native_adreno_gpu_profile_stop', '_native_adreno_gpu_profile_read', '_native_adreno_set_hwcg', '_native_adreno_get_gpu_freq',
                   '_native_adreno_a6xx_stats',
                   '_native_legacy_transfer', '_native_legacy_surfaces',
                   '_native_legacy_direct', '_native_legacy_validation',
                   '_native_ufs_dpu_smmu', '_native_ufs_dpu_state',
                   # T142 telemetry store: weak exports the storage driver adds for
                   # T103 4.2. Declared weak_import, so a build without them links and
                   # the TELDUMP/TELREAD verbs answer unsupported at run time.
                   '_native_ufs_tel_open', '_native_ufs_tel_read',
                   '_native_ufs_tel_write', '_native_ufs_tel_sync',
                   # T150: DEVTREE off-console thread needs these XNU imports
                   '_IOLog', '_kernel_thread_start', '_thread_deallocate',
                   '_thread_terminate_self',
                   # T144 fuel gauge: weak imports of the l738-device SPMI layer
                   # (drivers/NabuSpmi).  A kernel without it links, and FGREAD
                   # answers ERR nosp mi at run time.
                   '_nabu_spmi_init', '_nabu_spmi_enabled',
                   '_nabu_spmi_apid_for',
                   '_nabu_spmi_ext_readl', '_nabu_spmi_ext_read',
                   '_Debugger',
                   # T141 bus primitives: SYSREGSCAN's expected-fault claim.
                   # ml_expect_fault_pc_begin/end are the XNU API that lets an
                   # undefined MRS be claimed instead of panicking, and
                   # ml_set_interrupts_enabled brackets the armed window.
                   '_ml_expect_fault_pc_begin', '_ml_expect_fault_end',
                   '_ml_set_interrupts_enabled',
                   # T140 DEVTREE: the IOKit device-tree walk (QcomUsbDTDebug.cpp)
                   # uses the registry API directly -- IORegistryEntry::fromPath on the
                   # gIODTPlane, OSData/OSSymbol metaClass for safeMetaCast, and the
                   # gIODT*Key globals for compatible/device_type/model properties.
                   '__ZN15IORegistryEntry8fromPathEPKcPK15IORegistryPlanePcPiPS_',
                   '__ZN15IORegistryEntry9metaClassE',
                   '__ZN15OSMetaClassBase12safeMetaCastEPKS_PK11OSMetaClass',
                   '__ZN6OSData9metaClassE', '__ZN8OSSymbol9metaClassE',
                   '_gIODTPlane', '_gIODTCompatibleKey', '_gIODTModelKey', '_gIODTTypeKey',
                   '_gPhysBase', '_real_phys_size', '_cpu_ttep',
                   '_gPanicBase', '_gPanicSize',
                   '_sysctlbyname', '_tasks', '_kernproc', '_proc_struct_size',
                   '_kern_stack_snapshot_with_reason', '_kernel_stackshot_buf', '_kernel_stackshot_buf_size',
                   '_kd_control_trace',
                   '_panic',
                   '_kdp_register_send_receive', '_kdp_register_link',
                   '_kdp_set_ip_and_mac_addresses', '_kdp_en_send_pkt', '_z_crc32'}
        if external - allowed:
            raise RuntimeError('unexpected XNU imports: ' + str(sorted(external - allowed)))
        required = {'_qcom_usb_debug_' + s for s in
                    ('init', 'write', 'putc', 'poll', 'panic_prepare', 'panic_drain', 'stop', 'get_status',
                     'control_start', 'control_take_reboot', 'control_arm_reboot',
                     'control_reboot_failed', 'wait_drained', 'register_screenshot', 'register_framebuffer')}
        if not required <= defined:
            raise RuntimeError('missing public API exports')
        # The native adapter must never select the historical identity path,
        # unless the selected profile explicitly opts into the audited identity
        # lease (QUD_DMA_PREFER_IDENTITY=1: the bootloader already routes the
        # USB SID, so no private translated domain can be installed).
        native_imports = subprocess.check_output([nm, '--undefined-only', str(out / 'QcomXnuUsbDebug.o')], text=True)
        identity_lease = re.search(r'^[ \t]*#[ \t]*define[ \t]+QUD_DMA_PREFER_IDENTITY[ \t]+1\b',
                                   profile.read_text(), re.M) is not None
        if not identity_lease and any(s in native_imports for s in ('QcomUsbEnterIdentityDma', 'QcomUsbRestoreDma')):
            raise RuntimeError('identity DMA fallback in native backend')
        report['identity_lease'] = identity_lease
        report['external_imports'] = sorted(external)
        report['public_exports'] = sorted(required)
        report['source_sha256'] = before
        report['archive_sha256'] = hashlib.sha256((out / 'libQcomXnuUsbDebug.a').read_bytes()).hexdigest()
        report['successful_archive'] = True
    finally:
        (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(out / 'libQcomXnuUsbDebug.a')


if __name__ == '__main__':
    main()
