#!/usr/bin/env python3
"""Offline fake GitHub REST API for e2e tests: /user, /repos/{o}/{r}, /search/repositories, /user/repos,
/repos/{o}/{r}/pulls (GET list by head, POST create). Token 'good-token' is valid; others get 401 on authed reads."""
import json, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

TOKEN = "good-token"
PERSONAL = {"alice-token": {"id": 77, "login": "alice"}}   # issued by the fake device flow
DEVICE_POLLS = {}
REPOS = {
    "acme/widgets": {"description": "Widget toolkit", "private": False},
    "acme/gadgets": {"description": "widgets companion gadgets", "private": False},
    "octo/secret-widgets": {"description": "private widgets", "private": True},
    "octo/project": {"description": "the publish target", "private": False},
}
PULLS = []
LOG = []

def repo_json(full):
    o, n = full.split("/")
    meta = REPOS[full]
    return {"full_name": full, "name": n, "owner": {"login": o}, "clone_url": f"https://github.com/{full}.git",
            "html_url": f"https://github.com/{full}", "description": meta["description"], "private": meta["private"],
            "default_branch": "main"}

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, obj):
        b = json.dumps(obj).encode()
        self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def authed(self):
        a = self.headers.get("Authorization", "")
        return a == f"Bearer {TOKEN}" or any(a == f"Bearer {k}" for k in PERSONAL)
    def do_GET(self):
        u = urlparse(self.path); q = parse_qs(u.query); p = u.path
        LOG.append(("GET", self.path, self.headers.get("Authorization", "")))
        if p == "/_log": return self.send(200, {"log": LOG, "pulls": PULLS})
        if p == "/user":
            a = self.headers.get("Authorization", "")
            for k, v in PERSONAL.items():
                if a == f"Bearer {k}": return self.send(200, v)
            return self.send(200, {"id": 4242, "login": "octo"}) if self.authed() else self.send(401, {"message": "Bad credentials"})
        if p == "/user/repos":
            if not self.authed(): return self.send(401, {"message": "Bad credentials"})
            return self.send(200, [repo_json("octo/secret-widgets"), repo_json("octo/project")])
        if p == "/search/repositories":
            term = q.get("q", [""])[0].split(" in:")[0].lower()
            items = [repo_json(k) for k, v in REPOS.items() if not v["private"] and (term in k.lower() or term in v["description"].lower())]
            return self.send(200, {"total_count": len(items), "items": items})
        parts = p.strip("/").split("/")
        if len(parts) == 3 and parts[0] == "repos":
            full = f"{parts[1]}/{parts[2]}"
            return self.send(200, repo_json(full)) if full in REPOS else self.send(404, {"message": "Not Found"})
        if len(parts) == 5 and parts[0] == "repos" and parts[3] in ("pulls", "issues") and parts[4].isdigit():
            num = int(parts[4]); full = f"{parts[1]}/{parts[2]}"
            if full not in REPOS: return self.send(404, {"message": "Not Found"})
            j = {"number": num, "title": f"Fake {parts[3][:-1]} #{num}", "state": "open", "user": {"login": "octo"}, "body": "fake body",
                 "labels": [{"name": "bug"}], "html_url": f"https://github.com/{full}/{'pull' if parts[3] == 'pulls' else 'issues'}/{num}"}
            if parts[3] == "pulls": j.update({"head": {"ref": "feat"}, "base": {"ref": "main"}, "merged": False, "draft": False})
            return self.send(200, j)
        if len(parts) == 6 and parts[0] == "repos" and parts[3] == "commits" and parts[5] == "check-runs":
            return self.send(200, {"total_count": 2, "check_runs": [{"name": "build", "status": "completed", "conclusion": "success"}, {"name": "test", "status": "in_progress", "conclusion": None}]})
        if len(parts) == 4 and parts[0] == "repos" and parts[3] == "pulls":
            head = q.get("head", [""])[0]
            return self.send(200, [pr for pr in PULLS if f"{pr['owner']}:{pr['head']}" == head and pr["state"] == "open"])
        self.send(404, {"message": "Not Found"})
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0")); raw = self.rfile.read(n) or b"{}"
        LOG.append(("POST", self.path, self.headers.get("Authorization", "")))
        if self.path == "/login/device/code":
            form = parse_qs(raw.decode())
            if form.get("client_id") != ["oc-client"]: return self.send(200, {"error": "unauthorized_client"})
            return self.send(200, {"device_code": "dc-1", "user_code": "WDJB-MJHT", "verification_uri": "https://github.com/login/device", "expires_in": 900, "interval": 1})
        if self.path == "/login/oauth/access_token":
            form = parse_qs(raw.decode()); dc = form.get("device_code", [""])[0]
            DEVICE_POLLS[dc] = DEVICE_POLLS.get(dc, 0) + 1
            if dc != "dc-1": return self.send(200, {"error": "incorrect_device_code"})
            if DEVICE_POLLS[dc] == 1: return self.send(200, {"error": "authorization_pending"})
            return self.send(200, {"access_token": "alice-token", "token_type": "bearer", "scope": "repo,workflow"})
        body = json.loads(raw)
        parts = urlparse(self.path).path.strip("/").split("/")
        if len(parts) == 4 and parts[0] == "repos" and parts[3] == "pulls":
            if not self.authed(): return self.send(401, {"message": "Bad credentials"})
            num = len(PULLS) + 1
            pr = {"number": num, "state": "open", "owner": parts[1], "head": body["head"], "base": body["base"], "title": body["title"],
                  "body": body.get("body", ""), "html_url": f"https://github.com/{parts[1]}/{parts[2]}/pull/{num}"}
            PULLS.append(pr)
            return self.send(201, pr)
        self.send(404, {"message": "Not Found"})

srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
print(srv.server_address[1], flush=True)
srv.serve_forever()
