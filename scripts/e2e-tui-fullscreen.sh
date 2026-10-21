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
# 1.106: live streaming (agent.delta), tool blocks (agent.tool), thinking blocks, input queue, model/agent pickers
kill $GW; wait $GW 2>/dev/null || true
mkdir -p "$T/ws"; echo "secret notes" >"$T/ws/notes.txt"
cat >"$T/script.json" <<'J'
[
  {"tool_calls": [{"name": "read", "arguments": {"path": "notes.txt"}}]},
  {"content": "<think>pondering the notes carefully</think>The answer is forty two after reading", "delayMs": 2500}
]
J
OPENCLAW_MOCK_SCRIPT="$T/script.json" OPENCLAW_WORKSPACE="$T/ws" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >>"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
python3 - <<'P' || fail "pty streaming/tools/queue/pickers"
import os, pty, time, select, struct, fcntl, termios, re
BIN = os.environ["BIN"]
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    os.execv(BIN, [BIN, "tui", "--session", "st"])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 110, 0, 0))
buf = b""
def pump(t=0.6, until=None):
    global buf
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try: buf += os.read(fd, 65536)
            except OSError: return
        if until and (until in buf if isinstance(until, bytes) else re.search(until, buf)): return
def send(s): os.write(fd, s.encode() if isinstance(s, str) else s)
pump(4, b"| live")
assert b"| live" in buf, "event stream connected: " + repr(buf[-300:])
send("what do the notes say\r")
pump(3, "✓ read".encode())
assert "✓ read (".encode() in buf, "tool block: " + repr(buf[-1500:])
send("follow up please\r")
pump(1, b"queued (1)")
assert b"queued (1): follow up please" in buf and b"queued 1" in buf, "input queue"
pump(2, "▌".encode())
assert "▌".encode() in buf, "streaming cursor while deltas arrive"
assert "▸ thinking (".encode() in buf, "collapsed thinking block"
pump(6, b"you: follow up please")
assert b"agent: The answer is forty two after reading" in buf, "final streamed answer"
assert b"you: follow up please" in buf, "queued message sent after the reply"
pump(4, re.compile(rb"you: follow up please[\s\S]*agent: "))
buf = b""; send(b"\x14"); pump(1, b"thinking: pondering")
assert b"thinking: pondering the notes carefully" in buf, "Ctrl+T expands thinking"
buf = b""; send(b"\x0f"); pump(1, b"secret notes")
assert b"secret notes" in buf, "Ctrl+O expands tool output"
buf = b""; send(b"\x10"); pump(2, b"Select model")
assert b"Select model for agent:main:st" in buf and b"default (agent setting)" in buf, "model picker"
send(b"\x1b[B"); time.sleep(0.2); send("\r"); pump(2, b"model for this session:")
m = re.search(rb"model for this session: ([A-Za-z0-9_.:-]+)", buf); assert m, buf[-800:]
chosen = m.group(1)
pump(1); assert b"| model " + chosen in buf, "header shows model"
buf = b""; send(b"\x07"); pump(2, b"Switch agent")
assert b"Switch agent" in buf and b"main" in buf, "agent picker"
send("\r"); pump(2, b"agent main (session agent:main:main)")
assert b"agent main (session agent:main:main)" in buf
buf = b""; send(b"\x10"); pump(1, b"Select model"); send(b"\x1b"); pump(1)
send("/queue\r"); pump(1, b"queue empty"); assert b"queue empty" in buf
send("/exit\r"); pump(2)
_, st = os.waitpid(pid, 0)
assert os.WIFEXITED(st) and os.WEXITSTATUS(st) == 0, st
print("ok")
P
curl -s "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d '{"jsonrpc":"2.0","id":1,"method":"sessions.patch","params":{"key":"agent:main:st"}}' | grep -q '"model": "' || fail "picker set the session model override"
echo "[e2e-tui] full-screen tui (pty: edit, history, send, slash, scroll, resize, restore; streaming, tool/thinking blocks, queue, model/agent pickers): PASS"
