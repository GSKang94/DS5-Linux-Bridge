#ifndef DS5_BRIDGE_WEB_PAGE_H
#define DS5_BRIDGE_WEB_PAGE_H

// Config UI served over WiFi at http://<hostname>.local/ (default ds5wol.local).
// Single self-contained page; loads from GET /api/config and persists via
// POST /api/config. Settings mirror Config_body (src/config.h); the firmware
// re-validates every field, so the page is a convenience, not the source of
// truth for bounds.
static const char WEB_PAGE[] = R"rawhtml(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DS5-Linux-Bridge</title>
<style>
:root{color-scheme:dark}
body{font-family:system-ui,sans-serif;background:#111;color:#eee;max-width:560px;margin:2rem auto;padding:0 1rem}
h1{font-size:1.4rem}h1 small{color:#888;font-weight:normal;font-size:.7em}
.field{margin:1.1rem 0}
label.lbl{display:block;margin-bottom:.3rem;font-size:.95rem}
.hint{color:#888;font-size:.8rem;margin-top:.2rem}
select,input[type=number],input[type=text]{background:#222;border:1px solid #444;color:#eee;padding:.4rem;border-radius:4px;width:100%;box-sizing:border-box;font-size:.95rem}
input[type=range]{width:100%}
.chk{display:flex;align-items:center;gap:.5rem}
.chk input{width:auto}
button{background:#2563eb;border:0;color:#fff;padding:.55rem 1.3rem;border-radius:4px;cursor:pointer;font-size:1rem;margin-top:1rem}
button:disabled{background:#333;color:#777;cursor:default}
button.fg{background:#7f1d1d}
button.fg:disabled{background:#333;color:#777}
#status{min-height:1.2em;margin-left:1rem}
.dirty{color:#facc15}
.ok{color:#4ade80}
.err{color:#f87171}
hr{border:0;border-top:1px solid #333;margin:2rem 0}
h2{font-size:1.1rem;margin-bottom:.3rem}
.bond{display:flex;align-items:center;gap:.5rem;flex-wrap:wrap;padding:.5rem 0;border-bottom:1px solid #222}
.bond .nm{flex:1 1 8rem;min-width:0;background:#222;border:1px solid #444;color:#eee;padding:.35rem;border-radius:4px;font-size:.9rem}
.bond .addr{color:#888;font-size:.78rem;font-family:monospace}
.bond .dot{color:#4ade80;font-size:.78rem;white-space:nowrap}
.bond button{margin:0;padding:.35rem .7rem;font-size:.85rem;background:#3a3a3a;flex:none}
.bond button.fg,button.fg{background:#7f1d1d}
.btns{display:flex;gap:.5rem;align-items:center}
#bonds_empty{color:#888;font-size:.9rem}
#statuscard{display:flex;align-items:center;gap:1rem;flex-wrap:wrap;background:#1a1a1a;border:1px solid #333;border-radius:8px;padding:.7rem 1rem;margin:1rem 0}
#statuscard .dot{width:.6rem;height:.6rem;border-radius:50%;background:#555;flex:none}
#statuscard.on .dot{background:#4ade80}
#statuscard .s{font-size:.9rem}
#statuscard .s b{color:#fff}
#statuscard .muted{color:#888}
.batt{display:inline-flex;align-items:center;gap:.35rem}
.batt .bar{width:34px;height:14px;border:1px solid #888;border-radius:2px;position:relative;padding:1px}
.batt .bar::after{content:"";position:absolute;right:-3px;top:4px;width:2px;height:6px;background:#888}
.batt .fill{height:100%;background:#4ade80;border-radius:1px}
.batt.low .fill{background:#f87171}
footer{margin:2.5rem 0 1rem;padding-top:1rem;border-top:1px solid #333;display:flex;gap:1rem;flex-wrap:wrap;align-items:center;font-size:.85rem;color:#888}
footer a{color:#60a5fa;text-decoration:none}
footer a:hover{text-decoration:underline}
footer .kofi{color:#fff;background:#13c3ff;padding:.3rem .7rem;border-radius:4px}
footer .kofi:hover{text-decoration:none;opacity:.9}
</style></head><body>
<h1>DS5-Linux-Bridge <small id="ver"></small></h1>
<p>Adapter configuration. Changes are saved to the adapter's flash.</p>

<div id="statuscard">
  <span class="dot"></span>
  <span class="s" id="st_conn">Checking…</span>
  <span class="s batt" id="st_batt" style="display:none">
    <span class="bar"><span class="fill" id="st_fill"></span></span>
    <span id="st_pct"></span>
  </span>
</div>

<div class="field">
  <label class="lbl">Controller mode</label>
  <select id="controller_mode">
    <option value="2">Auto-detect</option>
    <option value="0">DualSense (DS5)</option>
    <option value="1">DualSense Edge (DSE)</option>
  </select>
  <div class="hint">Takes effect after reconnecting the controller.</div>
</div>

<div class="field">
  <label class="lbl">Polling rate</label>
  <select id="polling_rate_mode">
    <option value="0">250 Hz</option>
    <option value="1">500 Hz</option>
    <option value="2">Real-time (1000 Hz)</option>
  </select>
  <div class="hint">Takes effect after reconnecting the controller.</div>
</div>

<div class="field">
  <label class="lbl">Audio buffer length: <span id="ab_val"></span></label>
  <input type="range" id="audio_buffer_length" min="16" max="128" step="1">
  <div class="hint">Lower = less latency, higher = more stutter resistance (16-128).</div>
</div>

<div class="field">
  <label class="lbl">Inactivity timeout: <span id="it_val"></span> min</label>
  <input type="range" id="inactive_time" min="5" max="60" step="1">
  <div class="hint">Disconnect the controller after this idle time.</div>
</div>

<div class="field chk">
  <input type="checkbox" id="disable_inactive_disconnect">
  <label for="disable_inactive_disconnect">Never auto-disconnect on inactivity</label>
</div>

<div class="field chk">
  <input type="checkbox" id="disable_pico_led">
  <label for="disable_pico_led">Disable the onboard Pico LED</label>
</div>

<div>
  <button id="save">Save</button>
  <button id="factoryreset" class="fg">Factory reset</button>
  <span id="status"></span>
</div>
<div class="hint">Factory reset restores all settings above to defaults. Paired
  controllers are kept (use <b>Forget all</b> below to remove those).</div>

<hr>

<h2>Paired controllers</h2>
<div class="hint">Controllers the adapter remembers. The adapter holds up to
  <span id="bond_max">4</span>. Once a controller is paired the adapter stops
  looking for new ones (a remembered controller reconnects on its own) &mdash;
  use <b>Pair new controller</b> to add another, or forget one to free a slot.</div>
<div id="bonds"></div>
<div id="bonds_empty" style="display:none">No paired controllers stored.</div>
<div class="btns">
  <button id="pair">Pair new controller</button>
  <button id="forgetall" class="fg">Forget all</button>
  <span id="bstatus"></span>
</div>

<div id="wol_section" style="display:none">
<hr>
<h2>Network</h2>
<div class="field">
  <label class="lbl">Device name</label>
  <input id="hostname" type="text" inputmode="latin" maxlength="10"
         placeholder="ds5wol" pattern="[A-Za-z0-9-]{1,10}">
  <div class="hint">The name this adapter uses on your network — reach the page at
    <code>http://&lt;name&gt;.local/</code>. Give each adapter a unique name if you
    run more than one (otherwise they collide on <code>ds5wol.local</code>).
    Letters, digits and hyphens only. Takes effect after the adapter reboots.</div>
</div>
<div class="btns">
  <button id="net_save">Save</button>
  <span id="nstatus"></span>
</div>
<div class="field" id="wifi_reset_field" style="display:none">
  <button id="wifi_reset" type="button" class="fg">Reset saved WiFi</button>
  <span id="wrstatus"></span>
</div>

<h2>Wake-on-LAN</h2>
<div class="hint">Wake a PC over the network by sending it a magic packet. Useful
  when the PC is fully off (S4/S5) and USB wake isn't supported by its
  motherboard. Press the controller's PS button to wake, or use the button here.
  The target PC must have Wake-on-LAN enabled in its BIOS and OS network driver.</div>
<div class="field">
  <label class="lbl">Target PC MAC address</label>
  <input id="wol_target_mac" type="text" inputmode="latin"
         placeholder="AA:BB:CC:DD:EE:FF" pattern="([0-9A-Fa-f]{2}[:\-]?){5}[0-9A-Fa-f]{2}">
  <div class="hint">The network adapter (NIC) MAC of the PC to wake. Don't know
    it? Enter the PC's IP address below and click "Find MAC" — the adapter will
    look it up on your network automatically.</div>
  <div style="margin-top:.4rem;display:flex;gap:.5rem;align-items:center">
    <input id="wol_resolve_ip" type="text" inputmode="decimal" style="flex:1"
           placeholder="PC's IP, e.g. 192.168.1.50" pattern="\d{1,3}(\.\d{1,3}){3}">
    <button id="wol_resolve" type="button">Find MAC</button>
  </div>
</div>
<div class="btns">
  <button id="wol_save">Save</button>
  <button id="wol_wake">Wake PC now</button>
  <span id="wstatus"></span>
</div>
</div>

<script>
const $=id=>document.getElementById(id);
function setStatus(msg,cls){const s=$('status');s.className=cls||'';s.textContent=msg}

function bindRange(id,out){const el=$(id);const fn=()=>$(out).textContent=el.value;el.oninput=()=>{fn();markDirty()};return fn}
const upd=[bindRange('audio_buffer_length','ab_val'),bindRange('inactive_time','it_val')];

function markDirty(){$('save').disabled=false;setStatus('unsaved changes','dirty')}
['controller_mode','polling_rate_mode','disable_inactive_disconnect','disable_pico_led']
  .forEach(id=>$(id).onchange=markDirty);

async function load(){
  try{
    const c=await (await fetch('/api/config')).json();
    $('ver').textContent=c.version;
    $('controller_mode').value=c.controller_mode;
    $('polling_rate_mode').value=c.polling_rate_mode;
    $('audio_buffer_length').value=c.audio_buffer_length;
    $('inactive_time').value=c.inactive_time;
    $('disable_inactive_disconnect').checked=!!c.disable_inactive_disconnect;
    $('disable_pico_led').checked=!!c.disable_pico_led;
    // Network/Wake-on-LAN section. wol_target_mac is 12 hex chars, all-zero ==
    // unset. (wol_capable/wifi_capable are always true on current firmware; the
    // gates keep the page working against older firmware.)
    if(c.wol_capable){
      $('wol_section').style.display='';
      // Always reflect the current saved name (server returns a sanitized,
      // never-empty hostname). Assign unconditionally so the box shows the real
      // value, not the placeholder, after a rename.
      $('hostname').value=c.hostname||'';
      if(c.wol_target_mac&&c.wol_target_mac!=='000000000000')
        $('wol_target_mac').value=fmtAddr(c.wol_target_mac);
    }
    if(c.wifi_capable){
      $('wifi_reset_field').style.display='';
    }
    upd.forEach(f=>f());
    $('save').disabled=true;setStatus('');
  }catch(e){setStatus('load failed','err')}
}

async function save(){
  const body=[
    'controller_mode='+$('controller_mode').value,
    'polling_rate_mode='+$('polling_rate_mode').value,
    'audio_buffer_length='+$('audio_buffer_length').value,
    'inactive_time='+$('inactive_time').value,
    'disable_inactive_disconnect='+($('disable_inactive_disconnect').checked?1:0),
    'disable_pico_led='+($('disable_pico_led').checked?1:0)
  ].join('&');
  setStatus('saving…','dirty');
  try{
    const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
    if(r.ok){$('save').disabled=true;setStatus('Saved ✓','ok')}
    else setStatus('save failed — not written to flash, try again','err');
  }catch(e){setStatus('save failed','err')}
}

$('save').onclick=save;

async function factoryReset(){
  if(!confirm('Reset all settings to defaults? Paired controllers are kept.'))return;
  setStatus('resetting…','dirty');
  try{
    const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'factory_reset=1'});
    if(r.ok){setStatus('Reset ✓ — reloading','ok');setTimeout(()=>location.reload(),600)}
    else setStatus('reset failed — not written to flash, try again','err');
  }catch(e){setStatus('reset failed','err')}
}
$('factoryreset').onclick=factoryReset;

// ----- Paired controllers -----
function fmtAddr(h){return h.match(/.{2}/g).join(':')}
function bstatus(msg,cls){const s=$('bstatus');s.className=cls||'';s.textContent=msg}

async function loadBonds(){
  try{
    const d=await (await fetch('/api/bonds')).json();
    $('bond_max').textContent=d.max;
    const box=$('bonds');box.innerHTML='';
    const bonds=d.bonds||[];
    $('bonds_empty').style.display=bonds.length?'none':'block';
    bonds.forEach(b=>{
      const connected=d.connected&&d.connected===b.addr;
      const row=document.createElement('div');row.className='bond';
      const nm=document.createElement('input');
      nm.className='nm';nm.maxLength=15;nm.value=b.name;
      nm.placeholder=connected?'(connected)':'unnamed';
      const meta=document.createElement('span');meta.className='addr';
      meta.textContent=fmtAddr(b.addr);
      const dot=document.createElement('span');dot.className='dot';
      dot.textContent=connected?'● connected':'';
      const ren=document.createElement('button');ren.textContent='Rename';
      ren.onclick=()=>renameBond(b.addr,nm.value);
      const fg=document.createElement('button');fg.className='fg';fg.textContent='Forget';
      fg.onclick=()=>forgetBond(b.addr,nm.value||fmtAddr(b.addr));
      row.append(nm,meta,dot,ren,fg);
      box.appendChild(row);
    });
    bstatus('');
  }catch(e){bstatus('load failed','err')}
}

async function postBonds(body,msg){
  bstatus(msg,'dirty');
  try{
    const r=await fetch('/api/bonds',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
    if(r.ok){bstatus('Done ✓','ok');loadBonds()}
    else bstatus('failed','err');
  }catch(e){bstatus('failed','err')}
}
function renameBond(addr,name){
  postBonds('action=rename&addr='+addr+'&name='+encodeURIComponent(name),'saving…');
}
function forgetBond(addr,label){
  if(!confirm('Forget "'+label+'"?\nYou will need to re-pair it (Share + PS).'))return;
  postBonds('action=forget&addr='+addr,'forgetting…');
}
$('pair').onclick=()=>{
  if(!confirm('Pair a new controller?\nThe controller you are using now will disconnect (it stays remembered and reconnects later). Then put the new controller in pairing mode (hold Share + PS until the light bar flashes).'))return;
  postBonds('action=pair','opening pairing…');
};
$('forgetall').onclick=()=>{
  if(!confirm('Forget ALL paired controllers?\nEach will need to be re-paired.'))return;
  postBonds('action=forgetall','forgetting all…');
};

// ----- Network (device name) -----
function nstatus(msg,cls){const s=$('nstatus');s.className=cls||'';s.textContent=msg}
$('net_save').onclick=async()=>{
  const fields=[
    'hostname='+encodeURIComponent($('hostname').value.trim())
  ];
  nstatus('saving…','dirty');
  try{
    const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:fields.join('&')});
    if(r.ok)nstatus('Saved ✓ (reboot adapter to apply)','ok');
    else nstatus('save failed','err');
  }catch(e){nstatus('save failed','err')}
};

function wrstatus(msg,cls){const s=$('wrstatus');s.className=cls||'';s.textContent=msg}
$('wifi_reset').onclick=async()=>{
  if(!confirm('Reset saved WiFi credentials?\nThe adapter will reboot into setup AP mode.'))return;
  wrstatus('resetting...','dirty');
  $('wifi_reset').disabled=true;
  try{
    const r=await fetch('/api/wifi_reset',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'action=reset'});
    const d=await r.json();
    if(r.ok&&d.ok)wrstatus('Rebooting to setup AP...','ok');
    else{wrstatus('reset failed','err');$('wifi_reset').disabled=false}
  }catch(e){
    wrstatus('Rebooting to setup AP...','ok');
  }
};

// ----- Wake-on-LAN -----
function wstatus(msg,cls){const s=$('wstatus');s.className=cls||'';s.textContent=msg}
// Normalize "AA:BB:..", "aa-bb-..", "aabb.." -> 12 upper-hex chars, or '' if invalid.
function macHex(s){const h=s.replace(/[:\-.\s]/g,'').toUpperCase();return /^[0-9A-F]{12}$/.test(h)?h:''}
$('wol_save').onclick=async()=>{
  const raw=$('wol_target_mac').value.trim();
  // Empty MAC is allowed (clears the WOL target -> all-zero "unset"); only a
  // non-empty-but-malformed MAC is an error.
  let h='000000000000';
  if(raw){h=macHex(raw);if(!h){wstatus('invalid MAC','err');return}}
  wstatus('saving…','dirty');
  try{
    const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'wol_target_mac='+h});
    if(r.ok){wstatus('Saved ✓','ok');if(raw)$('wol_target_mac').value=fmtAddr(h)}
    else wstatus('save failed','err');
  }catch(e){wstatus('save failed','err')}
};
$('wol_wake').onclick=async()=>{
  const h=macHex($('wol_target_mac').value); // send the field value if set, else server uses stored
  wstatus('sending magic packet…','dirty');
  try{
    const r=await fetch('/api/wol',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'action=wake'+(h?'&mac='+h:'')});
    if(r.ok)wstatus('Magic packet sent ✓','ok');
    else wstatus('send failed','err');
  }catch(e){wstatus('send failed','err')}
};
$('wol_resolve').onclick=async()=>{
  const ip=$('wol_resolve_ip').value.trim();
  if(!/^\d{1,3}(\.\d{1,3}){3}$/.test(ip)){wstatus('enter a valid IP first','err');return}
  wstatus('looking up MAC…','dirty');
  $('wol_resolve').disabled=true;
  try{
    await fetch('/api/resolve_mac',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ip='+encodeURIComponent(ip)});
    // The POST only starts the ARP lookup (the adapter can't block the
    // response on it); poll the GET until it reports a final answer.
    let r=null;
    for(let i=0;i<20;i++){
      r=await (await fetch('/api/resolve_mac')).json();
      if(!r.pending)break;
      await new Promise(res=>setTimeout(res,100));
    }
    if(r&&!r.pending&&r.ok){$('wol_target_mac').value=fmtAddr(r.mac);wstatus('Found MAC '+fmtAddr(r.mac)+' ✓','ok')}
    else wstatus('no reply from that IP (is it online?)','err');
  }catch(e){wstatus('lookup failed','err')}
  finally{$('wol_resolve').disabled=false}
};

// ----- Live status (GET /api/status) -----
async function loadStatus(){
  try{
    const s=await (await fetch('/api/status')).json();
    const card=$('statuscard');
    card.className=s.connected?'on':'';
    if(s.connected){
      $('st_conn').innerHTML='<b>'+(s.model==='DSE'?'DualSense Edge':'DualSense')+'</b> connected';
      if(s.battery_valid){
        $('st_batt').style.display='';
        $('st_pct').textContent=s.battery_pct+'%'+(s.charging?' ⚡':'');
        const f=$('st_fill');f.style.width=s.battery_pct+'%';
        $('st_batt').className='s batt'+((s.battery_pct<=20&&!s.charging)?' low':'');
      }else{$('st_batt').style.display='none'}
    }else{
      $('st_conn').textContent='No controller connected';
      $('st_batt').style.display='none';
    }
  }catch(e){$('st_conn').textContent='status unavailable'}
}

load();
loadBonds();
loadStatus();
setInterval(loadStatus,4000);
</script>
<footer>
  <a href="https://github.com/kungaa/ds5-linux-bridge" target="_blank" rel="noopener">GitHub</a>
  <a class="kofi" href="https://ko-fi.com/mkungaa" target="_blank" rel="noopener">☕ Support on Ko-fi</a>
</footer>
</body></html>
)rawhtml";

#endif // DS5_BRIDGE_WEB_PAGE_H
