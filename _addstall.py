import io
P = "tools/nabu_console.py"
t = io.open(P, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in t else "\n"

if "stall_watchdog" in t:
    print("ALREADY"); raise SystemExit(0)

def rep(a, b, what):
    global t
    a2 = a.replace("\n", nl); b2 = b.replace("\n", nl)
    if a2 not in t: raise SystemExit("anchor: " + what)
    t = t.replace(a2, b2, 1)

# option
rep('''    ap.add_argument("--cmd-timeout", type=float, default=10.0)''',
'''    ap.add_argument("--cmd-timeout", type=float, default=10.0)
    ap.add_argument("--stall-exit", type=float, default=0.0,
                    help="设备 N 秒无任何输出就主动退出并释放端口（0=不启用）")''',
    "option")

# stall watchdog in the main loop + a last-output timestamp
rep('''    last_idle_check = time.time()''',
'''    last_idle_check = time.time()
    last_output_at = time.time()
    last_output_total = 0''', "state")

rep('''    while sess.alive:
        el = time.time() - t0
        if args.deadline and el > args.deadline:
            log("deadline reached"); break''',
'''    while sess.alive:
        el = time.time() - t0
        if args.deadline and el > args.deadline:
            log("deadline reached"); break

        # stall_watchdog: if the device stops producing bytes for too long the
        # kernel is almost certainly wedged (e.g. inside the debugger, where it
        # no longer services the CDC). Exiting here is what actually releases
        # the Windows handle: a blocked serial.read() keeps the OS handle alive
        # and Stop-Process cannot kill a thread stuck in that kernel wait.
        if sess.total != last_output_total:
            last_output_total = sess.total
            last_output_at = time.time()
        elif args.stall_exit > 0 and (time.time() - last_output_at) > args.stall_exit:
            log(">>> no device output for %.0fs (stall watchdog); releasing the port"
                % (time.time() - last_output_at))
            event(kind="stall_exit", idle=round(time.time() - last_output_at, 1))
            break''', "watchdog")

io.open(P, "w", encoding="utf-8", newline="").write(t)
print("stall watchdog added")
