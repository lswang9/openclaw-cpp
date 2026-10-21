import re,os,json,sys
ROOT='/tmp/up/src/cli'
want=set("devices nodes node sessions logs health doctor pairing hooks webhooks secrets security system users worktrees sandbox agents agent directory backup usage update claws automations dashboard audit transcripts message telemetry qr infer capability daemon exec-approvals promos proxy dns worker attach database storage triage completion setup onboard configure migrate tui crestodian".split())
tree={}
tok=re.compile(r'''\.command\(\s*["'`]([^"'`]+)["'`]|(?:\.option|\.requiredOption)\(\s*["'`]([^"'`]+)["'`]|\.argument\(\s*["'`]([^"'`]+)["'`]|new Option\(\s*["'`]([^"'`]+)["'`]''')
for dp,dn,fn in os.walk(ROOT):
  for f in fn:
    if not f.endswith('.ts') or '.test' in f or 'test-' in f: continue
    s=open(os.path.join(dp,f)).read()
    root=None;cur=None
    for m in tok.finditer(s):
      c,o,a,o2=m.groups()
      if c:
        parts=c.split()
        name=parts[0]
        args=[p.strip('<>[]').rstrip('.') for p in parts[1:]]
        if name in want and (root is None or name!=root):
          if root is None or s[max(0,m.start()-40):m.start()].rstrip().endswith('program') :
            root=name; cur=tree.setdefault(root,{"":{"flags":[],"args":args}})['']; continue
        if root is None: continue
        cur=tree[root].setdefault(name,{"flags":[],"args":args})
      elif (o or o2) and cur is not None:
        fl=[x for x in re.split(r'[ ,]+',o or o2) if x.startswith('--')]
        if fl:
          f0=fl[0]; takes='<' in (o or o2) or '[' in (o or o2)
          ent=f0+('=' if takes else '')
          if ent not in cur["flags"]: cur["flags"].append(ent)
      elif a and cur is not None:
        cur["args"].append(a.split()[0].strip('<>[]').rstrip('.'))
json.dump(tree,open('/tmp/ext/tree.json','w'),indent=1)
for k in sorted(tree): print(k, len(tree[k])-1, ' '.join(list(tree[k])[1:])[:150])
