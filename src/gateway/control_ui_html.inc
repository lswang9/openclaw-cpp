R"OCUI(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>openclaw-cpp Control</title>
<style>
  :root { font-family: ui-sans-serif, system-ui, sans-serif; color: #e8eaed; background: #0f1115; }
  body { max-width: 820px; margin: 2rem auto; padding: 0 1rem; }
  h1 { font-size: 1.25rem; font-weight: 600; }
  .card { background: #1a1d24; border: 1px solid #2a2f3a; border-radius: 10px; padding: 1rem; margin: 1rem 0; }
  button, input, textarea { font: inherit; }
  input, textarea { width: 100%; box-sizing: border-box; background: #0f1115; color: #e8eaed;
    border: 1px solid #2a2f3a; border-radius: 8px; padding: .6rem .75rem; }
  textarea { min-height: 90px; resize: vertical; }
  button { background: #3b82f6; color: white; border: 0; border-radius: 8px; padding: .55rem 1rem; cursor: pointer; }
  button.secondary { background: #374151; }
  button:disabled { opacity: .5; }
  pre { white-space: pre-wrap; word-break: break-word; background: #0f1115; padding: .75rem; border-radius: 8px;
    border: 1px solid #2a2f3a; max-height: 280px; overflow: auto; font-size: .85rem; }
  .row { display: flex; gap: .5rem; flex-wrap: wrap; align-items: center; }
  .ok { color: #34d399; } .bad { color: #f87171; } .muted { color: #9ca3af; font-size: .85rem; }
</style>
</head>
<body>
  <h1>openclaw-cpp Control <span class="muted">0.8 · OpenClaw 2026.10.5 align</span></h1>
  <div class="card">
    <div class="row">
      <button id="btnHealth" class="secondary">Health</button>
      <button id="btnStatus" class="secondary">Status</button>
      <button id="btnDoctor" class="secondary">Doctor</button>
      <span id="healthBadge" class="muted">—</span>
    </div>
    <pre id="meta">Click Health to probe gateway.</pre>
  </div>
  <div class="card">
    <label class="muted">Session key</label>
    <input id="session" value="ui-demo"/>
    <label class="muted" style="display:block;margin-top:.75rem">Message</label>
    <textarea id="msg" placeholder="Say something…"></textarea>
    <div class="row" style="margin-top:.75rem">
      <button id="btnSend">Send chat</button>
      <span class="muted">POST /v1/chat/completions (agent loop)</span>
    </div>
    <pre id="reply"></pre>
  </div>
<script>
const token = localStorage.getItem('openclaw_token') || '';
function headers() {
  const h = {'Content-Type': 'application/json'};
  if (token) h['Authorization'] = 'Bearer ' + token;
  return h;
}
async function get(path) {
  const r = await fetch(path, {headers: headers()});
  const t = await r.text();
  let j; try { j = JSON.parse(t); } catch { j = t; }
  return {ok: r.ok, status: r.status, body: j};
}
document.getElementById('btnHealth').onclick = async () => {
  const r = await get('/health');
  const b = document.getElementById('healthBadge');
  b.textContent = r.ok ? 'healthy' : ('HTTP ' + r.status);
  b.className = r.ok ? 'ok' : 'bad';
  document.getElementById('meta').textContent = JSON.stringify(r.body, null, 2);
};
document.getElementById('btnStatus').onclick = async () => {
  const r = await get('/v1/status');
  document.getElementById('meta').textContent = JSON.stringify(r.body, null, 2);
};
document.getElementById('btnDoctor').onclick = async () => {
  // doctor via a tiny RPC over chat? use status for now + methods count
  const r = await get('/v1/status');
  document.getElementById('meta').textContent = JSON.stringify(r.body, null, 2);
};
document.getElementById('btnSend').onclick = async () => {
  const msg = document.getElementById('msg').value.trim();
  const session = document.getElementById('session').value.trim() || 'ui-demo';
  if (!msg) return;
  document.getElementById('btnSend').disabled = true;
  document.getElementById('reply').textContent = '…';
  try {
    const r = await fetch('/v1/chat/completions', {
      method: 'POST', headers: headers(),
      body: JSON.stringify({
        model: 'deepseek-chat',
        session_id: session,
        messages: [{role:'user', content: msg}]
      })
    });
    const j = await r.json();
    if (!r.ok) {
      document.getElementById('reply').textContent = JSON.stringify(j, null, 2);
    } else {
      const text = j.choices?.[0]?.message?.content || JSON.stringify(j, null, 2);
      document.getElementById('reply').textContent = text;
    }
  } catch (e) {
    document.getElementById('reply').textContent = String(e);
  } finally {
    document.getElementById('btnSend').disabled = false;
  }
};
</script>
</body>
</html>
)OCUI"