"""Offline test of the PANICDUMP header decode using the offsets the kernel
module uses. Builds a synthetic region the way XNU would write it, then parses
it with the same field offsets, to prove the layout arithmetic is right before
spending a board round."""
import struct

# Offsets from the module (verified by compiling the struct: sizeof=169)
OFF = dict(magic=0x00, crc=0x04, version=0x08, flags=0x0c,
           log_offset=0x14, log_len=0x18, ss_offset=0x1c, ss_len=0x20,
           other_offset=0x24, other_len=0x28, os_version=0x34)
FUNK = 0x46554E4B
SIZE = 0x80000

def build(magic, log_text, version=4, flags=0x03):
    hdr = bytearray(169)
    lo = 169                      # log starts right after the header
    struct.pack_into('<I', hdr, OFF['magic'], magic & 0xFFFFFFFF)
    struct.pack_into('<I', hdr, OFF['crc'], 0xDEADBEEF)
    struct.pack_into('<I', hdr, OFF['version'], version)
    struct.pack_into('<Q', hdr, OFF['flags'], flags)
    struct.pack_into('<I', hdr, OFF['log_offset'], lo)
    struct.pack_into('<I', hdr, OFF['log_len'], len(log_text))
    struct.pack_into('<I', hdr, OFF['ss_offset'], 0)
    struct.pack_into('<I', hdr, OFF['ss_len'], 0)
    struct.pack_into('<I', hdr, OFF['other_offset'], 0)
    struct.pack_into('<I', hdr, OFF['other_len'], 0)
    osv = b"XNU 17.7.11 21H461\x00"
    hdr[OFF['os_version']:OFF['os_version']+len(osv)] = osv
    return bytes(hdr) + log_text

def parse(region):
    """Mirror of qud_panic_header()."""
    if len(region) < 0x34:
        return -1, None
    magic = struct.unpack_from('<I', region, OFF['magic'])[0]
    out = dict(magic=magic,
               version=struct.unpack_from('<I', region, OFF['version'])[0],
               flags=struct.unpack_from('<Q', region, OFF['flags'])[0],
               log_offset=struct.unpack_from('<I', region, OFF['log_offset'])[0],
               log_len=struct.unpack_from('<I', region, OFF['log_len'])[0],
               ss_len=struct.unpack_from('<I', region, OFF['ss_len'])[0],
               other_len=struct.unpack_from('<I', region, OFF['other_len'])[0])
    raw = region[OFF['os_version']:OFF['os_version']+32]
    out['os'] = raw.split(b'\x00')[0].decode('ascii', 'replace')
    if magic != FUNK:
        return 0, out
    if out['log_len'] and (out['log_offset'] > SIZE or
                           out['log_len'] > SIZE - out['log_offset']):
        return -2, out
    return 1, out

fail = 0
def check(name, region, want_status, want=None):
    global fail
    st, d = parse(region)
    ok = (st == want_status)
    if ok and want:
        for k, v in want.items():
            if d.get(k) != v:
                ok = False
                print("  FAIL %s: %s=%r want %r" % (name, k, d.get(k), v))
    print("%s %-34s status=%d %s" % ("PASS" if ok else "FAIL", name, st,
                                     "" if not d else "os=%r loglen=%d" % (d['os'], d['log_len'])))
    if not ok:
        fail += 1

panic_text = b"panic(cpu 0 caller 0xfffffff007abe624): Kernel data abort\n"
check("valid panic", build(FUNK, panic_text), 1,
      dict(magic=FUNK, log_len=len(panic_text), os="XNU 17.7.11 21H461"))

check("cleared region", build(0, b""), 0, dict(magic=0))
check("SHMC console", build(0x53484D43, b""), 0, dict(magic=0x53484D43))
check("boot profile BTRC", build(0x42545243, b""), 0, dict(magic=0x42545243))

# absurd log_len must be rejected, not followed
bad = bytearray(build(FUNK, panic_text))
struct.pack_into('<I', bad, OFF['log_len'], 0x7FFFFFFF)
check("absurd log_len", bytes(bad), -2)

# log_offset past the end
bad2 = bytearray(build(FUNK, panic_text))
struct.pack_into('<I', bad2, OFF['log_offset'], SIZE + 0x1000)
check("log_offset past end", bytes(bad2), -2)

# log running off the end
bad3 = bytearray(build(FUNK, b"x" * 64))
struct.pack_into('<I', bad3, OFF['log_offset'], SIZE - 8)
check("log runs off end", bytes(bad3), -2)

print()
if fail:
    print("%d FAILURE(S)" % fail)
    raise SystemExit(1)
print("ALL PASS")
