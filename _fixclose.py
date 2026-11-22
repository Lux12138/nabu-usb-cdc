import io
P = "tools/nabu_console.py"
t = io.open(P, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in t else "\n"

def rep(a, b, what):
    global t
    a2 = a.replace("\n", nl); b2 = b.replace("\n", nl)
    if a2 not in t: raise SystemExit("anchor: " + what)
    t = t.replace(a2, b2, 1)

rep('''        try:
            self.w.join(timeout=2)
        except Exception:
            pass
        try:
            self.s.cancel_read()''',
'''        writer_stuck = False
        try:
            self.w.join(timeout=2)
            writer_stuck = self.w.is_alive()
        except Exception:
            writer_stuck = True
        try:
            self.s.cancel_read()''', "detect stuck writer")

rep('''        try:
            self.out.close()
        except Exception:
            pass
        try:
            self.s.close()
        except Exception:
            pass''',
'''        try:
            self.out.close()
        except Exception:
            pass
        if writer_stuck:
            # Closing a port whose writer thread is parked in a kernel wait would
            # block this thread too, re-creating the unkillable-process problem.
            # Leaving it to process exit is the least-bad option: the OS reclaims
            # the handle when the process dies.
            self.log("! writer thread stuck; skipping port close (handle freed on exit)")
            return
        try:
            self.s.close()
        except Exception:
            pass''', "skip close when stuck")

io.open(P, "w", encoding="utf-8", newline="").write(t)
print("close() now skips s.close() when the writer is stuck")
