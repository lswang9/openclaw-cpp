#!/usr/bin/env python3
"""Classify every upstream v2026.10.5 gateway method by how the C++ gateway answers {} params:
real (impl=real or plain ok payload), real-sim (impl=real-sim / simulation), honest-partial (impl=honest),
validation (ok=false with a params/argument error: handler exists), error (other ok=false), unimplemented."""
import json, os, sys, urllib.request, collections
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
url = sys.argv[1]
names = [l.strip() for l in open(os.path.join(ROOT, "scripts/upstream-methods.txt")) if l.strip()]
out = collections.defaultdict(list)
for m in names:
    if "restart" in m or "shutdown" in m or m.startswith("update.") or m in ("gateway.stop", "gateway.stop.request", "config.reload"):
        out["skipped-destructive"].append(m); continue
    req = urllib.request.Request(url, json.dumps({"jsonrpc": "2.0", "id": 1, "method": m, "params": {}}).encode(), {"Content-Type": "application/json"})
    try: body = urllib.request.urlopen(req, timeout=15).read()
    except urllib.error.HTTPError as e: body = e.read()
    except Exception as e: out["timeout"].append(m); continue
    t = body.decode("utf-8", "replace")
    if "not implemented in the C++ runtime" in t: out["unimplemented"].append(m); continue
    try: j = json.loads(t)
    except Exception: out["error"].append(m); continue
    p = j.get("payload") if isinstance(j.get("payload"), dict) else {}
    s = json.dumps(j).lower()
    if p.get("impl") == "real-sim" or p.get("simulation") is True: out["real-sim"].append(m)
    elif p.get("impl") == "honest": out["honest-partial"].append(m)
    elif j.get("ok"): out["real"].append(m)
    elif any(k in s for k in ("required", "missing", "invalid", "expected", "must ", "unknown ", "not found")): out["validation"].append(m)
    else: out["error"].append(m)
json.dump({k: sorted(v) for k, v in out.items()}, open(sys.argv[2], "w"), indent=1)
print(json.dumps({k: len(v) for k, v in out.items()}), "total", len(names))
