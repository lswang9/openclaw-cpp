#!/usr/bin/env python3
"""Offline fake ClawHub registry (subset of /api/v1) for e2e tests. Usage: fake_clawhub.py <port> <datadir>"""
import hashlib, io, json, os, sys, tarfile, zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs, unquote

PORT, DATA = int(sys.argv[1]), sys.argv[2]
STATE = {"skill_version": "1.0.0", "hits429": 0}

def skill_zip(version):
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w") as z:
        z.writestr("SKILL.md", f"---\nname: demo\ndescription: Demo skill v{version}\n---\nSay hello ({version}).\n")
        z.writestr("scripts/run.sh", "echo demo\n")
    return b.getvalue()

def skill_fp(version):
    files = {"SKILL.md": f"---\nname: demo\ndescription: Demo skill v{version}\n---\nSay hello ({version}).\n".encode(),
             "scripts/run.sh": b"echo demo\n"}
    acc = "".join(f"{p}:{hashlib.sha256(files[p]).hexdigest()}\n" for p in sorted(files))
    return hashlib.sha256(acc.encode()).hexdigest()

def plugin_tgz():
    b = io.BytesIO()
    with tarfile.open(fileobj=b, mode="w:gz") as t:
        for name, body in {
            "package/openclaw.plugin.json": json.dumps({"id": "hubplug", "name": "Hub plugin"}),
            "package/package.json": json.dumps({"name": "@acme/hubplug", "version": "2.1.0", "type": "module", "openclaw": {"extensions": ["./index.mjs"]}}),
            "package/index.mjs": "export default function register(api){ api.registerCommand({name:'hubping', handler:()=>({text:'hub-pong'})}); }\n",
        }.items():
            data = body.encode(); ti = tarfile.TarInfo(name); ti.size = len(data); t.addfile(ti, io.BytesIO(data))
    return b.getvalue()

TGZ = plugin_tgz()

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, body, ctype="application/json", headers=None):
        if isinstance(body, (dict, list)): body = json.dumps(body)
        if isinstance(body, str): body = body.encode()
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items(): self.send_header(k, v)
        self.end_headers(); self.wfile.write(body)
    def do_POST(self):
        if self.path == "/__bump":
            STATE["skill_version"] = "1.1.0"; return self.send(200, {"ok": True})
        self.send(404, "not found", "text/plain")
    def do_GET(self):
        u = urlparse(self.path); q = {k: v[0] for k, v in parse_qs(u.query).items()}; p = unquote(u.path)
        with open(os.path.join(DATA, "requests.log"), "a") as f: f.write(self.path + "\n")
        v = STATE["skill_version"]
        if p == "/api/v1/search":
            if STATE["hits429"] == 0:
                STATE["hits429"] = 1
                return self.send(429, "Rate limit exceeded", "text/plain", {"Retry-After": "1"})
            return self.send(200, {"results": [{"slug": "demo", "displayName": "Demo", "summary": "demo skill", "version": v, "score": 1}]})
        if p == "/api/v1/skills/demo":
            return self.send(200, {"skill": {"slug": "demo", "displayName": "Demo"}, "latestVersion": {"version": v}, "owner": {"handle": "openclaw"}})
        if p == "/api/v1/skills/evil":
            return self.send(200, {"skill": {"slug": "evil"}, "latestVersion": {"version": "0.1.0"}, "moderation": {"isMalwareBlocked": True}})
        if p == "/api/v1/download" and q.get("slug") == "demo":
            return self.send(200, skill_zip(q.get("version", v)), "application/zip")
        if p == "/api/v1/resolve" and q.get("slug") == "demo":
            match = next((ver for ver in ("1.0.0", "1.1.0") if skill_fp(ver) == q.get("hash")), None)
            return self.send(200, {"slug": "demo", "match": {"version": match} if match else None, "latestVersion": {"version": v}})
        if p == "/api/v1/resolve":
            return self.send(404, "Skill not found", "text/plain")
        if p == "/api/v1/packages/@acme/hubplug":
            return self.send(200, {"package": {"name": "@acme/hubplug", "latestVersion": "2.1.0", "family": "code-plugin"}})
        if p == "/api/v1/packages/@acme/hubplug/versions/2.1.0/security":
            return self.send(200, {"trust": {"blockedFromDownload": False, "reasons": []}, "release": {"artifactSha256": hashlib.sha256(TGZ).hexdigest()}})
        if p == "/api/v1/packages/@acme/hubplug/versions/2.1.0/artifact":
            return self.send(200, {"artifact": {"kind": "npm-pack", "sha256": hashlib.sha256(TGZ).hexdigest()}})
        if p == "/api/v1/packages/@acme/hubplug/versions/2.1.0/artifact/download":
            return self.send(200, TGZ, "application/octet-stream", {"X-ClawHub-Artifact-Sha256": hashlib.sha256(TGZ).hexdigest()})
        if p == "/api/v1/packages/@acme/bad":
            return self.send(200, {"package": {"name": "@acme/bad", "latestVersion": "1.0.0"}})
        if p == "/api/v1/packages/@acme/bad/versions/1.0.0/security":
            return self.send(200, {"trust": {"blockedFromDownload": True, "reasons": ["scan:malicious"]}})
        if p == "/api/v1/packages/@acme/tampered":
            return self.send(200, {"package": {"name": "@acme/tampered", "latestVersion": "1.0.0"}})
        if p == "/api/v1/packages/@acme/tampered/versions/1.0.0/security":
            return self.send(200, {"trust": {"blockedFromDownload": False}})
        if p == "/api/v1/packages/@acme/tampered/versions/1.0.0/artifact":
            return self.send(200, {"artifact": {"kind": "npm-pack", "sha256": "0" * 64}})
        if p == "/api/v1/packages/@acme/tampered/versions/1.0.0/artifact/download":
            return self.send(200, TGZ, "application/octet-stream")
        if p == "/api/v1/plugins/search":
            return self.send(200, {"results": [{"name": "@acme/hubplug"}]})
        self.send(404, "not found", "text/plain")

ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
