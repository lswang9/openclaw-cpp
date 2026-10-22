#!/usr/bin/env python3
"""Offline fake for Bedrock Converse + Google OAuth token + Vertex Anthropic/Gemini.
Usage: fake_cloud_llm.py <port> <logfile>
Accepts AWS SigV4 (any Credential=AKIA... with X-Amz-Date) or Bearer bedrock-tok;
Google token endpoint issues access_token for refresh_token=rt-test or JWT assertion;
Vertex Anthropic rawPredict / Gemini generateContent require Bearer ya29.test."""
import hashlib, hmac, json, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
PORT, LOG = int(sys.argv[1]), sys.argv[2]
TOOL = "time_now"

def has_tool_result_bedrock(body):
    for m in body.get("messages", []):
        for c in m.get("content", []):
            if "toolResult" in c: return True
    return False

def has_tool_result_anth(body):
    for m in body.get("messages", []):
        if isinstance(m.get("content"), list) and any(c.get("type") == "tool_result" for c in m["content"]): return True
    return False

def has_tool_result_gem(body):
    for c in body.get("contents", []):
        for p in c.get("parts", []):
            if "functionResponse" in p: return True
    return False

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, obj):
        b = json.dumps(obj).encode(); self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def sse(self, events):
        self.send_response(200); self.send_header("Content-Type", "text/event-stream"); self.end_headers()
        for ev, data in events:
            self.wfile.write(((f"event: {ev}\n" if ev else "") + "data: " + json.dumps(data) + "\n\n").encode()); self.wfile.flush()
    def auth_aws(self):
        a = self.headers.get("Authorization", "")
        if a == "Bearer bedrock-tok": return True
        if a.startswith("AWS4-HMAC-SHA256 Credential="):
            return "X-Amz-Date" in self.headers or "x-amz-date" in {k.lower() for k in self.headers.keys()}
        return False
    def auth_google(self):
        a = self.headers.get("Authorization", "")
        return a == "Bearer ya29.test"
    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0)) or 0)
        body = {}
        ctype = self.headers.get("Content-Type", "")
        if "application/x-www-form-urlencoded" in ctype:
            from urllib.parse import parse_qs
            form = {k: v[0] for k, v in parse_qs(raw.decode()).items()}
            with open(LOG, "a") as f: f.write(json.dumps({"path": self.path, "headers": {k.lower(): v for k, v in self.headers.items()}, "form": form}) + "\n")
            # OAuth token
            if self.path.endswith("/token") or self.path == "/oauth2/v4/token" or "oauth2.googleapis.com" in self.path or self.path == "/token":
                if form.get("grant_type") == "refresh_token" and form.get("refresh_token") == "rt-test":
                    return self.send(200, {"access_token": "ya29.test", "expires_in": 3600, "token_type": "Bearer"})
                if form.get("grant_type") == "urn:ietf:params:oauth:grant-type:jwt-bearer" and form.get("assertion"):
                    return self.send(200, {"access_token": "ya29.test", "expires_in": 3600, "token_type": "Bearer"})
                return self.send(400, {"error": "invalid_grant"})
            return self.send(404, {"error": "no form route"})
        try: body = json.loads(raw or b"{}")
        except Exception: body = {}
        with open(LOG, "a") as f: f.write(json.dumps({"path": self.path, "headers": {k.lower(): v for k, v in self.headers.items()}, "body": body}) + "\n")
        p = self.path
        # Bedrock Converse
        if "/model/" in p and p.endswith("/converse"):
            if not self.auth_aws(): return self.send(403, {"message": "Missing Authentication Token"})
            if not has_tool_result_bedrock(body):
                return self.send(200, {"output": {"message": {"role": "assistant", "content": [{"text": "checking"}, {"toolUse": {"toolUseId": "tu_b1", "name": TOOL, "input": {}}}]}}, "stopReason": "tool_use"})
            return self.send(200, {"output": {"message": {"role": "assistant", "content": [{"text": "bedrock-final"}]}}, "stopReason": "end_turn"})
        if "/model/" in p and p.endswith("/converse-stream"):
            if not self.auth_aws(): return self.send(403, {"message": "Missing Authentication Token"})
            if not has_tool_result_bedrock(body):
                return self.sse([(None, {"contentBlockStart": {"start": {"toolUse": {"toolUseId": "tu_b1", "name": TOOL}}}}),
                                 (None, {"contentBlockDelta": {"delta": {"toolUse": {"input": "{}"}}}}),
                                 (None, {"messageStop": {"stopReason": "tool_use"}})])
            return self.sse([(None, {"contentBlockDelta": {"delta": {"text": "bedrock-"}}}),
                             (None, {"contentBlockDelta": {"delta": {"text": "streamed"}}}),
                             (None, {"messageStop": {"stopReason": "end_turn"}})])
        # Vertex Anthropic
        if ":rawPredict" in p or ":streamRawPredict" in p:
            if not self.auth_google(): return self.send(401, {"error": {"message": "UNAUTHENTICATED"}})
            if "anthropic_version" not in body: return self.send(400, {"error": {"message": "missing anthropic_version"}})
            done = has_tool_result_anth(body)
            if ":streamRawPredict" in p:
                if not done: return self.sse([("content_block_start", {"type": "content_block_start", "index": 0, "content_block": {"type": "tool_use", "id": "tu_v1", "name": TOOL}}),
                    ("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "input_json_delta", "partial_json": "{}"}}),
                    ("message_delta", {"type": "message_delta", "delta": {"stop_reason": "tool_use"}})])
                return self.sse([("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "vertex-anth-"}}),
                    ("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "streamed"}})])
            if not done: return self.send(200, {"content": [{"type": "tool_use", "id": "tu_v1", "name": TOOL, "input": {}}], "stop_reason": "tool_use"})
            return self.send(200, {"content": [{"type": "text", "text": "vertex-anth-final"}], "stop_reason": "end_turn"})
        # Vertex Gemini
        if ":generateContent" in p or ":streamGenerateContent" in p:
            if not self.auth_google(): return self.send(401, {"error": {"message": "UNAUTHENTICATED"}})
            done = has_tool_result_gem(body)
            if ":streamGenerateContent" in p:
                if not done: return self.sse([(None, {"candidates": [{"content": {"role": "model", "parts": [{"functionCall": {"name": TOOL, "args": {}}}]}}]})])
                return self.sse([(None, {"candidates": [{"content": {"role": "model", "parts": [{"text": "vertex-gem-"}]}}]}),
                                 (None, {"candidates": [{"content": {"role": "model", "parts": [{"text": "streamed"}]}, "finishReason": "STOP"}]})])
            if not done: return self.send(200, {"candidates": [{"content": {"role": "model", "parts": [{"functionCall": {"name": TOOL, "args": {}}}]}, "finishReason": "STOP"}]})
            return self.send(200, {"candidates": [{"content": {"role": "model", "parts": [{"text": "vertex-gem-final"}]}, "finishReason": "STOP"}]})
        self.send(404, {"error": {"message": "no route " + p}})
ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
