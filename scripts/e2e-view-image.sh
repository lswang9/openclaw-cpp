#!/usr/bin/env bash
# 1.133: view_image results reach the model as real image input (Anthropic image block, Gemini inlineData,
# Responses input_image, chat-completions image_url) and the base64 is stripped from the tool-result text.
set -eu
# reap background gateways/fakes on any exit, including timeout/^C
_reap(){ kill $(jobs -p) 2>/dev/null || true; }
trap '_reap; exit 143' INT TERM HUP
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; FL=""
trap '[[ -n "$GW" ]] && kill $GW 2>/dev/null; [[ -n "$FL" ]] && kill $FL 2>/dev/null; wait 2>/dev/null; [[ -z "${KEEP:-}" ]] && rm -rf "$T"; _reap' EXIT
fail() { echo "[e2e-view-image] FAIL: $*"; echo "T=$T"; [[ -f "$T/gw.log" ]] && tail -8 "$T/gw.log"; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN OPENCLAW_DEFAULT_PROVIDER ANTHROPIC_API_KEY GEMINI_API_KEY GOOGLE_API_KEY OPENCLAW_DOCS_URL
python3 - "$T/img.png" <<'PY'
import struct,zlib,sys
def ch(t,d): return struct.pack(">I",len(d))+t+d+struct.pack(">I",zlib.crc32(t+d)&0xffffffff)
raw=b"".join(b"\0"+b"\xff\0\0"*3 for _ in range(2))
open(sys.argv[1],"wb").write(b"\x89PNG\r\n\x1a\n"+ch(b"IHDR",struct.pack(">IIBBBBB",3,2,8,2,0,0,0))+ch(b"IDAT",zlib.compress(raw))+ch(b"IEND",b""))
PY
FP=$(freeport); FAKE_TOOL=view_image FAKE_TOOL_ARGS="{\"image\":\"$T/img.png\",\"prompt\":\"what colour\"}" python3 "$ROOT/scripts/fake_llm.py" "$FP" "$T/llm.log" & FL=$!
B="http://127.0.0.1:$FP"
cat > "$T/cfg.json" <<JSON
{"models":{"providers":{
   "anth":{"api":"anthropic-messages","baseUrl":"$B","apiKey":"ak-test"},
   "gem":{"api":"google-generative-ai","baseUrl":"$B/v1beta","apiKey":"gk-test"},
   "resp":{"api":"openai-responses","baseUrl":"$B/v1","apiKey":"rk"},
   "cc":{"baseUrl":"$B/v1","apiKey":"ck"}}},
 "agents":{"defaults":{"model":{"primary":"cc/m"}},
   "list":[{"id":"main"},{"id":"a1","model":"anth/c"},{"id":"a2","model":"gem/g"},{"id":"a3","model":"resp/r"}]}}
JSON
PORT=$(freeport)
OPENCLAW_CONFIG_PATH="$T/cfg.json" OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_DATA_DIR="$T/d" OPENCLAW_STATE_DIR="$T/st" HOME="$T" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
ask() { curl -s -m 60 "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' \
  -d "{\"id\":1,\"method\":\"chat.send\",\"params\":{\"agentId\":\"$1\",\"sessionKey\":\"agent:$1:img\",\"message\":\"look\"}}" > "$T/r.json"
  grep -q "$2" "$T/r.json" || fail "$1: expected $2: $(head -c 400 $T/r.json)"; }
ask a1 anthropic-final
ask a2 gemini-final
ask a3 responses-final
ask main chat-final
python3 - "$T/llm.log" <<'PY' || fail "wire format"
import json,sys
L=[json.loads(l) for l in open(sys.argv[1])]
def last(pred): return [x for x in L if pred(x["path"])][-1]["body"]
a=last(lambda p:p.endswith("/v1/messages"))
blocks=[c for m in a["messages"] if isinstance(m["content"],list) for c in m["content"]]
tr=[c for c in blocks if c.get("type")=="tool_result"][0]
assert "dataUrl" not in json.dumps(tr) and "imageAttached" in json.dumps(tr), tr
img=[c for c in blocks if c.get("type")=="image"]
assert img and img[0]["source"]["type"]=="base64" and img[0]["source"]["media_type"]=="image/png" and img[0]["source"]["data"].startswith("iVBOR"), blocks
assert any(c.get("type")=="text" and "what colour" in c.get("text","") for c in blocks)
g=last(lambda p:":generateContent" in p)
inl=[p["inlineData"] for c in g["contents"] for p in c["parts"] if "inlineData" in p]
assert inl and inl[0]["mimeType"]=="image/png" and inl[0]["data"].startswith("iVBOR"), g["contents"]
r=last(lambda p:p.endswith("/responses"))
ii=[c for i in r["input"] if isinstance(i.get("content"),list) for c in i["content"] if c.get("type")=="input_image"]
assert ii and ii[0]["image_url"].startswith("data:image/png;base64,"), r["input"]
c=last(lambda p:p.endswith("/chat/completions"))
tm=[m for m in c["messages"] if m["role"]=="tool"][0]
assert "dataUrl" not in tm["content"], tm
um=[m for m in c["messages"] if m["role"]=="user" and isinstance(m["content"],list)]
assert um and any(p.get("type")=="image_url" and p["image_url"]["url"].startswith("data:image/png") for p in um[-1]["content"]), c["messages"]
PY
echo "[e2e-view-image] OK"
