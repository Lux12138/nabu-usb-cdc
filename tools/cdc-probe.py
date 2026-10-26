#!/usr/bin/env python3
"""Step-by-step open/read probe for the XNU USB CDC port on Windows.

Waits for VID_1209/PID_316D, dumps the PnP problem state, then walks the Win32
open sequence with GetLastError after every call, and finally tries pyserial
twice. Everything is timestamped into --out; raw bytes (if any) go to --raw.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import datetime
import json
import subprocess
import sys
import time
from pathlib import Path

import serial
from serial.tools import list_ports

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
SETRTS = 3
SETDTR = 5


class DCB(ctypes.Structure):
    _fields_ = [
        ('DCBlength', wt.DWORD), ('BaudRate', wt.DWORD),
        ('flags', wt.DWORD),
        ('wReserved', wt.WORD), ('XonLim', wt.WORD), ('XoffLim', wt.WORD),
        ('ByteSize', wt.BYTE), ('Parity', wt.BYTE), ('StopBits', wt.BYTE),
        ('XonChar', ctypes.c_char), ('XoffChar', ctypes.c_char),
        ('ErrorChar', ctypes.c_char), ('EofChar', ctypes.c_char),
        ('EvtChar', ctypes.c_char), ('wReserved1', wt.WORD),
    ]


class COMMTIMEOUTS(ctypes.Structure):
    _fields_ = [('ReadIntervalTimeout', wt.DWORD), ('ReadTotalTimeoutMultiplier', wt.DWORD),
                ('ReadTotalTimeoutConstant', wt.DWORD), ('WriteTotalTimeoutMultiplier', wt.DWORD),
                ('WriteTotalTimeoutConstant', wt.DWORD)]


k32.CreateFileW.restype = ctypes.c_void_p
k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, ctypes.c_void_p]
k32.GetCommState.argtypes = [ctypes.c_void_p, ctypes.POINTER(DCB)]
k32.SetCommState.argtypes = [ctypes.c_void_p, ctypes.POINTER(DCB)]
k32.SetCommTimeouts.argtypes = [ctypes.c_void_p, ctypes.POINTER(COMMTIMEOUTS)]
k32.SetupComm.argtypes = [ctypes.c_void_p, wt.DWORD, wt.DWORD]
k32.PurgeComm.argtypes = [ctypes.c_void_p, wt.DWORD]
k32.SetCommMask.argtypes = [ctypes.c_void_p, wt.DWORD]
k32.EscapeCommFunction.argtypes = [ctypes.c_void_p, wt.DWORD]
k32.ReadFile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
k32.ReadFile.restype = wt.BOOL
k32.CloseHandle.argtypes = [ctypes.c_void_p]


def now():
    return datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]


def err():
    return ctypes.get_last_error()


def wait_port(seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        for p in list_ports.comports():
            hwid = (getattr(p, 'hwid', '') or '').upper()
            desc = ' '.join(str(x) for x in (p.description, p.product, p.manufacturer) if x)
            if '1209' in hwid and '316D' in hwid or 'QCOMXNUUSBDEBUG' in desc.upper() or 'QCOMXNUUSBDEBUG' in hwid:
                return p.device, desc, hwid
        time.sleep(2)
    return None, None, None


def pnp_state(port):
    script = ("Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | "
              "Where-Object { $_.InstanceId -match '1209' -and $_.InstanceId -match '316D' } | "
              "Select-Object Status,Class,FriendlyName,InstanceId,Problem | ConvertTo-Json -Compress")
    try:
        out = subprocess.run(['powershell.exe', '-NoProfile', '-Command', script],
                             capture_output=True, text=True, timeout=20)
        return out.stdout.strip() or out.stderr.strip()
    except Exception as e:  # noqa: BLE001
        return 'pnp query failed: %s' % e


def raw_probe(port, log, raw_path):
    path = '\\\\.\\%s' % port
    h = k32.CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, None, OPEN_EXISTING, 0, None)
    log('CreateFileW(%s) -> %s err=%d' % (path, h, err()))
    if h == INVALID_HANDLE_VALUE or h is None:
        return
    try:
        dcb = DCB()
        dcb.DCBlength = ctypes.sizeof(DCB)
        ok = k32.GetCommState(ctypes.c_void_p(h), ctypes.byref(dcb))
        log('GetCommState -> %d err=%d baud=%d bytesize=%d parity=%d stop=%d flags=0x%08x' %
            (ok, err(), dcb.BaudRate, dcb.ByteSize, dcb.Parity, dcb.StopBits, dcb.flags))

        ok = k32.SetCommState(ctypes.c_void_p(h), ctypes.byref(dcb))
        log('SetCommState(as returned) -> %d err=%d' % (ok, err()))

        dcb.BaudRate = 115200
        dcb.ByteSize = 8
        dcb.Parity = 0
        dcb.StopBits = 0
        dcb.flags = (1 << 0) | (1 << 4) | (1 << 12)  # binary, DTR on, RTS on
        ok = k32.SetCommState(ctypes.c_void_p(h), ctypes.byref(dcb))
        log('SetCommState(115200 8N1 dtr/rts on) -> %d err=%d' % (ok, err()))

        ok = k32.SetupComm(ctypes.c_void_p(h), 4096, 4096)
        log('SetupComm -> %d err=%d' % (ok, err()))
        ok = k32.PurgeComm(ctypes.c_void_p(h), 0x000F)
        log('PurgeComm -> %d err=%d' % (ok, err()))

        tmo = COMMTIMEOUTS(0, 0, 1000, 0, 1000)
        ok = k32.SetCommTimeouts(ctypes.c_void_p(h), ctypes.byref(tmo))
        log('SetCommTimeouts -> %d err=%d' % (ok, err()))

        ok = k32.SetCommMask(ctypes.c_void_p(h), 0)
        log('SetCommMask(0) -> %d err=%d' % (ok, err()))
        ok = k32.EscapeCommFunction(ctypes.c_void_p(h), SETDTR)
        log('EscapeCommFunction(SETDTR) -> %d err=%d' % (ok, err()))
        ok = k32.EscapeCommFunction(ctypes.c_void_p(h), SETRTS)
        log('EscapeCommFunction(SETRTS) -> %d err=%d' % (ok, err()))

        buf = ctypes.create_string_buffer(4096)
        got = wt.DWORD(0)
        t0 = time.time()
        ok = k32.ReadFile(ctypes.c_void_p(h), buf, 4096, ctypes.byref(got), None)
        log('ReadFile -> %d err=%d bytes=%d elapsed=%.3fs' % (ok, err(), got.value, time.time() - t0))
        if got.value:
            with open(raw_path, 'ab') as f:
                f.write(buf.raw[:got.value])
            log('raw first bytes: %s' % buf.raw[:min(64, got.value)].hex())
    finally:
        k32.CloseHandle(ctypes.c_void_p(h))
        log('CloseHandle')


def pyserial_probe(port, log, raw_path, **kw):
    label = 'pyserial(%s)' % (', '.join('%s=%s' % i for i in kw.items()) or 'defaults')
    try:
        with serial.Serial(port, timeout=1, **kw) as s:
            log('%s open ok' % label)
            t0 = time.time()
            data = s.read(4096)
            log('%s read bytes=%d elapsed=%.3fs' % (label, len(data), time.time() - t0))
            if data:
                with open(raw_path, 'ab') as f:
                    f.write(data)
                log('%s first bytes: %s' % (label, data[:64].hex()))
    except Exception as e:  # noqa: BLE001
        log('%s FAILED: %s winerror=%s' % (label, e, getattr(e, 'winerror', None)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--wait', type=int, default=180)
    ap.add_argument('--out', default='logs/probe.txt')
    ap.add_argument('--raw', default='logs/probe.bin')
    a = ap.parse_args()
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    lines = []

    def log(msg):
        line = '%s %s' % (now(), msg)
        lines.append(line)
        print(line, flush=True)
        Path(a.out).write_text('\n'.join(lines) + '\n', encoding='utf-8')

    port, desc, hwid = wait_port(a.wait)
    if not port:
        log('no CDC port within %ss' % a.wait)
        return 2
    log('found %s [%s] [%s]' % (port, desc, hwid))
    pnp = pnp_state(port)
    log('PnP: %s' % pnp)

    raw_probe(port, log, a.raw)

    try:
        pyserial_probe(port, log, a.raw)
        pyserial_probe(port, log, a.raw, baudrate=115200, rtscts=False, dsrdtr=False)
    except Exception as e:  # noqa: BLE001
        log('pyserial phase aborted: %s' % e)
    log('done')
    return 0


if __name__ == '__main__':
    sys.exit(main())
