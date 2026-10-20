R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"/><meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>openclaw-cpp Pairing</title>
<style>
body{margin:0;font-family:system-ui,sans-serif;background:#0f1419;color:#e7ecf3}
header{padding:1rem 1.25rem;border-bottom:1px solid #2a3548;display:flex;gap:1rem;align-items:center}
header a{color:#3d8bfd;text-decoration:none} main{padding:1rem;display:grid;gap:1rem;max-width:960px}
.panel{background:#1a2332;border:1px solid #2a3548;border-radius:10px;padding:.75rem}
table{width:100%;border-collapse:collapse;font-size:.85rem} td,th{padding:.4rem;border-bottom:1px solid #2a3548;text-align:left}
button{background:#3d8bfd;color:#fff;border:0;border-radius:6px;padding:.3rem .6rem;cursor:pointer}
button.deny{background:#a33} button.sec{background:#2a3548}
input{background:#0d1218;color:#e7ecf3;border:1px solid #2a3548;border-radius:6px;padding:.35rem}
.code{font-family:ui-monospace,monospace;font-size:1.05rem;letter-spacing:.08em}
.banner{background:#2a2208;color:#f0d78c;padding:.5rem .75rem;border-radius:8px;font-size:.8rem}
pre{white-space:pre-wrap;font-size:.8rem}
</style></head><body>
<header><strong>Device pairing</strong><a href="/">Control</a><a href="/board">Board</a><a href="/canvas">Canvas</a></header>
<main>
<div class="banner">Local pairing-code flow; tokens stored as SHA-256 on disk. Not upstream cloud device-link / QR / push.</div>
<section class="panel"><h3>Pending &amp; recent links</h3>
<button class="sec" id="refresh">Refresh</button>
<table><thead><tr><th>Code</th><th>Device</th><th>State</th><th>Expires</th><th></th></tr></thead><tbody id="links"></tbody></table></section>
<section class="panel"><h3>Issued tokens</h3>
<table><thead><tr><th>Token id</th><th>Device</th><th>Revoked</th><th></th></tr></thead><tbody id="tokens"></tbody></table></section>
<section class="panel"><h3>Simulate a device (testing)</h3>
<input id="dname" value="test-device"/> <button id="start">Start link</button>
<button class="sec" id="claim">Claim token</button><pre id="out"></pre></section>
</main>
<script>
const rpc=async(m,p={})=>{const r=await fetch('/v1/rpc',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({method:m,params:p})});const j=await r.json();return j.payload||j;};
let last=null;
const esc=s=>String(s??'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
async function load(){const r=await rpc('pairing.link.list');
 const lb=document.getElementById('links');lb.innerHTML='';
 (r.links||[]).slice().reverse().forEach(l=>{const tr=document.createElement('tr');
  tr.innerHTML=`<td class="code">${esc(l.code)}</td><td>${esc(l.device_name)}</td><td>${esc(l.state)}</td><td>${new Date(l.expires_ms).toLocaleTimeString()}</td><td></td>`;
  if(l.state==='pending'){const a=document.createElement('button');a.textContent='Approve';a.onclick=async()=>{await rpc('pairing.link.approve',{id:l.id});load();};
   const d=document.createElement('button');d.textContent='Deny';d.className='deny';d.onclick=async()=>{await rpc('pairing.link.deny',{id:l.id});load();};
   tr.lastChild.append(a,' ',d);} lb.appendChild(tr);});
 const tb=document.getElementById('tokens');tb.innerHTML='';
 (r.tokens||[]).forEach(t=>{const tr=document.createElement('tr');
  tr.innerHTML=`<td>${esc(t.id)}</td><td>${esc(t.device_name)}</td><td>${t.revoked?'yes':'no'}</td><td></td>`;
  if(!t.revoked){const b=document.createElement('button');b.className='deny';b.textContent='Revoke';b.onclick=async()=>{await rpc('pairing.token.revoke',{token_id:t.id});load();};tr.lastChild.append(b);}
  tb.appendChild(tr);});}
document.getElementById('refresh').onclick=load;
document.getElementById('start').onclick=async()=>{last=await rpc('pairing.link.start',{device_name:document.getElementById('dname').value});document.getElementById('out').textContent=JSON.stringify(last,null,2);load();};
document.getElementById('claim').onclick=async()=>{if(!last||!last.link)return;const r=await rpc('pairing.link.claim',{id:last.link.id,poll_secret:last.poll_secret});document.getElementById('out').textContent=JSON.stringify(r,null,2);load();};
load();setInterval(load,5000);
</script></body></html>
)HTML"
