#!/usr/bin/env bash
# 1.101: full-screen tui under a pseudo-terminal: alternate screen, header, wrapped transcript, line editing,
# history recall, background send + reply, slash commands, PgUp scroll, resize, clean terminal restore.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "[e2e-tui] FAIL: $*"; exit 1; }
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
export OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_SESSION_DIR="$T/sess" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/none.json" HOME="$T/home"
mkdir -p "$T/home"
OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
export OPENCLAW_GATEWAY_URL="http://127.0.0.1:$PORT" BIN
python3 - <<'P' || fail "pty session"
import os, pty, time, select, struct, fcntl, termios, signal, sys
BIN = os.environ["BIN"]
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    os.execv(BIN, [BIN, "tui", "--session", "fs"])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 20, 60, 0, 0))
buf = b""
def pump(t=0.6, until=None):
    global buf
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try: buf += os.read(fd, 65536)
            except OSError: return
        if until and until in buf: return
def send(s): os.write(fd, s.encode() if isinstance(s, str) else s)
pump(3, b"openclaw tui | ")
assert b"\x1b[?1049h" in buf, "alternate screen"
assert b"openclaw tui | agent:main:fs" in buf, buf[-400:]
send("helo"); send(b"\x1b[D"); send("l"); send(b"\x1b[F"); send(" world\r")   # cursor edit → "hello world"
pump(4, b"agent: ")
assert b"you: hello world" in buf, buf[-800:]
assert b"agent: " in buf, buf[-800:]
buf = b""; send(b"\x1b[A"); pump(0.5)
assert b"hello world" in buf, "history recall"
send(b"\x15")  # Ctrl+U clears the line
send("/help\r"); pump(1, b"/sessions")
assert b"/session <key>" in buf, buf[-600:]
send("/bogus\r"); pump(1, b"unknown command")
assert b"unknown command /bogus" in buf
for i in range(6): send(f"msg {i}\r"); pump(2, f"msg {i}".encode())
pump(2)
buf = b""; send(b"\x1b[5~"); pump(0.6)
assert b"scrolled" in buf, "PgUp scroll indicator"
send(b"\x1b[6~")
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0)); os.kill(pid, signal.SIGWINCH)
buf = b""; pump(0.8)
assert b"\x1b[30;1H" in buf, "redraw at new size"
send("/exit\r"); pump(2)
_, st = os.waitpid(pid, 0)
assert os.WIFEXITED(st) and os.WEXITSTATUS(st) == 0, st
assert b"\x1b[?1049l" in buf, "terminal restored"
print("ok")
P
# history persisted through the gateway: plain mode sees the full-screen turn
OUT=$(printf '/history\n/exit\n' | "$BIN" tui --session fs --plain 2>/dev/null)
grep -q "^you: .*hello world" <<<"$OUT" || { echo "$OUT"; fail "transcript via gateway"; }
echo "[e2e-tui] full-screen tui (pty: edit, history, send, slash, scroll, resize, restore): PASS"
