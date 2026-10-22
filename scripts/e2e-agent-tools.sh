#!/usr/bin/env bash
# 1.128: web_search (Brave/Tavily fakes), view_image, agents_wait, ask_user (question.* RPC + channel answer),
# conversations_list/send/turn over the offline Telegram fake.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${OPENCLAW_BIN:-$ROOT/build/build/Release/openclaw-cpp}"
T=$(mktemp -d); GW=""; FT=""; FS=""
trap 'for p in $GW $FT $FS; do kill $p 2>/dev/null; done; wait 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "[e2e-agent-tools] FAIL: $*"; [[ -f "$T/gw.log" ]] && tail -8 "$T/gw.log"; exit 1; }
freeport() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
unset OPENCLAW_GATEWAY_URL OPENCLAW_GATEWAY_TOKEN TELEGRAM_BOT_TOKEN TELEGRAM_API_BASE TELEGRAM_MODE TELEGRAM_WEBHOOK_URL BRAVE_API_KEY TAVILY_API_KEY
FP=$(freeport); python3 "$ROOT/scripts/fake_telegram.py" "$FP" "$T/tg.log" & FT=$!
SP=$(freeport); python3 "$ROOT/scripts/fake_search.py" "$SP" & FS=$!
for i in $(seq 1 50); do curl -s "http://127.0.0.1:$FP/" >/dev/null 2>&1 && curl -s "http://127.0.0.1:$SP/" >/dev/null 2>&1 && break; sleep 0.1; done
echo '[{"content":"welcome"},{"content":"child done"},{"content":"should-not-run"}]' > "$T/script.json"
echo "{\"channels\":{\"telegram\":{\"botToken\":\"TT-1:abc\",\"apiBase\":\"http://127.0.0.1:$FP\",\"dmPolicy\":\"open\"}}}" > "$T/cfg.json"
PORT=$(freeport)
BRAVE_API_KEY=brave-key OPENCLAW_BRAVE_BASE_URL="http://127.0.0.1:$SP" TAVILY_API_KEY=tav-key OPENCLAW_TAVILY_BASE_URL="http://127.0.0.1:$SP" \
OPENCLAW_DEFAULT_PROVIDER=mock OPENCLAW_MOCK_SCRIPT="$T/script.json" OPENCLAW_TELEGRAM_POLL_TIMEOUT=1 OPENCLAW_CONFIG_PATH="$T/cfg.json" \
OPENCLAW_SESSION_DIR="$T/s" OPENCLAW_DATA_DIR="$T/d" OPENCLAW_STATE_DIR="$T/st" HOME="$T" OPENCLAW_GATEWAY_PORT=$PORT "$BIN" gateway >>"$T/gw.log" 2>&1 & GW=$!
for i in $(seq 1 50); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.2; done
rpc() { curl -s -m 60 "http://127.0.0.1:$PORT/v1/rpc" -H 'Content-Type: application/json' -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}"; }
tool() { rpc tools.invoke "{\"name\":\"$1\",\"args\":$2${3:+,\"sessionKey\":\"$3\"}}"; }
J() { python3 -c "import json,sys;d=json.loads(sys.stdin.read());r=d.get('result',d);r=r.get('payload',r);r=r.get('result',r) if isinstance(r,dict) else r;$1"; }
push() { curl -s -X POST "http://127.0.0.1:$FP/__push" -d "$1" >/dev/null; }
sent() { python3 - "$T/tg.log" "$1" <<'PY'
import json,sys
for l in open(sys.argv[1]):
    e=json.loads(l)
    if e["method"]=="sendMessage" and eval(sys.argv[2]): sys.exit(0)
sys.exit(1)
PY
}
waitsent() { for i in $(seq 1 100); do sent "$1" && return 0; sleep 0.1; done; fail "no sendMessage matching $1"; }

# web_search
tool web_search '{"query":"openclaw","count":3}' | J 'assert r["provider"]=="brave" and len(r["results"])==3 and r["results"][0]["title"]=="Brave 0 openclaw", r' || fail "brave search"
tool web_search '{"query":"claw","provider":"tavily"}' | J 'assert r["provider"]=="tavily" and r["results"][0]["snippet"]=="tav snippet", r' || fail "tavily search"
tool web_search '{}' | J 'assert r["ok"]==False and "query" in r["error"], r' || fail "search needs query"

# view_image
python3 - "$T/img.png" <<'PY'
import struct,zlib,sys
def ch(t,d): return struct.pack(">I",len(d))+t+d+struct.pack(">I",zlib.crc32(t+d)&0xffffffff)
raw=b"".join(b"\0"+b"\xff\0\0"*3 for _ in range(2))
open(sys.argv[1],"wb").write(b"\x89PNG\r\n\x1a\n"+ch(b"IHDR",struct.pack(">IIBBBBB",3,2,8,2,0,0,0))+ch(b"IDAT",zlib.compress(raw))+ch(b"IEND",b""))
PY
tool view_image "{\"image\":\"$T/img.png\"}" | J 'assert r["mimeType"]=="image/png" and r["width"]==3 and r["height"]==2 and r["dataUrl"].startswith("data:image/png;base64,iVBOR"), r' || fail "view_image png"
tool view_image '{"image":"/nonexistent.png"}' | J 'assert r["ok"]==False, r' || fail "view_image missing"

# conversations: an inbound Telegram DM creates the conversationRef
push '{"message":{"message_id":1,"chat":{"id":777,"type":"private"},"from":{"id":777,"first_name":"A"},"text":"hi"}}'
waitsent 'e["args"]["chat_id"]=="777" and e["args"]["text"]=="welcome"'
REF=$(tool conversations_list '{"channel":"telegram"}' | J 'c=r["conversations"];assert len(c)==1 and c[0]["to"]=="777", r;print(c[0]["conversationRef"])') || fail "conversations_list"
[[ "$REF" =~ ^conv_[0-9a-f]{32}$ ]] || fail "ref format $REF"
SK=$(tool conversations_list '{}' | J 'print(r["conversations"][0]["sessionKey"])')
tool conversations_send "{\"conversationRef\":\"$REF\",\"message\":\"direct note\"}" | J 'assert r["status"]=="sent", r' || fail "conversations_send"
waitsent 'e["args"]["chat_id"]=="777" and e["args"]["text"]=="direct note"'
tool conversations_send '{"conversationRef":"conv_00000000000000000000000000000000","message":"x"}' | J 'assert r["ok"]==False and "unknown" in r["error"], r' || fail "unknown ref"
tool conversations_turn "{\"conversationRef\":\"$REF\",\"message\":\"ping?\",\"timeoutSeconds\":20}" > "$T/turn.json" &
waitsent 'e["args"]["chat_id"]=="777" and e["args"]["text"]=="ping?"'
push '{"message":{"message_id":2,"chat":{"id":777,"type":"private"},"from":{"id":777},"text":"pong!"}}'
wait $!; J 'assert r["status"]=="replied" and r["reply"]["text"]=="pong!", r' < "$T/turn.json" || fail "conversations_turn: $(cat $T/turn.json)"

# ask_user answered over RPC (option by number), then answered by the next channel message
tool ask_user '{"question":"Pick one","options":["red","blue"],"timeoutSeconds":20}' "$SK" > "$T/ask1.json" &
A1=$!
waitsent 'e["args"]["chat_id"]=="777" and "Pick one" in e["args"]["text"] and "2. blue" in e["args"]["text"]'
QID=$(rpc question.list '{"status":"pending"}' | J 'q=r["questions"];assert len(q)==1, r;print(q[0]["id"])') || fail "question.list"
rpc question.resolve "{\"id\":\"$QID\",\"answers\":{\"q1\":\"2\"}}" | J 'assert r["status"]=="answered", r' || fail "question.resolve"
wait $A1; J 'assert r["status"]=="answered" and r["answers"][0]["answer"]=="blue" and r["mirroredToChannel"], r' < "$T/ask1.json" || fail "ask_user rpc answer: $(cat $T/ask1.json)"
rpc question.resolve "{\"id\":\"$QID\",\"answer\":\"x\"}" | J 'assert "error" in json.dumps(d), d' || fail "double resolve rejected"
tool ask_user '{"question":"Your name?","timeoutSeconds":20}' "$SK" > "$T/ask2.json" &
A2=$!
waitsent '"Your name?" in e["args"]["text"]'
push '{"message":{"message_id":3,"chat":{"id":777,"type":"private"},"from":{"id":777},"text":"Ada"}}'
wait $A2; J 'assert r["status"]=="answered" and r["answers"][0]["answer"]=="Ada", r' < "$T/ask2.json" || fail "ask_user channel answer: $(cat $T/ask2.json)"
tool ask_user '{"question":"Timeout?","timeoutSeconds":1}' "agent:main:nochannel" | J 'assert r["status"]=="timeout", r' || fail "ask_user timeout"
sleep 0.5; sent '"should-not-run" in e["args"]["text"]' && fail "turn/question replies must not start an agent turn"

# agents_wait on a spawned sub-agent and an unknown id
RUN=$(tool sessions_spawn '{"task":"do it","label":"kid"}' "$SK" | J 'assert r["status"]=="done", r;print(r["runId"])') || fail "spawn"
tool agents_wait "{\"ids\":[\"$RUN\",\"nope\"],\"timeoutSeconds\":1}" "$SK" | J 'assert r["completed"][0]["status"]=="done" and r["completed"][0]["result"]=="child done" and r["errors"][0]["error"]=="not_found", r' || fail "agents_wait"
echo "[e2e-agent-tools] OK"
