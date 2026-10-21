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
AUTH = os.environ.get("FAKE_ACP_AUTH") == "1"
authed = False
sessions = {}   # sid -> turn count (persisted to FAKE_ACP_STATE so session/load works across processes)
STATE = os.environ.get("FAKE_ACP_STATE")
if STATE and os.path.exists(STATE): sessions = json.load(open(STATE))
def save():
    if STATE: json.dump(sessions, open(STATE, "w"))
for line in sys.stdin:
    m = json.loads(line); mid = m.get("id"); meth = m.get("method"); p = m.get("params", {})
    if meth == "initialize":
        assert p["protocolVersion"] == 1 and p["clientCapabilities"]["fs"]["readTextFile"]
        out({"jsonrpc": "2.0", "id": mid, "result": {"protocolVersion": 1, "agentInfo": {"name": "fake-acp", "version": "0"},
             "agentCapabilities": {"loadSession": True, "promptCapabilities": {"audio": os.environ.get("FAKE_ACP_AUDIO") == "1"}},
             "authMethods": [{"id": "fake-login", "name": "Fake login", "description": "test"}] if AUTH else []}})
        TERM = p["clientCapabilities"].get("terminal", False)
    elif meth == "authenticate":
        authed = p.get("methodId") == "fake-login"
        out({"jsonrpc": "2.0", "id": mid, **({"result": {}} if authed else {"error": {"code": -32602, "message": "bad method"}})})
    elif meth in ("session/new", "session/load") and AUTH and not authed:
        out({"jsonrpc": "2.0", "id": mid, "error": {"code": -32000, "message": "Authentication required"}})
    elif meth == "session/new":
        cwd = p["cwd"]; sid = "S-%d" % (len(sessions) + 1); sessions[sid] = 0; save()
        out({"jsonrpc": "2.0", "id": mid, "result": {"sessionId": sid}})
    elif meth == "session/load":
        cwd = p["cwd"]
        if p["sessionId"] not in sessions: out({"jsonrpc": "2.0", "id": mid, "error": {"code": -32002, "message": "no such session"}})
        else:
            upd(p["sessionId"], {"sessionUpdate": "user_message_chunk", "content": {"type": "text", "text": "(replayed history)"}})
            out({"jsonrpc": "2.0", "id": mid, "result": None})
    elif meth == "session/prompt" and p["prompt"][0].get("text", "").startswith("turn"):
        sid = p["sessionId"]; sessions[sid] += 1; save()
        upd(sid, {"sessionUpdate": "agent_message_chunk", "content": {"type": "text", "text": "%s#%d:%s" % (sid, sessions[sid], p["prompt"][0]["text"])}})
        out({"jsonrpc": "2.0", "id": mid, "result": {"stopReason": "end_turn"}})
    elif meth == "session/prompt" and p["prompt"][0].get("text", "") == "run tests":
        sid = p["sessionId"]
        if not TERM:
            upd(sid, {"sessionUpdate": "agent_message_chunk", "content": {"type": "text", "text": "no-terminal"}})
        else:
            c = ask("terminal/create", {"sessionId": sid, "command": "sh", "args": ["-c", "echo out-$FOO; pwd; echo err >&2; exit 3"],
                                        "env": [{"name": "FOO", "value": "bar"}], "outputByteLimit": 4096})
            tid = c["result"]["terminalId"]
            w = ask("terminal/wait_for_exit", {"sessionId": sid, "terminalId": tid})
            o = ask("terminal/output", {"sessionId": sid, "terminalId": tid})
            bad = ask("terminal/create", {"sessionId": sid, "command": "true", "cwd": "/"})
            s = ask("terminal/create", {"sessionId": sid, "command": "sleep", "args": ["30"]})
            k = ask("terminal/kill", {"sessionId": sid, "terminalId": s["result"]["terminalId"]})
            ko = ask("terminal/output", {"sessionId": sid, "terminalId": s["result"]["terminalId"]})
            ask("terminal/release", {"sessionId": sid, "terminalId": tid}); ask("terminal/release", {"sessionId": sid, "terminalId": s["result"]["terminalId"]})
            upd(sid, {"sessionUpdate": "agent_message_chunk", "content": {"type": "text", "text": json.dumps(
                {"exit": w["result"], "output": o["result"]["output"], "cwd_escape": "error" in bad, "killed": ko["result"]["exitStatus"]})}})
        out({"jsonrpc": "2.0", "id": mid, "result": {"stopReason": "end_turn"}})
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
