#!/usr/bin/env python3
"""Fake external ACP agent (stdio, ACP v1) for e2e-acpx.sh: streams chunks, reports a tool call, asks permission,
reads/writes files through the client (fs/*), and echoes audio blocks it receives (advertises audio when FAKE_ACP_AUDIO=1)."""
import json, os, sys
nid = 1000
def out(m): sys.stdout.write(json.dumps(m) + "\n"); sys.stdout.flush()
def ask(method, params):
    global nid; nid += 1; out({"jsonrpc": "2.0", "id": nid, "method": method, "params": params})
    while True:
        m = json.loads(sys.stdin.readline())
        if m.get("id") == nid: return m
def upd(sid, u): out({"jsonrpc": "2.0", "method": "session/update", "params": {"sessionId": sid, "update": u}})
print("fake agent booting (non-JSON log line on stdout)", flush=True)
cwd = None
for line in sys.stdin:
    m = json.loads(line); mid = m.get("id"); meth = m.get("method"); p = m.get("params", {})
    if meth == "initialize":
        assert p["protocolVersion"] == 1 and p["clientCapabilities"]["fs"]["readTextFile"]
        out({"jsonrpc": "2.0", "id": mid, "result": {"protocolVersion": 1, "agentInfo": {"name": "fake-acp", "version": "0"},
             "agentCapabilities": {"promptCapabilities": {"audio": os.environ.get("FAKE_ACP_AUDIO") == "1"}}, "authMethods": []}})
    elif meth == "session/new":
        cwd = p["cwd"]; out({"jsonrpc": "2.0", "id": mid, "result": {"sessionId": "S-1"}})
    elif meth == "session/prompt":
        sid = p["sessionId"]; blocks = p["prompt"]
        kinds = ",".join(b["type"] + (":" + b["text"] if b["type"] == "text" else ":" + b.get("mimeType", "")) for b in blocks)
        upd(sid, {"sessionUpdate": "agent_message_chunk", "content": {"type": "text", "text": "got[" + kinds + "] "}})
        upd(sid, {"sessionUpdate": "tool_call", "toolCallId": "t1", "title": "Edit notes.txt", "kind": "edit", "status": "pending"})
        perm = ask("session/request_permission", {"sessionId": sid, "toolCall": {"toolCallId": "t1", "title": "Edit notes.txt"},
                   "options": [{"optionId": "a1", "name": "Allow", "kind": "allow_once"}, {"optionId": "r1", "name": "Reject", "kind": "reject_once"}]})
        outcome = perm["result"]["outcome"]
        allowed = outcome.get("optionId") == "a1"
        rd = ask("fs/read_text_file", {"sessionId": sid, "path": os.path.join(cwd, "notes.txt")})
        esc = ask("fs/read_text_file", {"sessionId": sid, "path": "/etc/passwd"})
        msg = "read=" + (rd.get("result", {}).get("content", "ERR").strip()) + " escape=" + ("blocked" if "error" in esc else "LEAK")
        if allowed:
            wr = ask("fs/write_text_file", {"sessionId": sid, "path": os.path.join(cwd, "notes.txt"), "content": "edited by agent\n"})
            msg += " write=" + ("ok" if "error" not in wr else wr["error"]["message"])
        upd(sid, {"sessionUpdate": "tool_call_update", "toolCallId": "t1", "status": "completed" if allowed else "failed"})
        upd(sid, {"sessionUpdate": "agent_message_chunk", "content": {"type": "text", "text": msg}})
        out({"jsonrpc": "2.0", "id": mid, "result": {"stopReason": "end_turn"}})
