R"OCUI(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>openclaw-cpp Control</title>
<style>
  :root { font-family: ui-sans-serif, system-ui, sans-serif; color: #e8eaed; background: #0f1115; }
  body { max-width: 960px; margin: 1.5rem auto; padding: 0 1rem; }
  h1 { font-size: 1.2rem; } h2 { font-size: 1rem; margin: 0 0 .75rem; color: #93c5fd; }
  .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 1rem; }
  @media (max-width: 720px) { .grid { grid-template-columns: 1fr; } }
  .card { background: #1a1d24; border: 1px solid #2a2f3a; border-radius: 10px; padding: 1rem; margin-bottom: 1rem; }
  button, input, textarea, select { font: inherit; }
  input, textarea, select { width: 100%; box-sizing: border-box; background: #0f1115; color: #e8eaed;
    border: 1px solid #2a2f3a; border-radius: 8px; padding: .55rem .7rem; margin: .35rem 0; }
  textarea { min-height: 72px; }
  button { background: #3b82f6; color: #fff; border: 0; border-radius: 8px; padding: .5rem .9rem; cursor: pointer; margin: .2rem .3rem .2rem 0; }
  button.secondary { background: #374151; }
  button:disabled { opacity: .5; }
  pre, .list { white-space: pre-wrap; word-break: break-word; background: #0f1115; padding: .65rem; border-radius: 8px;
    border: 1px solid #2a2f3a; max-height: 220px; overflow: auto; font-size: .82rem; margin: 0; }
  .ok { color: #34d399; } .bad { color: #f87171; } .muted { color: #9ca3af; font-size: .85rem; }
  .chip { display: inline-block; background: #111827; border: 1px solid #374151; border-radius: 999px; padding: .15rem .55rem; margin: .15rem; font-size: .8rem; cursor: pointer; }
  .chip:hover { border-color: #60a5fa; }
</style>
</head>
<body>
  <h1>openclaw-cpp Control <span class="muted">0.9 · align 2026.10.5</span></h1>
  <div class="card">
    <button class="secondary" id="btnHealth">Health</button>
    <button class="secondary" id="btnStatus">Status</button>
    <button class="secondary" id="btnRefresh">Refresh sessions/tools</button>
    <span id="badge" class="muted">—</span>
    <pre id="meta" class="muted" style="margin-top:.75rem">Ready.</pre>
  </div>
  <div class="grid">
    <div class="card">
      <h2>Sessions</h2>
      <div id="sessions" class="list muted">Loading…</div>
    </div>
    <div class="card">
      <h2>Tools</h2>
      <div id="tools" class="list muted">Loading…</div>
    </div>
  </div>
  <div class="card">
    <h2>Chat</h2>
    <label class="muted">Session</label>
    <input id="session" value="ui-demo"/>
    <label class="muted">Message</label>
    <textarea id="msg" placeholder="Message…"></textarea>
    <div>
      <label class="muted"><input type="checkbox" id="stream"/> Stream (SSE)</label>
    </div>
    <button id="btnSend">Send</button>
    <pre id="reply"></pre>
  </div>
<script>
const token = localStorage.getItem('openclaw_token') || '';
function headers(json=true) {
  const h = {};
  if (json) h['Content-Type'] = 'application/json';
  if (token) h['Authorization'] = 'Bearer ' + token;
  return h;
}
async function get(path) {
  const r = await fetch(path, {headers: headers(false)});
  const t = await r.text();
  let j; try { j = JSON.parse(t); } catch { j = t; }
  return {ok:r.ok, status:r.status, body:j};
}
function setBadge(ok, text) {
  const b = document.getElementById('badge');
  b.textContent = text;
  b.className = ok ? 'ok' : 'bad';
}
async function refreshLists() {
  const st = await get('/v1/status');
  const sessions = st.body.sessions || [];
  const tools = st.body.tools || [];
  document.getElementById('sessions').innerHTML = sessions.length
    ? sessions.map(s => `<span class="chip" data-s="${s}">${s}</span>`).join('')
    : '<span class="muted">(none)</span>';
  document.getElementById('tools').innerHTML = tools.length
    ? tools.map(t => `<span class="chip">${t}</span>`).join('')
    : '<span class="muted">(none)</span>';
  document.querySelectorAll('#sessions .chip').forEach(el => {
    el.onclick = () => { document.getElementById('session').value = el.dataset.s; };
  });
  const toolsAlt = await get('/v1/tools');
  if (toolsAlt.ok && toolsAlt.body.tools) {
    document.getElementById('tools').innerHTML = toolsAlt.body.tools.map(t => {
      const name = typeof t === 'string' ? t : (t.name || JSON.stringify(t));
      return `<span class="chip">${name}</span>`;
    }).join('');
  }
}
document.getElementById('btnHealth').onclick = async () => {
  const r = await get('/health');
  setBadge(r.ok, r.ok ? 'healthy' : 'HTTP '+r.status);
  document.getElementById('meta').textContent = JSON.stringify(r.body, null, 2);
};
document.getElementById('btnStatus').onclick = async () => {
  const r = await get('/v1/status');
  document.getElementById('meta').textContent = JSON.stringify(r.body, null, 2);
  setBadge(r.ok, r.ok ? 'status ok' : 'status fail');
};
document.getElementById('btnRefresh').onclick = refreshLists;
document.getElementById('btnSend').onclick = async () => {
  const msg = document.getElementById('msg').value.trim();
  const session = document.getElementById('session').value.trim() || 'ui-demo';
  const stream = document.getElementById('stream').checked;
  if (!msg) return;
  const btn = document.getElementById('btnSend');
  const out = document.getElementById('reply');
  btn.disabled = true; out.textContent = '';
  try {
    const body = {
      model: 'deepseek-chat',
      session_id: session,
      stream,
      messages: [{role:'user', content: msg}]
    };
    if (!stream) {
      const r = await fetch('/v1/chat/completions', {method:'POST', headers: headers(), body: JSON.stringify(body)});
      const j = await r.json();
      out.textContent = r.ok ? (j.choices?.[0]?.message?.content || JSON.stringify(j,null,2)) : JSON.stringify(j,null,2);
    } else {
      const r = await fetch('/v1/chat/completions', {method:'POST', headers: headers(), body: JSON.stringify(body)});
      if (!r.ok) { out.textContent = await r.text(); return; }
      const reader = r.body.getReader();
      const dec = new TextDecoder();
      let buf = '';
      while (true) {
        const {done, value} = await reader.read();
        if (done) break;
        buf += dec.decode(value, {stream:true});
        const parts = buf.split('\n');
        buf = parts.pop();
        for (const line of parts) {
          if (!line.startsWith('data:')) continue;
          const data = line.slice(5).trim();
          if (data === '[DONE]') continue;
          try {
            const j = JSON.parse(data);
            const delta = j.choices?.[0]?.delta?.content;
            if (delta) out.textContent += delta;
            if (j.error) out.textContent += '\n' + JSON.stringify(j.error);
          } catch {}
        }
      }
    }
    await refreshLists();
  } catch (e) {
    out.textContent = String(e);
  } finally {
    btn.disabled = false;
  }
};
refreshLists();
</script>
</body>
</html>
)OCUI"