R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>openclaw-cpp Canvas</title>
<style>
:root { --bg:#0f1419; --card:#1a2332; --fg:#e7ecf3; --muted:#8b9bb4; --accent:#3d8bfd; --border:#2a3548; }
* { box-sizing: border-box; }
body { margin:0; font-family: ui-sans-serif, system-ui, sans-serif; background:var(--bg); color:var(--fg); }
header { padding:1rem 1.25rem; border-bottom:1px solid var(--border); display:flex; gap:1rem; align-items:center; flex-wrap:wrap; }
header a { color:var(--accent); text-decoration:none; }
h1 { font-size:1.1rem; margin:0; font-weight:600; }
.muted { color:var(--muted); font-size:.85rem; }
main { display:grid; grid-template-columns: 240px 1fr 280px; gap:1rem; padding:1rem; min-height: calc(100vh - 64px); }
@media (max-width: 960px) { main { grid-template-columns: 1fr; } }
.panel { background:var(--card); border:1px solid var(--border); border-radius:10px; padding:.75rem; }
.panel h2 { margin:0 0 .75rem; font-size:.95rem; }
ul { list-style:none; padding:0; margin:0; }
li.doc { padding:.5rem .6rem; border-radius:8px; cursor:pointer; border:1px solid transparent; margin-bottom:.35rem; }
li.doc:hover, li.doc.active { background:#243044; border-color:var(--border); }
label { display:block; font-size:.75rem; color:var(--muted); margin:.5rem 0 .25rem; }
input, textarea { width:100%; background:#0d1218; color:var(--fg); border:1px solid var(--border); border-radius:8px; padding:.5rem .6rem; font:inherit; }
textarea { min-height: 120px; font-family: ui-monospace, monospace; font-size:.85rem; }
.row { display:flex; gap:.5rem; flex-wrap:wrap; margin-top:.75rem; }
button { background:var(--accent); color:#fff; border:0; border-radius:8px; padding:.45rem .8rem; cursor:pointer; font:inherit; }
button.secondary { background:#2a3548; }
button.danger { background:#a33; }
#preview { background:#fff; color:#111; border-radius:8px; padding:1rem; min-height:220px; overflow:auto; }
#events { max-height: 360px; overflow:auto; font-size:.8rem; font-family: ui-monospace, monospace; }
.ev { border-bottom:1px solid var(--border); padding:.35rem 0; }
.banner { background:#2a2208; color:#f0d78c; padding:.5rem .75rem; border-radius:8px; font-size:.8rem; margin-bottom:.75rem; }
</style>
</head>
<body>
<header>
  <h1>Canvas</h1>
  <span class="muted">local HTML/JSON · not full A2UI</span>
  <a href="/">Control</a>
  <a href="/board">Board</a>
</header>
<main>
  <section class="panel">
    <h2>Documents</h2>
    <div class="row">
      <button type="button" id="btnNew">New</button>
      <button type="button" class="secondary" id="btnRefresh">Refresh</button>
    </div>
    <ul id="docs"></ul>
  </section>
  <section class="panel">
    <div class="banner">Disk-backed local canvas. Not upstream OpenClaw canvas / A2UI compositor.</div>
    <label>Title</label>
    <input id="title" />
    <label>HTML</label>
    <textarea id="html"></textarea>
    <label>Widgets (JSON array)</label>
    <textarea id="widgets">[]</textarea>
    <div class="row">
      <button type="button" id="btnSave">Save patch</button>
      <button type="button" class="danger" id="btnDel">Delete</button>
    </div>
    <label style="margin-top:1rem">Preview</label>
    <div id="preview"></div>
  </section>
  <section class="panel">
    <h2>Event log</h2>
    <div class="row">
      <button type="button" class="secondary" id="btnEv">Reload</button>
      <button type="button" class="secondary" id="btnPing">Append ui.ping</button>
    </div>
    <div id="events"></div>
  </section>
</main>
<script>
const rpc = async (method, params={}) => {
  const res = await fetch('/v1/rpc', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify({method, params})
  });
  const j = await res.json();
  return j.payload || j.result || j;
};
let currentId = null;
const el = (id) => document.getElementById(id);

async function loadList() {
  const r = await rpc('canvas.list');
  const docs = (r.docs || []);
  const ul = el('docs');
  ul.innerHTML = '';
  docs.forEach(d => {
    const li = document.createElement('li');
    li.className = 'doc' + (d.id === currentId ? ' active' : '');
    li.textContent = (d.title || d.id) + ' · ' + d.id;
    li.onclick = () => select(d.id);
    ul.appendChild(li);
  });
  if (!currentId && docs.length) await select(docs[0].id);
}

async function select(id) {
  currentId = id;
  const r = await rpc('canvas.get', {id});
  const doc = r.document || {};
  el('title').value = doc.title || '';
  el('html').value = doc.html || '';
  el('widgets').value = JSON.stringify(doc.widgets || [], null, 2);
  el('preview').innerHTML = doc.html || '';
  await loadList();
  await loadEvents();
}

el('btnNew').onclick = async () => {
  const title = prompt('Title', 'canvas') || 'canvas';
  const r = await rpc('canvas.create', {title, html: '<h1>'+title+'</h1><p>edit me</p>'});
  if (r.document) await select(r.document.id);
  else await loadList();
};
el('btnRefresh').onclick = () => loadList();
el('btnSave').onclick = async () => {
  if (!currentId) return;
  let widgets = [];
  try { widgets = JSON.parse(el('widgets').value || '[]'); } catch(e) { alert('widgets JSON invalid'); return; }
  const r = await rpc('canvas.patch', {
    id: currentId,
    title: el('title').value,
    html: el('html').value,
    widgets
  });
  if (r.document) {
    el('preview').innerHTML = r.document.html || '';
    await loadList();
    await loadEvents();
  } else alert(JSON.stringify(r));
};
el('btnDel').onclick = async () => {
  if (!currentId || !confirm('Delete '+currentId+'?')) return;
  await rpc('canvas.delete', {id: currentId});
  currentId = null;
  el('title').value = ''; el('html').value = ''; el('preview').innerHTML = '';
  await loadList();
};
async function loadEvents() {
  const r = await rpc('canvas.events', {limit: 40, id: currentId || ''});
  const box = el('events');
  box.innerHTML = '';
  (r.events || []).forEach(e => {
    const d = document.createElement('div');
    d.className = 'ev';
    d.textContent = (e.ts_ms||'') + ' ' + (e.kind||'') + ' ' + (e.canvas_id||'') + ' ' + JSON.stringify(e.detail||{});
    box.appendChild(d);
  });
}
el('btnEv').onclick = loadEvents;
el('btnPing').onclick = async () => {
  await rpc('canvas.event', {kind:'ui.ping', id: currentId || '', detail:{from:'canvas-ui'}});
  await loadEvents();
};
loadList();
</script>
</body>
</html>
)HTML"
