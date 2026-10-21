import json, sys
t = json.load(open(sys.argv[1]))["payload"]["tree"]
r = {"b": None, "e": None}
def w(n):
    if n.get("role") in ("push button", "button") and n.get("name") == "Press me": r["b"] = r["b"] or n["path"]
    if n.get("role") in ("text", "entry"): r["e"] = r["e"] or n["path"]
    for c in n.get("children", []): w(c)
w(t)
print(json.dumps(r["b"], separators=(",", ":")), json.dumps(r["e"], separators=(",", ":")))
