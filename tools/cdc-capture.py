#!/usr/bin/env python3
"""Capture the XNU USB CDC console across one boot, until the port disappears.

Waits for VID_1209/PID_316D, waits 0.5 s after the PnP registration, opens with
up to 5 one-second retries, then polls with a 150 ms timeout until the device
goes away (or --seconds elapses). Raw bytes go to --raw, one line per read to
--tsv (time, length, first 16 bytes), and open/disconnect events to both.
"""
import argparse
import datetime
import sys
import time
from pathlib import Path

import serial
from serial.tools import list_ports


def now():
    return datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]


def cdc_ports():
    out = []
    for p in list_ports.comports():
        desc = ' '.join(str(x) for x in (p.description, p.product, p.manufacturer) if x)
        hwid = (getattr(p, 'hwid', '') or '').upper()
        if 'QCOMXNUUSBDEBUG' in hwid or 'QCOMXNUUSBDEBUG' in desc.upper() or \
                ('1209' in hwid and '316D' in hwid):
            out.append((p.device, desc, hwid))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--seconds', type=int, default=180)
    ap.add_argument('--wait', type=int, default=180)
    ap.add_argument('--raw', default='logs/cdc.bin')
    ap.add_argument('--tsv', default='logs/cdc.tsv')
    a = ap.parse_args()

    log_path = Path(a.tsv).with_suffix('.log')
    log_path.parent.mkdir(parents=True, exist_ok=True)
    events = []

    def log(msg):
        line = '%s %s' % (now(), msg)
        events.append(line)
        print(line, flush=True)
        log_path.write_text('\n'.join(events) + '\n', encoding='utf-8')

    port = a.port
    if not port:
        deadline = time.time() + a.wait
        while time.time() < deadline:
            hits = cdc_ports()
            if hits:
                port = hits[0][0]
                log('found %s [%s] [%s]' % (port, hits[0][1], hits[0][2]))
                break
            log('waiting for CDC ... %.0fs left' % (deadline - time.time()))
            time.sleep(2)
        if not port:
            log('no CDC port within %ss' % a.wait)
            return 2

    time.sleep(0.5)  # PnP registers before the CDC interface is ready
    s = None
    for attempt in range(1, 6):
        try:
            s = serial.Serial(port, a.baud, timeout=0.15)
            log('open %s @%d ok (attempt %d)' % (port, a.baud, attempt))
            break
        except Exception as e:  # noqa: BLE001
            log('open attempt %d failed: %s winerror=%s' % (attempt, e, getattr(e, 'winerror', None)))
            time.sleep(1)
    if s is None:
        log('could not open %s' % port)
        return 3

    total = 0
    last_read = None
    last_read_at = None
    started = time.time()
    reason = 'timeout'
    try:
        with open(a.raw, 'ab', buffering=0) as raw, open(a.tsv, 'a', encoding='utf-8', buffering=1) as tsv:
            tsv.write('%s open %s %d\n' % (now(), port, a.baud))
            while time.time() - started < a.seconds:
                try:
                    data = s.read(4096)
                except serial.SerialException as e:
                    reason = 'disconnect: %s' % e
                    log('disconnect at %s: %s' % (now(), e))
                    break
                except Exception as e:  # noqa: BLE001
                    reason = '%s: %s' % (type(e).__name__, e)
                    log('read error: %s' % reason)
                    break
                if not data:
                    continue
                raw.write(data)
                total += len(data)
                last_read = time.time()
                last_read_at = now()
                tsv.write('%s %d %s\n' % (now(), len(data), data[:16].hex()))
                sys.stdout.write(data.decode('utf-8', 'replace'))
                sys.stdout.flush()
    finally:
        try:
            s.close()
        except Exception:  # noqa: BLE001
            pass

    gap = 'n/a' if last_read is None else '%.3fs' % (time.time() - last_read)
    log('end %s bytes=%d elapsed=%.1fs last_read=%s gap_to_end=%s'
        % (reason, total, time.time() - started,
           'never' if last_read is None else last_read_at, gap))
    return 0 if total else 4


if __name__ == '__main__':
    sys.exit(main())
