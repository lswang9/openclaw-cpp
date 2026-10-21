R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"/><meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>openclaw-cpp Login</title>
<style>
body{margin:0;font-family:system-ui,sans-serif;background:#0f1419;color:#e7ecf3;display:grid;place-items:center;min-height:100vh}
.card{background:#1a2332;border:1px solid #2a3548;border-radius:12px;padding:1.5rem;width:min(420px,92vw)}
input{width:100%;box-sizing:border-box;background:#0d1218;color:#e7ecf3;border:1px solid #2a3548;border-radius:8px;padding:.55rem;font-family:ui-monospace,monospace}
button{margin-top:.75rem;background:#3d8bfd;color:#fff;border:0;border-radius:8px;padding:.5rem 1rem;cursor:pointer}
button.sec{background:#2a3548}.muted{color:#8b9bb4;font-size:.8rem}#msg{min-height:1.2em;margin-top:.6rem;font-size:.85rem}
</style></head><body>
<div class="card">
<h2 style="margin-top:0">openclaw-cpp login</h2>
<p class="muted">Paste the gateway token (OPENCLAW_GATEWAY_TOKEN) or an ocd_ device token. It is stored as an HttpOnly, SameSite=Strict cookie for this gateway.</p>
<form id="f"><input id="tok" type="password" autocomplete="off" placeholder="token" autofocus/>
<input id="otp" autocomplete="one-time-code" inputmode="numeric" placeholder="MFA code (if enabled)"/>
<button type="submit">Log in</button> <button type="button" class="sec" id="out">Log out</button></form>
<div id="msg"></div>
<p class="muted">Pairing a new device? Start a link from the device; an operator approves it at /pairing.</p>
</div>
<script>
const next=new URLSearchParams(location.search).get('next')||'/';
const safeNext=(next.startsWith('/')&&!next.startsWith('//'))?next:'/';
const msg=t=>document.getElementById('msg').textContent=t;
document.getElementById('f').onsubmit=async e=>{e.preventDefault();
 const r=await fetch('/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({token:document.getElementById('tok').value,otp:document.getElementById('otp').value})});
 const j=await r.json().catch(()=>({}));
 if(r.ok&&j.ok){msg('Logged in as '+j.role+' — redirecting…');location.href=safeNext;}else msg('Login failed: '+(j.error||r.status)+(j.mfa_required?' — enter your authenticator code':''));};
document.getElementById('out').onclick=async()=>{await fetch('/logout',{method:'POST'});msg('Logged out');};
fetch('/v1/whoami').then(r=>r.ok?r.json():null).then(j=>{if(j)msg('Current: '+j.role+(j.auth_required?'':' (auth off)'));}).catch(()=>{});
</script></body></html>
)HTML"
