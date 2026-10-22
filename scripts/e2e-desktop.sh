#!/usr/bin/env bash
# Real desktop control under Xvfb + private D-Bus/AT-SPI: launch policy, windows, a11y tree, a11y click/set_text, release.
# SKIPs honestly when Xvfb/xdotool/python3-gi Atspi/dbus-run-session are missing.
set -euo pipefail
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
for b in Xvfb xdotool dbus-run-session python3; do command -v $b >/dev/null || { echo "[e2e-desktop] SKIP ($b missing)"; exit 0; }; done
python3 -c "import gi; gi.require_version('Atspi','2.0'); gi.require_version('Gtk','3.0'); from gi.repository import Atspi, Gtk" 2>/dev/null || { echo "[e2e-desktop] SKIP (python3-gi Atspi/Gtk missing)"; exit 0; }
if [[ "${OC_DESKTOP_INNER:-}" != 1 ]]; then OC_DESKTOP_INNER=1 exec timeout 120 dbus-run-session -- bash "$0" "$@" 2> >(grep -v 'dbus-daemon\|SpiRegistry' >&2); fi
T=$(mktemp -d); PORT=$((20000 + RANDOM % 10000)); DN=$((90 + RANDOM % 100))
export OPENCLAW_STATE_DIR="$T/state" OPENCLAW_DATA_DIR="$T/data" OPENCLAW_CONFIG_PATH="$T/openclaw.json" HOME="$T" OPENCLAW_SESSION_DIR="$T/sess"
unset OPENCLAW_GATEWAY_TOKEN OPENCLAW_DESKTOP_LAUNCH OPENCLAW_DESKTOP_APPS
Xvfb ":$DN" -screen 0 1024x768x24 >/dev/null 2>&1 & XV=$!
export DISPLAY=":$DN" NO_AT_BRIDGE=0 GTK_A11Y=atspi
APP="$T/oc-a11y-app"; cp "$ROOT/scripts/fixtures/a11y-test-app.py" "$APP"; sed -i '1i #!/usr/bin/env python3' "$APP"; chmod +x "$APP"
GW=
cleanup() { [[ -n "$GW" ]] && kill $GW 2>/dev/null; kill $XV 2>/dev/null; true; }
trap 'cleanup; _reap' EXIT
fail() { echo "[e2e-desktop] FAIL: $*"; tail -20 "$T/gw.log" 2>/dev/null; exit 1; }
for _ in $(seq 30); do xdotool getmouselocation >/dev/null 2>&1 && break; sleep 0.1; done
start_gw() { "$BIN" gateway --port "$PORT" --host 127.0.0.1 >"$T/gw.log" 2>&1 & GW=$!
  for _ in $(seq 80); do curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && break; sleep 0.1; done; }
rpc() { curl -s -X POST "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d "$1"; }
start_gw
rpc "{\"method\":\"desktop.launch\",\"params\":{\"app\":\"$APP\"}}" | grep -q 'denied' || fail "launch must be denied by default"
kill $GW; wait $GW 2>/dev/null || true
export OPENCLAW_DESKTOP_APPS="oc-a11y-app"
start_gw
L=$(rpc "{\"method\":\"desktop.launch\",\"params\":{\"app\":\"$APP\",\"wait_ms\":8000}}")
printf '%s' "$L" | grep -Eq '"ok": *true' || fail "launch: $L"
PID=$(printf '%s' "$L" | python3 -c 'import json,sys;print(json.load(sys.stdin)["payload"]["pid"])')
rpc '{"method":"desktop.windows"}' | grep -q 'OC Test App' || fail "window list"
sleep 0.5
rpc '{"method":"desktop.a11y.tree","params":{"app":"oc-a11y-app"}}' > "$T/tree.json"
python3 "$ROOT/scripts/fixtures/a11y-find.py" "$T/tree.json" > "$T/paths"
read -r BPATH EPATH < "$T/paths"
[[ "$BPATH" != null ]] || { head -c 1500 "$T/tree.json"; fail "button not in a11y tree"; }
grep -q 'count=0' "$T/tree.json" || fail "label in tree"
if [[ "$EPATH" != null ]]; then rpc "{\"method\":\"desktop.a11y.action\",\"params\":{\"path\":$EPATH,\"text\":\"hello-a11y\"}}" | grep -Eq '"ok": *true' || fail "set_text"; fi
rpc "{\"method\":\"desktop.a11y.action\",\"params\":{\"path\":$BPATH,\"action\":\"click\"}}" | grep -Eq '"ok": *true' || fail "a11y click"
sleep 0.5
rpc '{"method":"desktop.a11y.tree","params":{"app":"oc-a11y-app"}}' > "$T/tree2.json"
grep -q 'count=1' "$T/tree2.json" || fail "click took effect"
if [[ "$EPATH" != null ]]; then grep -q 'hello-a11y' "$T/tree2.json" || fail "entry text set"; fi
rpc '{"method":"tools.invoke","params":{"tool":"desktop","args":{"action":"windows"}}}' | grep -q 'OC Test App' || fail "desktop agent tool"
rpc '{"method":"desktop.observe"}' | grep -q '"windows"' || fail "observe"
rpc "{\"method\":\"desktop.release\",\"params\":{\"pid\":$PID}}" | grep -q "$PID" || fail "release"
sleep 0.3; kill -0 "$PID" 2>/dev/null && fail "process alive after release"
echo "[e2e-desktop] PASS"
