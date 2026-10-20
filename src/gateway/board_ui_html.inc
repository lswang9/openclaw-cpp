R"OCUI(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>openclaw-cpp Board</title>
<style>
  :root { font-family: ui-sans-serif, system-ui, sans-serif; color: #e8eaed; background: #0f1115; }
  body { margin: 0; padding: 1rem 1.25rem 2rem; }
  h1 { font-size: 1.15rem; margin: 0 0 .35rem; }
  .muted { color: #9ca3af; font-size: .85rem; }
  a { color: #93c5fd; }
  .toolbar { display: flex; flex-wrap: wrap; gap: .5rem; align-items: center; margin: .75rem 0 1rem; }
  input, button, textarea { font: inherit; }
  input, textarea { background: #0f1115; color: #e8eaed; border: 1px solid #2a2f3a; border-radius: 8px; padding: .45rem .65rem; }
  button { background: #3b82f6; color: #fff; border: 0; border-radius: 8px; padding: .45rem .8rem; cursor: pointer; }
  button.secondary { background: #374151; }
  .board { display: flex; gap: 1rem; overflow-x: auto; align-items: flex-start; padding-bottom: 1rem; }
  .col { min-width: 260px; max-width: 300px; background: #1a1d24; border: 1px solid #2a2f3a; border-radius: 12px; padding: .75rem; }
  .col h2 { font-size: .95rem; margin: 0 0 .65rem; color: #93c5fd; }
  .card { background: #0f1115; border: 1px solid #2a2f3a; border-radius: 10px; padding: .65rem; margin-bottom: .55rem; }
  .card .title { font-weight: 600; margin-bottom: .25rem; }
  .card .body { color: #9ca3af; font-size: .85rem; white-space: pre-wrap; }
  .card .actions { margin-top: .45rem; display: flex; flex-wrap: wrap; gap: .3rem; }
  .card .actions button { font-size: .75rem; padding: .25rem .5rem; }
  #msg { min-height: 1.2rem; }
  .ok { color: #34d399; } .bad { color: #f87171; }
</style>
</head>
<body>
  <h1>Board <span class="muted">local JSON · not full OpenClaw board UX</span></h1>
  <p class="muted"><a href="/">← Control</a> · RPC <code>board.*</code> · persisted under data dir</p>
  <div class="toolbar">
    <input id="title" placeholder="Card title" style="min-width:160px"/>
    <input id="body" placeholder="Body (optional)" style="min-width:200px"/>
    <select id="colPick"></select>
    <button id="btnAdd">Add card</button>
    <button class="secondary" id="btnRefresh">Refresh</button>
    <span id="msg" class="muted"></span>
  </div>
  <div id="board" class="board">Loading…</div>
<script>
const token = localStorage.getItem('openclaw_token') || '';
function headers() {
  const h = {'Content-Type': 'application/json'};
  if (token) h['Authorization'] = 'Bearer ' + token;
  return h;
}
async function rpc(method, params={}) {
  const r = await fetch('/v1/rpc', {
    method: 'POST', headers: headers(),
    body: JSON.stringify({jsonrpc:'2.0', id:1, method, params})
  });
  const j = await r.json();
  if (j.error) throw new Error(j.error.message || JSON.stringify(j.error));
  return j.payload || j.result || j;
}
function setMsg(t, ok) {
  const el = document.getElementById('msg');
  el.textContent = t;
  el.className = ok === false ? 'bad' : (ok ? 'ok' : 'muted');
}
function sortedCols(board) {
  return (board.columns || []).slice().sort((a,b)=>(a.order||0)-(b.order||0));
}
function cardsIn(board, colId) {
  return (board.cards || []).filter(c => c.column_id === colId)
    .sort((a,b)=>(a.order||0)-(b.order||0));
}
function render(board) {
  const root = document.getElementById('board');
  const pick = document.getElementById('colPick');
  const cols = sortedCols(board);
  pick.innerHTML = cols.map(c => `<option value="${c.id}">${c.title}</option>`).join('');
  root.innerHTML = cols.map(col => {
    const cards = cardsIn(board, col.id);
    return `<div class="col" data-col="${col.id}">
      <h2>${col.title} <span class="muted">(${cards.length})</span></h2>
      ${cards.map(card => {
        const moves = cols.filter(c => c.id !== col.id)
          .map(c => `<button data-id="${card.id}" data-move="${c.id}" class="secondary">→ ${escapeHtml(c.title)}</button>`).join('');
        return `<div class="card" data-id="${card.id}">
        <div class="title">${escapeHtml(card.title||'')}</div>
        <div class="body">${escapeHtml(card.body||'')}</div>
        <div class="actions">
          ${moves}
          <button data-del="${card.id}" class="secondary">Delete</button>
        </div>
      </div>`;
      }).join('') || '<div class="muted">Empty</div>'}
    </div>`;
  }).join('') || '<div class="muted">No columns</div>';

  root.querySelectorAll('[data-move]').forEach(btn => {
    btn.onclick = async () => {
      try {
        const id = btn.getAttribute('data-id');
        const col = btn.getAttribute('data-move');
        const res = await rpc('board.card.move', {id, column_id: col});
        render(res.board || (await rpc('board.get')).board || res);
        setMsg('moved ' + id, true);
      } catch (e) { setMsg(String(e), false); }
    };
  });
  root.querySelectorAll('[data-del]').forEach(btn => {
    btn.onclick = async () => {
      try {
        const id = btn.getAttribute('data-del');
        const res = await rpc('board.card.delete', {id});
        render(res.board || (await rpc('board.get')).board || res);
        setMsg('deleted ' + id, true);
      } catch (e) { setMsg(String(e), false); }
    };
  });
}
function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}
async function refresh() {
  try {
    const res = await rpc('board.get');
    const board = res.board || res;
    render(board);
    setMsg('columns=' + (board.columns||[]).length + ' cards=' + (board.cards||[]).length, true);
  } catch (e) { setMsg(String(e), false); }
}
document.getElementById('btnRefresh').onclick = refresh;
document.getElementById('btnAdd').onclick = async () => {
  try {
    const title = document.getElementById('title').value.trim() || 'Untitled';
    const body = document.getElementById('body').value.trim();
    const column_id = document.getElementById('colPick').value;
    const res = await rpc('board.card.create', {title, body, column_id});
    document.getElementById('title').value = '';
    document.getElementById('body').value = '';
    render(res.board || (await rpc('board.get')).board);
    setMsg('created', true);
  } catch (e) { setMsg(String(e), false); }
};
refresh();
</script>
</body>
</html>
)OCUI"
