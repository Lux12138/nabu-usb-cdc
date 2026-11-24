#!/bin/bash
# kdp.sh -- one entry point for the KDP-over-CDC workflow.
#
# Why this exists: on win32 a serial port can only be opened exclusively
# ("win32 only supports exclusive access (not: False)", measured on kb-165), so
# the nabu console and the KDP bridge cannot hold COM3 at the same time. Until
# the bridge learns to forward SXNU1 control frames itself, every KDP session
# needs the console stopped first. Doing that by hand is where rounds get lost:
# a stale python holding the port looks exactly like "the bridge cannot open it".
#
#   ./kdp.sh status              who holds COM3, is the bridge up, does KDP answer
#   ./kdp.sh console [tag]       run the nabu console in the foreground
#   ./kdp.sh bridge [framing]    stop the console, start the bridge (default kdpf)
#   ./kdp.sh stop                stop every python that might hold the port
#   ./kdp.sh cli <args...>       run kdp_cli over UDP against a live bridge
#
# framing: kdpf  = device side sends raw Ethernet frames (nabu-kdp-net=1)  [B layer]
#          serial = device side uses XNU serial KDP (nabu-cdc-rx=1)        [A layer]
set -uo pipefail

PORT=${PORT:-COM3}
UDP_PORT=${UDP_PORT:-41139}
ROOT=/e/nabu/state/bridge/answers
BRIDGE=$ROOT/kdp_net_bridge/kdp_net_bridge.py
CLIENT=$ROOT/kdp_client
LOGS=/e/nabu/logs

py_count() {
    MSYS_NO_PATHCONV=1 powershell -NoProfile -Command \
        "(Get-Process python -ErrorAction SilentlyContinue | Measure-Object).Count" 2>/dev/null | tr -d '\r'
}

port_state() {
    MSYS_NO_PATHCONV=1 powershell -NoProfile -Command \
        "[System.IO.Ports.SerialPort]::GetPortNames()" 2>/dev/null | tr -d '\r' | grep -c "^$PORT$"
}

stop_all() {
    local n
    n=$(py_count)
    if [ "${n:-0}" = "0" ]; then
        echo "[kdp] no python processes running"
        return 0
    fi
    echo "[kdp] stopping $n python process(es) that may hold $PORT"
    MSYS_NO_PATHCONV=1 powershell -NoProfile -Command \
        "Get-Process python -ErrorAction SilentlyContinue | Stop-Process -Force" 2>/dev/null
    for _ in $(seq 1 10); do
        sleep 1
        [ "$(py_count)" = "0" ] && { echo "[kdp] port released"; return 0; }
    done
    echo "[kdp] WARNING: a python process survived Stop-Process"
    return 1
}

cmd_status() {
    echo "== python processes: $(py_count)"
    echo "== $PORT present:    $(port_state)"
    MSYS_NO_PATHCONV=1 powershell -NoProfile -Command \
        "[System.IO.Ports.SerialPort]::GetPortNames()" 2>/dev/null | tr -d '\r' | sed 's/^/   /'
    if [ -f "$LOGS/kdp-bridge.log" ]; then
        echo "== last bridge log lines:"
        tail -3 "$LOGS/kdp-bridge.log" 2>/dev/null | sed 's/^/   /'
    fi
    echo "== kdp ping:"
    ( cd "$CLIENT" && python kdp_cli.py --transport udp --host 127.0.0.1 \
        --udp-port "$UDP_PORT" version 2>&1 | tail -2 | sed 's/^/   /' )
}

cmd_console() {
    local tag=${1:-live}
    stop_all || return 1
    cd /d/_nabu-usb-cdc || return 1
    rm -f "$LOGS/$tag-cmd.txt"
    exec python -u tools/nabu_console.py --tag "$tag" --out E:/nabu/logs \
        --wait 400 --deadline 7200 --stall-exit 0 \
        --kernel "$(ls -d /e/nabu/hold/kb-* | sort -V | tail -1)/kernel.macho"
}

cmd_bridge() {
    local framing=${1:-kdpf}
    stop_all || return 1
    : > "$LOGS/kdp-bridge.log"
    cd "$ROOT/kdp_net_bridge" || return 1
    # nohup so the bridge outlives this shell; the log is the only handle on it
    nohup python -u kdp_net_bridge.py --port "$PORT" --cdc-framing "$framing" \
        --udp-mode eth --verbose > "$LOGS/kdp-bridge.log" 2>&1 &
    sleep 6
    tail -4 "$LOGS/kdp-bridge.log"
    if grep -q "cannot open" "$LOGS/kdp-bridge.log"; then
        echo "[kdp] bridge failed to open $PORT -- run './kdp.sh status'"
        return 1
    fi
    echo "[kdp] bridge up (framing=$framing). Try: ./kdp.sh cli connect"
}

cmd_cli() {
    cd "$CLIENT" || return 1
    python kdp_cli.py --transport udp --host 127.0.0.1 --udp-port "$UDP_PORT" "$@"
}

case "${1:-status}" in
    status)  cmd_status ;;
    console) shift; cmd_console "$@" ;;
    bridge)  shift; cmd_bridge "$@" ;;
    stop)    stop_all ;;
    cli)     shift; cmd_cli "$@" ;;
    *)       sed -n '2,20p' "$0" | sed 's/^# \?//' ;;
esac
