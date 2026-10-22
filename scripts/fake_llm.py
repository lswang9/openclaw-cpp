#!/usr/bin/env python3
"""Offline fake LLM endpoints for native provider tests. Usage: fake_llm.py <port> <logfile>
First request per API asks for a `time_now` tool call; once a tool result is present it answers with text."""
import json, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
PORT, LOG = int(sys.argv[1]), sys.argv[2]
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, obj):
        b = json.dumps(obj).encode(); self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def sse(self, events):
        self.send_response(200); self.send_header("Content-Type", "text/event-stream"); self.end_headers()
        for ev, data in events:
            self.wfile.write(((f"event: {ev}\n" if ev else "") + "data: " + json.dumps(data) + "\n\n").encode()); self.wfile.flush()
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
        with open(LOG, "a") as f: f.write(json.dumps({"path": self.path, "headers": {k.lower(): v for k, v in self.headers.items()}, "body": body}) + "\n")
        p = self.path
        if p.endswith("/v1/messages"):
            if self.headers.get("x-api-key") != "ak-test": return self.send(401, {"type": "error", "error": {"type": "authentication_error", "message": "bad key"}})
            done = any(isinstance(m["content"], list) and any(c.get("type") == "tool_result" for c in m["content"]) for m in body["messages"])
            if body.get("stream"):
                if not done: return self.sse([("message_start", {"type": "message_start"}), ("content_block_start", {"type": "content_block_start", "index": 0, "content_block": {"type": "tool_use", "id": "tu_1", "name": "time_now", "input": {}}}),
                    ("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "input_json_delta", "partial_json": "{"}}), ("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "input_json_delta", "partial_json": "}"}}),
                    ("message_delta", {"type": "message_delta", "delta": {"stop_reason": "tool_use"}}), ("message_stop", {"type": "message_stop"})])
                return self.sse([("content_block_start", {"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}}), ("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "anthropic-"}}),
                    ("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "streamed"}}), ("message_delta", {"type": "message_delta", "delta": {"stop_reason": "end_turn"}})])
            if not done: return self.send(200, {"content": [{"type": "text", "text": "checking"}, {"type": "tool_use", "id": "tu_1", "name": "time_now", "input": {}}], "stop_reason": "tool_use"})
            return self.send(200, {"content": [{"type": "text", "text": "anthropic-final"}], "stop_reason": "end_turn"})
        if ":streamGenerateContent" in p:
            done = any("functionResponse" in part for c in body["contents"] for part in c["parts"])
            if not done: return self.sse([(None, {"candidates": [{"content": {"role": "model", "parts": [{"functionCall": {"name": "time_now", "args": {}}}]}}]})])
            return self.sse([(None, {"candidates": [{"content": {"role": "model", "parts": [{"text": "gemini-"}]}}]}), (None, {"candidates": [{"content": {"role": "model", "parts": [{"text": "streamed"}]}, "finishReason": "STOP"}]})])
        if ":generateContent" in p:
            if self.headers.get("x-goog-api-key") != "gk-test": return self.send(403, {"error": {"message": "bad key"}})
            done = any("functionResponse" in part for c in body["contents"] for part in c["parts"])
            if not done: return self.send(200, {"candidates": [{"content": {"role": "model", "parts": [{"functionCall": {"name": "time_now", "args": {}}}]}, "finishReason": "STOP"}]})
            return self.send(200, {"candidates": [{"content": {"role": "model", "parts": [{"text": "gemini-final"}]}, "finishReason": "STOP"}]})
        if p.endswith("/responses"):
            done = any(i.get("type") == "function_call_output" for i in body["input"])
            if body.get("stream"):
                if not done: return self.sse([("response.output_item.done", {"type": "response.output_item.done", "item": {"type": "function_call", "call_id": "fc_1", "name": "time_now", "arguments": "{}"}}), ("response.completed", {"type": "response.completed"})])
                return self.sse([("response.output_text.delta", {"type": "response.output_text.delta", "delta": "responses-"}), ("response.output_text.delta", {"type": "response.output_text.delta", "delta": "streamed"}), ("response.completed", {"type": "response.completed"})])
            if not done: return self.send(200, {"status": "completed", "output": [{"type": "function_call", "call_id": "fc_1", "name": "time_now", "arguments": "{}"}]})
            return self.send(200, {"status": "completed", "output": [{"type": "message", "content": [{"type": "output_text", "text": "responses-final"}]}]})
        if p.endswith("/chat/completions"):
            done = any(m.get("role") == "tool" for m in body["messages"])
            if body.get("stream"):
                if not done: return self.sse([(None, {"choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "c1", "type": "function", "function": {"name": "time_now", "arguments": "{}"}}]}}]}), (None, {"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]})])
                return self.sse([(None, {"choices": [{"index": 0, "delta": {"content": "chat-"}}]}), (None, {"choices": [{"index": 0, "delta": {"content": "streamed:" + body["model"]}, "finish_reason": "stop"}]})])
            if not done: return self.send(200, {"choices": [{"finish_reason": "tool_calls", "message": {"role": "assistant", "content": "", "tool_calls": [{"id": "c1", "type": "function", "function": {"name": "time_now", "arguments": "{}"}}]}}]})
            return self.send(200, {"choices": [{"finish_reason": "stop", "message": {"role": "assistant", "content": "chat-final:" + body["model"]}}]})
        self.send(404, {"error": {"message": "no route " + p}})
ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
