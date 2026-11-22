import io
P = "tools/nabu_console.py"
t = io.open(P, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in t else "\n"

if "self.wq" in t:
    print("ALREADY"); raise SystemExit(0)

def rep(a, b, what):
    global t
    a2 = a.replace("\n", nl); b2 = b.replace("\n", nl)
    if a2 not in t: raise SystemExit("anchor: " + what)
    t = t.replace(a2, b2, 1)

# 1) writer queue + thread
rep('''        self.t = threading.Thread(target=self._reader, daemon=True)
        self.t.start()''',
'''        self.t = threading.Thread(target=self._reader, daemon=True)
        self.t.start()
        # Writes go through their own thread.
        #
        # serial.write() blocks inside the Windows driver when the kernel stops
        # servicing the CDC (measured: the process became unkillable and held
        # COM3 forever, because a thread parked in that kernel wait cannot be
        # terminated). With a writer thread the main loop keeps running, so the
        # stall watchdog can still fire and close the port cleanly.
        self.wq = queue.Queue()
        self.w = threading.Thread(target=self._writer, daemon=True)
        self.w.start()
        self.write_stuck = False''', "writer state")

# 2) writer thread body
rep('''    def send(self, payload):
        self.s.write(payload); self.s.flush()''',
'''    def _writer(self):
        while self.running:
            try:
                payload = self.wq.get(timeout=0.2)
            except queue.Empty:
                continue
            if payload is None:
                break
            try:
                self.s.write(payload)
                self.s.flush()
            except Exception as e:
                # A write failure means the kernel is not reading; flag it and
                # let the main loop decide (the watchdog will close the port).
                self.write_stuck = True
                self.log("WRITE ERR %s" % e)
                break

    def send(self, payload):
        """Enqueue a frame. Never blocks on the device."""
        self.wq.put(payload)''', "writer thread")

# 3) close(): stop the writer too
rep('''        self.running = False
        try:
            self.s.cancel_read()''',
'''        self.running = False
        try:
            self.wq.put(None)
        except Exception:
            pass
        try:
            self.w.join(timeout=2)
        except Exception:
            pass
        try:
            self.s.cancel_read()''', "close writer")

# 4) main loop: treat a stuck writer like a stall
rep('''        if sess.total != last_output_total:
            last_output_total = sess.total
            last_output_at = time.time()''',
'''        if sess.write_stuck:
            log(">>> write stuck (kernel not servicing the CDC); releasing the port")
            event(kind="write_stuck")
            break
        if sess.total != last_output_total:
            last_output_total = sess.total
            last_output_at = time.time()''', "write stuck break")

io.open(P, "w", encoding="utf-8", newline="").write(t)
print("writer thread added; write_stuck exits the loop")
