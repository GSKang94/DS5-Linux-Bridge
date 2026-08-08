#ifndef EIGHTBITDO_WEB_PAGE_H
#define EIGHTBITDO_WEB_PAGE_H

// Brutalist web config UI for 8BitDo Bridge firmware.
static const char WEB_PAGE[] = R"rawhtml(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>8BitDo Bridge</title>
<style>
:root{color-scheme:dark}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'JetBrains Mono',monospace,system-ui;background:#000;color:#fff;max-width:520px;margin:0 auto;padding:2rem 1.5rem}
h1{font-size:1.6rem;font-weight:900;letter-spacing:-.02em;border-bottom:3px solid #fff;padding-bottom:.5rem;margin-bottom:1.5rem}
h2{font-size:.9rem;font-weight:700;text-transform:uppercase;letter-spacing:.1em;color:#888;margin:2rem 0 .8rem;border-left:3px solid #444;padding-left:.6rem}
.card{border:1px solid #333;padding:1rem;margin:.8rem 0}
.card.active{border-color:#0f0}
.status{display:flex;align-items:center;gap:.6rem}
.dot{width:10px;height:10px;border:2px solid #555}
.dot.on{background:#0f0;border-color:#0f0}
.lbl{font-size:.75rem;color:#888;text-transform:uppercase;letter-spacing:.08em;margin-bottom:.3rem}
.val{font-size:1.1rem;font-weight:700}
input[type=text]{background:#000;border:1px solid #555;color:#fff;font-family:inherit;font-size:.9rem;padding:.5rem;width:100%;margin-top:.2rem}
input[type=text]:focus{border-color:#fff;outline:none}
.chk{display:flex;align-items:center;gap:.5rem;margin:.6rem 0}
.chk input{width:18px;height:18px;accent-color:#fff}
.chk label{font-size:.85rem}
button{background:#fff;color:#000;border:0;font-family:inherit;font-weight:700;font-size:.8rem;text-transform:uppercase;letter-spacing:.05em;padding:.6rem 1.2rem;cursor:pointer;transition:background .1s}
button:hover{background:#ccc}
button:disabled{background:#333;color:#666;cursor:default}
button.danger{background:#f00;color:#fff}
button.danger:hover{background:#c00}
.row{display:flex;gap:.5rem;margin-top:.8rem;flex-wrap:wrap}
.msg{font-size:.8rem;margin-top:.4rem;min-height:1.2em}
.msg.ok{color:#0f0}
.msg.err{color:#f00}
.msg.wait{color:#888}
.field{margin:.8rem 0}
footer{margin-top:3rem;padding-top:1rem;border-top:1px solid #333;font-size:.7rem;color:#555}
footer a{color:#888}
</style></head><body>
<h1>8BITDO<br>BRIDGE</h1>

<div class="card" id="ctrl_card">
  <div class="status">
    <div class="dot" id="dot"></div>
    <span class="val" id="ctrl_status">Scanning...</span>
  </div>
</div>

<h2>TV Control</h2>
<div class="card">
  <div class="field">
    <div class="lbl">Server IP</div>
    <input type="text" id="tv_server_ip" placeholder="192.168.2.22">
  </div>
  <div class="chk">
    <input type="checkbox" id="tv_adb_enabled">
    <label for="tv_adb_enabled">Enable TV control</label>
  </div>
  <div class="chk">
    <input type="checkbox" id="tv_sleep_on_suspend">
    <label for="tv_sleep_on_suspend">Sleep TV when PC sleeps</label>
  </div>
  <div class="chk">
    <input type="checkbox" id="tv_input_on_wake">
    <label for="tv_input_on_wake">Switch input on wake</label>
  </div>
  <div class="row">
    <button id="tv_save">SAVE</button>
    <button id="tv_test_sleep">TEST SLEEP</button>
    <button id="tv_test_input">TEST INPUT</button>
  </div>
  <div class="msg" id="tv_msg"></div>
</div>

<h2>Wake-on-LAN</h2>
<div class="card">
  <div class="field">
    <div class="lbl">PC MAC</div>
    <input type="text" id="wol_target_mac" placeholder="AA:BB:CC:DD:EE:FF">
  </div>
  <div class="field">
    <div class="lbl">TV MAC</div>
    <input type="text" id="wol_target_mac2" placeholder="AA:BB:CC:DD:EE:FF">
  </div>
  <div class="row">
    <button id="wol_save">SAVE</button>
    <button id="wol_wake">WAKE NOW</button>
  </div>
  <div class="msg" id="wol_msg"></div>
</div>

<h2>Network</h2>
<div class="card">
  <div class="field">
    <div class="lbl">Hostname</div>
    <input type="text" id="hostname" placeholder="ds5" maxlength="10">
  </div>
  <div class="row">
    <button id="net_save">SAVE</button>
    <button id="wifi_reset" class="danger">RESET WIFI</button>
  </div>
  <div class="msg" id="net_msg"></div>
</div>

<h2>Firmware</h2>
<div class="card">
  <div class="field">
    <div class="lbl">Installed</div>
    <div class="val" id="fw_version">-</div>
  </div>
  <div class="row">
    <button id="ota_update">CHECK FOR UPDATES</button>
    <button id="switch_ds5" class="danger">SWITCH TO DUALSENSE</button>
  </div>
  <div class="msg" id="fw_msg"></div>
</div>

<footer>
  8BitDo Bridge &middot; <a href="https://github.com/GSKang94/DS5-Linux-Bridge">GitHub</a>
</footer>

<script>
const $=id=>document.getElementById(id);
function msg(el,text,cls){$(el).className='msg '+(cls||'');$(el).textContent=text}

async function load(){
  try{
    const c=await(await fetch('/api/config')).json();
    $('fw_version').textContent=c.version||'dev';
    if(c.tv_server_ip&&c.tv_server_ip!=='0.0.0.0')$('tv_server_ip').value=c.tv_server_ip;
    $('tv_adb_enabled').checked=!!c.tv_adb_enabled;
    $('tv_sleep_on_suspend').checked=!!c.tv_sleep_on_suspend;
    $('tv_input_on_wake').checked=!!c.tv_input_on_wake;
    if(c.hostname)$('hostname').value=c.hostname;
    if(c.wol_target_mac&&c.wol_target_mac!=='000000000000')
      $('wol_target_mac').value=c.wol_target_mac.match(/.{2}/g).join(':');
    if(c.wol_target_mac2&&c.wol_target_mac2!=='000000000000')
      $('wol_target_mac2').value=c.wol_target_mac2.match(/.{2}/g).join(':');
  }catch(e){}
}

async function loadStatus(){
  try{
    const s=await(await fetch('/api/status')).json();
    if(s.connected){
      $('dot').classList.add('on');
      $('ctrl_status').textContent='Connected';
      $('ctrl_card').classList.add('active');
    }else{
      $('dot').classList.remove('on');
      $('ctrl_status').textContent='Scanning...';
      $('ctrl_card').classList.remove('active');
    }
  }catch(e){}
}

async function post(body){
  return fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
}

$('tv_save').onclick=async()=>{
  msg('tv_msg','Saving...','wait');
  const b=[
    'tv_adb_enabled='+($('tv_adb_enabled').checked?1:0),
    'tv_server_ip='+encodeURIComponent($('tv_server_ip').value.trim()),
    'tv_sleep_on_suspend='+($('tv_sleep_on_suspend').checked?1:0),
    'tv_input_on_wake='+($('tv_input_on_wake').checked?1:0)
  ].join('&');
  try{const r=await post(b);msg('tv_msg',r.ok?'Saved':'Failed',r.ok?'ok':'err')}
  catch(e){msg('tv_msg','Failed','err')}
};

$('tv_test_sleep').onclick=async()=>{
  msg('tv_msg','Sending...','wait');
  try{const r=await post('tv_test=sleep');msg('tv_msg',r.ok?'Done':'Failed',r.ok?'ok':'err')}
  catch(e){msg('tv_msg','Failed','err')}
};
$('tv_test_input').onclick=async()=>{
  msg('tv_msg','Sending...','wait');
  try{const r=await post('tv_test=input');msg('tv_msg',r.ok?'Done':'Failed',r.ok?'ok':'err')}
  catch(e){msg('tv_msg','Failed','err')}
};

$('wol_save').onclick=async()=>{
  msg('wol_msg','Saving...','wait');
  const m1=($('wol_target_mac').value||'').replace(/[:\-]/g,'').padEnd(12,'0');
  const m2=($('wol_target_mac2').value||'').replace(/[:\-]/g,'').padEnd(12,'0');
  try{const r=await post('wol_target_mac='+m1+'&wol_target_mac2='+m2);msg('wol_msg',r.ok?'Saved':'Failed',r.ok?'ok':'err')}
  catch(e){msg('wol_msg','Failed','err')}
};
$('wol_wake').onclick=async()=>{
  msg('wol_msg','Sending...','wait');
  try{const r=await fetch('/api/wol',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'action=wake'});msg('wol_msg',r.ok?'Sent':'Failed',r.ok?'ok':'err')}
  catch(e){msg('wol_msg','Failed','err')}
};

$('net_save').onclick=async()=>{
  msg('net_msg','Saving...','wait');
  try{const r=await post('hostname='+encodeURIComponent($('hostname').value.trim()));msg('net_msg',r.ok?'Saved (reboot to apply)':'Failed',r.ok?'ok':'err')}
  catch(e){msg('net_msg','Failed','err')}
};
$('wifi_reset').onclick=async()=>{
  if(!confirm('Reset WiFi credentials? Device will enter setup mode.'))return;
  msg('net_msg','Resetting...','wait');
  try{await fetch('/api/wifi_reset',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'reset=1'});msg('net_msg','Rebooting into setup...','ok')}
  catch(e){msg('net_msg','Failed','err')}
};

$('ota_update').onclick=async()=>{
  msg('fw_msg','Checking...','wait');
  // Trigger OTA check/install
  try{
    const r=await fetch('/api/ota/start',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'go=1'});
    msg('fw_msg',r.ok?'Updating... do not unplug':'Failed','wait');
  }catch(e){msg('fw_msg','Failed','err')}
};

$('switch_ds5').onclick=async()=>{
  if(!confirm('Switch to DualSense firmware? The device will reboot.'))return;
  msg('fw_msg','Downloading DS5 firmware...','wait');
  try{
    const r=await fetch('/api/ota/start',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'go=1&asset=ds5-bridge-pico2w-ota.bin'});
    msg('fw_msg',r.ok?'Switching... do not unplug':'Failed','wait');
  }catch(e){msg('fw_msg','Failed','err')}
};

load();
loadStatus();
setInterval(loadStatus,3000);
</script>
</body></html>)rawhtml";

#endif // EIGHTBITDO_WEB_PAGE_H
