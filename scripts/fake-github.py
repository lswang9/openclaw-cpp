#!/usr/bin/env python3
"""Offline fake GitHub REST API for e2e tests: /user, /repos/{o}/{r}, /search/repositories, /user/repos,
/repos/{o}/{r}/pulls (GET list by head, POST create). Token 'good-token' is valid; others get 401 on authed reads."""
import json, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

TOKEN = "good-token"
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
        return self.headers.get("Authorization", "") == f"Bearer {TOKEN}"
    def do_GET(self):
        u = urlparse(self.path); q = parse_qs(u.query); p = u.path
        LOG.append(("GET", self.path, self.headers.get("Authorization", "")))
        if p == "/_log": return self.send(200, {"log": LOG, "pulls": PULLS})
        if p == "/user":
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
        if len(parts) == 4 and parts[0] == "repos" and parts[3] == "pulls":
            head = q.get("head", [""])[0]
            return self.send(200, [pr for pr in PULLS if f"{pr['owner']}:{pr['head']}" == head and pr["state"] == "open"])
        self.send(404, {"message": "Not Found"})
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0")); body = json.loads(self.rfile.read(n) or b"{}")
        LOG.append(("POST", self.path, self.headers.get("Authorization", "")))
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
