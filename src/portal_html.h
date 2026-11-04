#pragma once
// Wi-Fi setup page, compiled into the firmware so first-time setup works
// before the LittleFS dashboard has been uploaded.
static const char kPortalHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SaltView Setup</title>
<style>
:root{--bg:#f4f6f8;--card:#fff;--text:#1b2430;--muted:#5d6b7a;--border:#d8dee5;--accent:#1f6feb}
@media (prefers-color-scheme:dark){:root{--bg:#0f141a;--card:#18202a;--text:#e6edf3;--muted:#8b98a5;--border:#2a3542;--accent:#4c9aff}}
*{box-sizing:border-box}body{margin:0;font:16px/1.45 system-ui,sans-serif;background:var(--bg);color:var(--text)}
main{max-width:440px;margin:0 auto;padding:24px 16px}h1{font-size:22px;margin:0 0 4px}p{color:var(--muted);margin:0 0 16px}
.card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:16px;margin-bottom:16px}
ul{list-style:none;margin:0;padding:0;max-height:260px;overflow:auto}
li{display:flex;justify-content:space-between;padding:10px 8px;border-radius:8px;cursor:pointer}li:hover{background:var(--bg)}
label{display:block;font-size:14px;color:var(--muted);margin:12px 0 4px}
input{width:100%;padding:10px;border:1px solid var(--border);border-radius:8px;background:var(--bg);color:var(--text);font-size:16px}
button{width:100%;margin-top:16px;padding:12px;border:0;border-radius:8px;background:var(--accent);color:#fff;font-size:16px;cursor:pointer}
.row{display:flex;gap:8px;align-items:center;margin-top:8px;font-size:14px;color:var(--muted)}.row input{width:auto}
.err{border-color:#cf222e;color:#cf222e}
</style></head><body><main>
<h1>SaltView Wi-Fi setup</h1><p>Pick your home network. The device will reboot and join it.</p>
<div class="card err" id="err" hidden></div>
<div class="card"><ul id="nets"><li>Scanning&hellip;</li></ul></div>
<form class="card" id="f">
<label for="ssid">Network name</label><input id="ssid" required maxlength="32" autocapitalize="off" autocorrect="off" spellcheck="false" autocomplete="off">
<label for="pass">Password</label><input id="pass" type="password" maxlength="64" autocapitalize="off" autocorrect="off" spellcheck="false" autocomplete="off">
<div class="row"><input type="checkbox" id="show"><span>Show password</span></div>
<button>Save and connect</button></form>
<p id="msg"></p>
</main><script>
const $=s=>document.querySelector(s);
const bars=r=>r>-55?'&#9608;&#9608;&#9608;':r>-70?'&#9608;&#9608;&#9601;':'&#9608;&#9601;&#9601;';
async function scan(){
  const r=await fetch('/api/wifi/scan').then(r=>r.json()).catch(()=>({scanning:true}));
  if(r.scanning){setTimeout(scan,1500);return}
  const nets=r.networks.sort((a,b)=>b.rssi-a.rssi);
  $('#nets').innerHTML=nets.length?'':'<li>No networks found</li>';
  nets.forEach(n=>{const li=document.createElement('li');
    li.innerHTML=`<span></span><span>${n.secure?'&#128274; ':''}${bars(n.rssi)}</span>`;
    li.firstChild.textContent=n.ssid;li.onclick=()=>{$('#ssid').value=n.ssid;$('#pass').focus()};
    $('#nets').appendChild(li)});
}
fetch('/api/wifi/status').then(r=>r.json()).then(s=>{
  if(s.ssid)$('#ssid').value=s.ssid;
  if(s.error){$('#err').hidden=false;$('#err').textContent=`Couldn't join "${s.ssid}": ${s.error}.`}
}).catch(()=>{});
$('#show').onchange=e=>$('#pass').type=e.target.checked?'text':'password';
$('#f').onsubmit=async e=>{e.preventDefault();
  const r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({ssid:$('#ssid').value,pass:$('#pass').value})});
  $('#msg').textContent=r.ok?'Saved. SaltView is rebooting. Reconnect to your home Wi-Fi and open http://saltview.local (or the IP shown on the display).':'Save failed: '+await r.text();
};
scan();
</script></body></html>)HTML";
