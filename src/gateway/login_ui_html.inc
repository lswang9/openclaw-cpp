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
<p id="sso" style="display:none"><a href="/login/oidc">Sign in with SSO (OIDC)</a></p>
<p><button type="button" class="sec" id="pk" style="display:none">Sign in with a passkey</button>
<button type="button" class="sec" id="pkreg">Register a passkey (while logged in)</button></p>
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
fetch('/login/methods').then(r=>r.json()).then(j=>{if(j.oidc)document.getElementById('sso').style.display='';if(j.webauthn)document.getElementById('pk').style.display='';}).catch(()=>{});
const b64u=b=>btoa(String.fromCharCode(...new Uint8Array(b))).replace(/\+/g,'-').replace(/\//g,'_').replace(/=+$/,'');
const unb=s=>Uint8Array.from(atob(s.replace(/-/g,'+').replace(/_/g,'/')+'==='.slice((s.length+3)%4)),c=>c.charCodeAt(0));
const csrf=()=>(document.cookie.match(/(?:^|; )oc_csrf=([^;]*)/)||[])[1]||'';
document.getElementById('pk').onclick=async()=>{try{
 const o=await (await fetch('/login/webauthn/options',{method:'POST',body:'{}'})).json();
 const c=await navigator.credentials.get({publicKey:{...o,challenge:unb(o.challenge),allowCredentials:o.allowCredentials.map(a=>({...a,id:unb(a.id)}))}});
 const r=await fetch('/login/webauthn',{method:'POST',body:JSON.stringify({id:c.id,rawId:b64u(c.rawId),type:c.type,response:{clientDataJSON:b64u(c.response.clientDataJSON),authenticatorData:b64u(c.response.authenticatorData),signature:b64u(c.response.signature)}})});
 const j=await r.json();if(r.ok){msg('Logged in as '+j.role+' with a passkey');location.href=safeNext;}else msg('Passkey login failed: '+j.error);}catch(e){msg('Passkey: '+e);}};
document.getElementById('pkreg').onclick=async()=>{try{
 const h={'X-CSRF-Token':decodeURIComponent(csrf())};
 const o=await (await fetch('/login/webauthn/register/options',{method:'POST',headers:h,body:'{}'})).json();
 if(!o.challenge){msg('Log in first: '+(o.error||''));return;}
 const c=await navigator.credentials.create({publicKey:{...o,challenge:unb(o.challenge),user:{...o.user,id:unb(o.user.id)},excludeCredentials:o.excludeCredentials.map(a=>({...a,id:unb(a.id)}))}});
 const r=await fetch('/login/webauthn/register',{method:'POST',headers:h,body:JSON.stringify({id:c.id,rawId:b64u(c.rawId),type:c.type,response:{clientDataJSON:b64u(c.response.clientDataJSON),attestationObject:b64u(c.response.attestationObject)}})});
 const j=await r.json();msg(r.ok?'Passkey registered ('+j.role+')':'Register failed: '+j.error);}catch(e){msg('Passkey: '+e);}};
fetch('/v1/whoami').then(r=>r.ok?r.json():null).then(j=>{if(j)msg('Current: '+j.role+(j.auth_required?'':' (auth off)'));}).catch(()=>{});
</script></body></html>
)HTML"
