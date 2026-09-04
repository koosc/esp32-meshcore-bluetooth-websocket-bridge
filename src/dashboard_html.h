#pragma once
#include <Arduino.h>

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>ESP32 MeshCore BLE Bridge</title>
<style>
:root { --bg: #0d1117; --panel: #161b22; --border: #30363d; --text: #c9d1d9; --accent: #58a6ff; --green: #238636; --green-txt: #3fb950; --red: #da3633; --yellow: #d29922; }
* { box-sizing: border-box; }
body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 16px; }
.container { max-width: 920px; margin: 0 auto; display: flex; flex-direction: column; gap: 16px; }
.header { display: flex; justify-content: space-between; align-items: center; background: var(--panel); border: 1px solid var(--border); border-radius: 8px; padding: 12px 16px; flex-wrap: wrap; gap: 10px; }
.title { font-size: 19px; font-weight: bold; color: var(--accent); display: flex; align-items: center; gap: 8px; }
.badge { padding: 4px 10px; border-radius: 12px; font-size: 12px; font-weight: bold; display: inline-flex; align-items: center; gap: 4px; }
.bg-green { background: rgba(35,134,54,0.2); color: var(--green-txt); border: 1px solid var(--green); }
.bg-red { background: rgba(218,54,51,0.2); color: #f85149; border: 1px solid var(--red); }
.bg-yellow { background: rgba(210,153,34,0.2); color: var(--yellow); border: 1px solid var(--yellow); }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 16px; }
.card { background: var(--panel); border: 1px solid var(--border); border-radius: 8px; padding: 16px; }
.card-title { font-size: 13px; font-weight: bold; text-transform: uppercase; color: #8b949e; letter-spacing: 0.5px; margin-bottom: 12px; }
.row { display: flex; justify-content: space-between; padding: 6px 0; border-bottom: 1px solid rgba(255,255,255,0.05); font-size: 13px; }
.row:last-child { border-bottom: none; }
.label { color: #8b949e; }
.val { font-family: monospace; font-weight: bold; color: #f0f6fc; }
.btn { background: #21262d; border: 1px solid var(--border); color: #c9d1d9; padding: 8px 14px; border-radius: 6px; font-size: 13px; font-weight: 500; cursor: pointer; transition: all 0.2s; display: inline-flex; align-items: center; gap: 6px; }
.btn:hover { background: #30363d; border-color: #8b949e; }
.btn-primary { background: var(--green); border-color: rgba(240,246,252,0.1); color: #fff; }
.btn-primary:hover { background: #2ea043; }
.btn-blue { background: #1f6feb; border-color: rgba(240,246,252,0.1); color: #fff; }
.btn-blue:hover { background: #388bfd; }
.btn-red { background: rgba(218,54,51,0.2); border-color: var(--red); color: #f85149; }
.btn-red:hover { background: var(--red); color: #fff; }
.btn-group { display: flex; flex-wrap: wrap; gap: 8px; }
input, select, textarea { background: #0d1117; border: 1px solid var(--border); color: #c9d1d9; padding: 8px 12px; border-radius: 6px; font-size: 14px; width: 100%; outline: none; }
input:focus, select:focus, textarea:focus { border-color: var(--accent); }
.chat-box { height: 260px; overflow-y: auto; background: #0d1117; border: 1px solid var(--border); border-radius: 6px; padding: 12px; display: flex; flex-direction: column; gap: 8px; }
.chat-msg { background: #161b22; border: 1px solid var(--border); border-radius: 6px; padding: 8px 12px; font-size: 13px; }
.chat-msg.tx { border-left: 3px solid var(--accent); }
.chat-msg.rx { border-left: 3px solid var(--green-txt); }
.chat-msg.sys { border-left: 3px solid var(--yellow); color: #8b949e; font-size: 12px; }
.chat-sender { font-weight: bold; color: var(--accent); margin-bottom: 2px; display: flex; justify-content: space-between; font-size: 12px; }
.log-box { height: 220px; overflow-y: auto; background: #0d1117; border: 1px solid var(--border); border-radius: 6px; padding: 8px; font-family: monospace; font-size: 12px; }
.log-line { padding: 3px 6px; border-radius: 4px; margin-bottom: 2px; }
.log-tx { color: #58a6ff; background: rgba(88,166,255,0.08); }
.log-rx { color: #3fb950; background: rgba(63,185,80,0.08); }
.log-err { color: #f85149; background: rgba(248,81,73,0.08); }
.log-info { color: #8b949e; }
.device-card { background: #0d1117; border: 1px solid var(--border); border-radius: 6px; padding: 10px 14px; cursor: pointer; transition: all 0.2s; }
.device-card:hover { border-color: var(--accent); background: #161b22; }
.device-connected { border-color: var(--green) !important; background: rgba(35,134,54,0.08) !important; }
</style></head><body>
<div class='container'>
<div class='header'>
<div style='display:flex; align-items:center; gap:16px; flex-wrap:wrap;'>
<div class='title'>⚡ MeshCore BLE Bridge</div>
<div class='btn-group'>
<button id='btnTabConsole' class='btn btn-primary' onclick='switchTab("console")'>📡 Mesh Console</button>
<button id='btnTabConfig' class='btn' onclick='switchTab("config")'>⚙️ Bluetooth & Config</button>
</div></div>
<div style='display:flex; gap:8px; flex-wrap:wrap;'>
<span id='wifiHeaderBadge' class='badge bg-yellow' style='cursor:pointer;' onclick='switchTab("config")'>WiFi: Checking...</span>
<span id='bleBadge' class='badge bg-yellow' style='cursor:pointer;' onclick='switchTab("config")'>BLE: Connecting...</span>
<span id='wsBadge' class='badge bg-yellow'>WS: Connecting...</span>
</div></div>
<div id='tabConsole' style='display:flex; flex-direction:column; gap:16px;'>
<div class='grid'>
<div class='card'>
<div class='card-title'>Connected Radio Profile</div>
<div class='row'><span class='label'>Node Name</span><span class='val' id='valNodeName'>-</span></div>
<div class='row'><span class='label'>Public Key</span><span class='val' id='valPubKey' style='font-size:11px; word-break:break-all;'>-</span></div>
<div class='row'><span class='label'>Battery</span><span class='val' id='valBattery'>-</span></div>
<div class='row'><span class='label'>LoRa Frequency</span><span class='val' id='valFreq'>-</span></div>
<div class='row'><span class='label'>Bandwidth / SF / CR</span><span class='val' id='valRadioParams'>-</span></div>
<div class='row'><span class='label'>TX Power</span><span class='val' id='valTxPower'>-</span></div>
<div class='btn-group' style='margin-top:12px;'>
<button class='btn btn-primary' onclick='cmdAppStart()'>🔄 Query Node</button>
<button class='btn' onclick='cmdSendAdvert()'>📡 Send Beacon / Advert</button>
<button class='btn' onclick='cmdGetBattery()'>🔋 Battery</button>
<button class='btn' onclick='cmdSyncTime()'>⏱️ Sync Time</button>
</div></div>
<div class='card'>
<div class='card-title'>Bridge Endpoints</div>
<div class='row'><span class='label'>Network Mode</span><span class='val' id='valNetMode'>-</span></div>
<div class='row'><span class='label'>WiFi / AP SSID</span><span class='val' id='valWiFiSSID'>-</span></div>
<div class='row'><span class='label'>Bridge IP</span><span class='val' id='valBridgeIP'>-</span></div>
<div class='row'><span class='label'>Native TCP Port</span><span class='val' id='valTcpPort'>-</span></div>
<div class='row'><span class='label'>WebSocket Port</span><span class='val' id='valWsPort'>-</span></div>
<div class='row'><span class='label'>BLE Device</span><span class='val' id='valBleDev'>-</span></div>
<div class='row'><span class='label'>BLE Packets (RX / TX)</span><span class='val' id='valBlePackets'>-</span></div>
<div class='btn-group' style='margin-top:12px;'>
<button class='btn' onclick='cmdPollMessages()'>📩 Check Messages</button>
<button class='btn' onclick='location.reload()'>🔄 Refresh Page</button>
</div></div>
</div>
<div class='card'>
<div class='card-title'>Channel Messages & Mesh Chat</div>
<div class='chat-box' id='chatBox'>
<div class='chat-msg sys'>Mesh chat initialized. Send a message below to broadcast over LoRa mesh!</div>
</div>
<div style='display:flex; gap:8px; margin-top:10px;'>
<select id='channelSelect' style='width:140px;'>
<option value='0'>Channel 0 (Public)</option>
<option value='1'>Channel 1</option>
<option value='2'>Channel 2</option>
<option value='3'>Channel 3</option>
</select>
<input type='text' id='msgInput' placeholder='Type a message to send over LoRa mesh...' onkeypress='if(event.key==="Enter") sendMessage()' />
<button class='btn btn-blue' onclick='sendMessage()'>Send 📤</button>
</div></div>
<div class='card'>
<div class='card-title' style='display:flex; justify-content:space-between; align-items:center;'>
<span>Live Protocol Packet Stream</span>
<button class='btn' style='padding:2px 8px; font-size:11px;' onclick='document.getElementById("logBox").innerHTML=""'>Clear</button>
</div>
<div class='log-box' id='logBox'></div>
<div style='display:flex; gap:8px; margin-top:8px;'>
<input type='text' id='hexInput' placeholder='Send raw Hex command (e.g. 01 04 00 00 00 00 00 00 42 72 69 64 67 65)' />
<button class='btn' onclick='sendRawHex()'>Send Hex</button>
</div></div>
</div>
<div id='tabConfig' style='display:none; flex-direction:column; gap:16px;'>
<div class='card' id='wifiSetupCard' style='border: 1px solid #1f6feb;'>
<div style='display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px; margin-bottom:12px;'>
<div class='card-title' style='margin-bottom:0; color:#58a6ff;'>📶 Wi-Fi Network Configuration & Soft AP</div>
<span id='wifiModeBadge' class='badge bg-yellow'>Detecting...</span>
</div>
<div id='wifiStatusDesc' style='font-size:13px; color:#c9d1d9; margin-bottom:12px;'>
Select your local Wi-Fi network below or enter credentials manually to connect the bridge.
</div>
<div class='grid' style='grid-template-columns: 1fr 1fr; gap:12px;'>
<div>
<div style='display:flex; justify-content:space-between; align-items:center; margin-bottom:4px;'>
<label class='label'>Select Wi-Fi Network</label>
<button class='btn' style='padding:2px 8px; font-size:11px;' onclick='scanWiFi()'>🔄 Scan Wi-Fi</button>
</div>
<select id='wifiSelect' style='margin-bottom:6px;' onchange='onSelectWiFiNetwork()'>
<option value=''>-- Click 'Scan Wi-Fi' to discover networks --</option>
</select>
<input type='text' id='wifiManualSsid' placeholder='Or enter SSID manually...' />
</div>
<div>
<label class='label' style='display:block; margin-bottom:4px;'>Wi-Fi Password</label>
<div style='display:flex; gap:6px;'>
<input type='password' id='wifiPass' placeholder='Enter Wi-Fi password (blank if open)' />
<button class='btn' type='button' style='padding:4px 10px; font-size:12px;' onclick='togglePassVisibility()'>👁️</button>
</div>
</div></div>
<div style='margin-top:12px; display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:12px;'>
<div id='wifiFeedback' style='font-size:13px; font-family:monospace; min-height:20px;'></div>
<div class='btn-group'>
<button class='btn btn-primary' id='btnSaveWifi' onclick='saveWiFi()'>💾 Save & Connect</button>
<button class='btn btn-red' id='btnForgetWifi' onclick='forgetWiFi()'>🗑️ Forget Wi-Fi & Start Soft AP</button>
</div></div>
</div>
<div class='card'>
<div class='card-title'>Active Bluetooth Connection</div>
<div id='activeConnBox' style='display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:12px;'>
<div>
<div style='font-size:16px; font-weight:bold; color:#f0f6fc;' id='activeDevName'>No device connected</div>
<div style='font-family:monospace; font-size:13px; color:#8b949e;' id='activeDevAddr'>-</div>
</div>
<div style='display:flex; gap:8px; align-items:center;'>
<span id='activeDevRSSI' class='badge bg-yellow'>Disconnected</span>
<button id='btnDisconnect' class='btn btn-red' style='display:none;' onclick='disconnectTarget()'>🔌 Disconnect</button>
</div></div></div>
<div class='card'>
<div style='display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px; margin-bottom:12px;'>
<div class='card-title' style='margin-bottom:0;'>Discovered Bluetooth Devices</div>
<div style='display:flex; align-items:center; gap:12px; flex-wrap:wrap;'>
<label style='font-size:13px; color:#c9d1d9; display:flex; align-items:center; gap:6px; cursor:pointer;'>
<input type='checkbox' id='showAllCheck' style='width:auto; cursor:pointer;' onchange='renderDevices()' />
Show all Bluetooth devices (unfiltered)
</label>
<button id='scanBtn' class='btn btn-blue' onclick='startScan()'>🔍 Scan for Devices</button>
</div></div>
<div id='scanSummary' style='font-size:12px; color:#8b949e; margin-bottom:10px;'>Click 'Scan for Devices' to discover nearby BLE devices.</div>
<div id='deviceList' style='display:flex; flex-direction:column; gap:8px; max-height:360px; overflow-y:auto; padding-right:4px;'>
<div style='padding:20px; text-align:center; color:#8b949e;'>Click 'Scan for Devices' above to search for nearby Bluetooth devices.</div>
</div></div>
<div class='card'>
<div class='card-title'>Bluetooth Connection & PIN Configuration</div>
<div class='grid' style='grid-template-columns: 1fr 1fr; gap:12px;'>
<div>
<label class='label' style='display:block; margin-bottom:4px;'>Target Device MAC Address</label>
<input type='text' id='cfgTargetMac' placeholder='e.g. 24:4C:AB:12:34:56' />
</div>
<div>
<label class='label' style='display:block; margin-bottom:4px;'>BLE Pairing PIN / Passkey (Default: 808978)</label>
<input type='number' id='cfgPin' value='808978' placeholder='808978' />
</div></div>
<div style='margin-top:12px; display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:12px;'>
<label style='font-size:13px; color:#c9d1d9; display:flex; align-items:center; gap:6px; cursor:pointer;'>
<input type='checkbox' id='cfgAuto' checked style='width:auto; cursor:pointer;' />
Automatically reconnect to this device on boot
</label>
<div class='btn-group'>
<button class='btn btn-primary' onclick='connectTarget()'>🔗 Connect to Device</button>
<button class='btn' onclick='forgetTarget()'>🗑️ Forget Target</button>
</div></div>
<div id='connFeedback' style='margin-top:10px; font-size:13px; font-family:monospace; min-height:20px;'></div>
</div>
<div class='card'>
<div style='display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px; margin-bottom:12px;'>
<div class='card-title' style='margin-bottom:0;'>🗄️ Bluetooth Keystore & Bonding Cache</div>
<div style='display:flex; gap:8px;'>
<button class='btn btn-blue' onclick='fetchBonds()'>🔄 Refresh Keystore</button>
<button class='btn btn-red' onclick='clearAllBonds()'>🗑️ Clear All Stored Bonds</button>
</div></div>
<div style='font-size:12px; color:#8b949e; margin-bottom:12px;'>
The Bluetooth security keystore holds authenticated pairing keys and bonding records in NVS. If you change a device PIN or experience encryption status 261 errors, clearing the bond cache forces fresh PIN negotiation.
</div>
<div id='bondsList' style='display:flex; flex-direction:column; gap:8px;'>
<div style='padding:15px; text-align:center; color:#8b949e;'>Loading bonded devices keystore...</div>
</div>
<div style='margin-top:14px; padding-top:12px; border-top:1px solid #30363d; display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px;'>
<span style='font-size:12px; color:#8b949e;'>Scanner Cache: <b id='scanCacheCount'>0</b> device(s) in memory</span>
<button class='btn' style='padding:4px 10px; font-size:12px;' onclick='clearDiscoveredCache()'>🧹 Clear Scanner Cache</button>
</div>
<div id='cacheFeedback' style='margin-top:8px; font-size:13px; font-family:monospace; min-height:18px;'></div>
</div>
</div>
<script>
let ws = null;
let myNodeName = 'BridgeWeb';
let devicesList = [];
let bondsList = [];
let configPollInterval = null;
const logBox = document.getElementById('logBox');
const chatBox = document.getElementById('chatBox');
function scanWiFi() {
  const sel = document.getElementById('wifiSelect');
  sel.innerHTML = '<option value="">⏳ Scanning nearby Wi-Fi networks...</option>';
  function pollScan() {
    fetch('/api/wifi/scan').then(r => r.json()).then(d => {
      if (d.scanning) {
        setTimeout(pollScan, 800);
        return;
      }
      let opts = '<option value="">-- Select a Wi-Fi Network (' + (d.networks ? d.networks.length : 0) + ' found) --</option>';
      if (d.networks && d.networks.length > 0) {
        d.networks.sort((a,b) => b.rssi - a.rssi);
        const seen = new Set();
        d.networks.forEach(n => {
          if (!n.ssid || seen.has(n.ssid)) return;
          seen.add(n.ssid);
          const lock = n.secure ? '🔒' : '🔓';
          const signal = n.rssi > -65 ? '📶 Strong' : (n.rssi > -80 ? '📶 Medium' : '📶 Weak');
          opts += `<option value="${encodeURIComponent(n.ssid)}">${lock} ${n.ssid} (${n.rssi} dBm, ${signal})</option>`;
        });
      } else {
        opts += '<option value="">No networks found. Try scanning again.</option>';
      }
      sel.innerHTML = opts;
    }).catch(e => {
      sel.innerHTML = `<option value="">Scan failed: ${e}</option>`;
    });
  }
  pollScan();
}
function onSelectWiFiNetwork() {
  const sel = document.getElementById('wifiSelect');
  if (sel.value) {
    document.getElementById('wifiManualSsid').value = decodeURIComponent(sel.value);
  }
}
function togglePassVisibility() {
  const p = document.getElementById('wifiPass');
  p.type = p.type === 'password' ? 'text' : 'password';
}
function saveWiFi() {
  const ssid = document.getElementById('wifiManualSsid').value.trim();
  const pass = document.getElementById('wifiPass').value;
  if (!ssid) { alert('Please select or enter a Wi-Fi SSID.'); return; }
  const fb = document.getElementById('wifiFeedback');
  fb.innerHTML = `<span style="color:#d29922;">Connecting to "${ssid}"... Please wait.</span>`;
  const btn = document.getElementById('btnSaveWifi');
  btn.disabled = true;
  btn.textContent = '⏳ Connecting...';
  const params = new URLSearchParams();
  params.append('ssid', ssid);
  params.append('password', pass);
  fetch('/api/wifi/save', { method: 'POST', body: params }).then(r => r.json()).then(d => {
    fb.innerHTML = `<span style="color:#3fb950;">Credentials saved! Connecting to ${ssid}...</span>`;
    setTimeout(() => {
      btn.disabled = false;
      btn.textContent = '💾 Save & Connect';
      checkStatus();
    }, 4000);
  }).catch(e => {
    fb.innerHTML = `<span style="color:#da3633;">Connection error: ${e}</span>`;
    btn.disabled = false;
    btn.textContent = '💾 Save & Connect';
  });
}
function forgetWiFi() {
  if (!confirm('Forget saved Wi-Fi credentials and switch back to Soft AP mode?')) return;
  const fb = document.getElementById('wifiFeedback');
  fb.innerHTML = '<span style="color:#d29922;">Forgetting Wi-Fi network...</span>';
  fetch('/api/wifi/forget', { method: 'POST' }).then(r => r.json()).then(d => {
    fb.innerHTML = '<span style="color:#3fb950;">Wi-Fi credentials erased. Soft AP mode active.</span>';
    document.getElementById('wifiManualSsid').value = '';
    document.getElementById('wifiPass').value = '';
    setTimeout(checkStatus, 1500);
  });
}
function switchTab(tab) {
  document.getElementById('tabConsole').style.display = tab === 'console' ? 'flex' : 'none';
  document.getElementById('tabConfig').style.display = tab === 'config' ? 'flex' : 'none';
  document.getElementById('btnTabConsole').className = 'btn ' + (tab === 'console' ? 'btn-primary' : '');
  document.getElementById('btnTabConfig').className = 'btn ' + (tab === 'config' ? 'btn-primary' : '');
  if (tab === 'config') {
    fetchDevices();
    fetchBonds();
    if (!configPollInterval) configPollInterval = setInterval(() => { fetchDevices(); fetchBonds(); }, 3000);
  } else {
    if (configPollInterval) { clearInterval(configPollInterval); configPollInterval = null; }
  }
}
function fetchBonds() {
  fetch('/api/bonds').then(r => r.json()).then(d => {
    bondsList = d.bonds || [];
    renderBonds();
  }).catch(e => {
    console.error('fetchBonds error:', e);
    document.getElementById('bondsList').innerHTML = `<div style='color:#da3633; text-align:center; padding:10px;'>Failed to load keystore: ${e}</div>`;
  });
}
function renderBonds() {
  const el = document.getElementById('bondsList');
  if (bondsList.length === 0) {
    el.innerHTML = '<div style="padding:15px; text-align:center; color:#8b949e;">No stored bonding keys found in keystore.</div>';
    return;
  }
  let html = '';
  bondsList.forEach(b => {
    const authTag = b.authenticated ? '<span class="badge bg-green">MITM Authenticated</span>' : '<span class="badge bg-yellow">Unauthenticated (JustWorks)</span>';
    const scTag = b.sc ? '<span class="badge bg-green">Secure Conn</span>' : '<span class="badge" style="background:#30363d; color:#8b949e;">Legacy Sec</span>';
    const ltkTag = b.ltk_present ? '<span class="badge" style="background:#1f6feb; color:#fff;">LTK Bond</span>' : '';
    const irkTag = b.irk_present ? '<span class="badge" style="background:#6f42c1; color:#fff;">IRK Identity</span>' : '';
    html += `<div style="background:#0d1117; border:1px solid #30363d; border-radius:6px; padding:10px 14px; display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px;">`;
    html += `<div><div style="font-family:monospace; font-weight:bold; font-size:14px; color:#f0f6fc;">${b.address}</div>`;
    html += `<div style="display:flex; gap:6px; margin-top:5px; flex-wrap:wrap;">${authTag} ${scTag} ${ltkTag} ${irkTag} <span class="badge" style="background:#21262d; color:#8b949e;">Key: ${b.key_size * 8} bits</span></div></div>`;
    html += `<button class="btn btn-red" style="padding:4px 10px; font-size:12px;" onclick="deleteBond('${b.address}')">Delete Bond 🗑️</button>`;
    html += `</div>`;
  });
  el.innerHTML = html;
}
function deleteBond(mac) {
  if (!confirm(`Delete bonding key for ${mac} from keystore?`)) return;
  const fb = document.getElementById('cacheFeedback');
  fb.innerHTML = `<span style="color:#d29922;">Deleting bond for ${mac}...</span>`;
  const params = new URLSearchParams();
  params.append('mac', mac);
  fetch('/api/bonds/delete', { method: 'POST', body: params }).then(r => r.json()).then(d => {
    fb.innerHTML = `<span style="color:#3fb950;">Bond deleted for ${mac}.</span>`;
    fetchBonds();
  }).catch(e => {
    fb.innerHTML = `<span style="color:#da3633;">Error deleting bond: ${e}</span>`;
  });
}
function clearAllBonds() {
  if (!confirm('Clear ALL stored security bonds from keystore? This will disconnect any active BLE connection.')) return;
  const fb = document.getElementById('cacheFeedback');
  fb.innerHTML = '<span style="color:#d29922;">Clearing security keystore...</span>';
  fetch('/api/bonds/clear', { method: 'POST' }).then(r => r.json()).then(d => {
    fb.innerHTML = '<span style="color:#3fb950;">All security bonds successfully erased from keystore.</span>';
    fetchBonds();
    checkStatus();
  }).catch(e => {
    fb.innerHTML = `<span style="color:#da3633;">Error clearing keystore: ${e}</span>`;
  });
}
function clearDiscoveredCache() {
  fetch('/api/cache/clear-devices', { method: 'POST' }).then(r => r.json()).then(d => {
    document.getElementById('cacheFeedback').innerHTML = '<span style="color:#3fb950;">Discovered devices cache cleared.</span>';
    devicesList = [];
    renderDevices();
  });
}
function fetchDevices() {
  fetch('/api/devices').then(r => r.json()).then(d => {
    devicesList = d.devices || [];
    const targetInput = document.getElementById('cfgTargetMac');
    if (d.target_mac && !targetInput.value) {
      targetInput.value = d.target_mac;
    }
    const pinInput = document.getElementById('cfgPin');
    if (d.pin && !pinInput.dataset.userEdited) pinInput.value = d.pin;
    if (typeof d.auto_conn !== 'undefined') document.getElementById('cfgAuto').checked = d.auto_conn;
    renderDevices();
    const btn = document.getElementById('scanBtn');
    if (d.scanning) {
      btn.textContent = '⏳ Scanning...';
      btn.disabled = true;
    } else {
      btn.textContent = '🔍 Scan for Devices';
      btn.disabled = false;
    }
  }).catch(e => console.error('fetchDevices error:', e));
}
function startScan() {
  const btn = document.getElementById('scanBtn');
  btn.textContent = '⏳ Scanning...';
  btn.disabled = true;
  document.getElementById('scanSummary').textContent = 'Scanning for nearby Bluetooth devices...';
  fetch('/api/scan', { method: 'POST' }).then(() => {
    fetchDevices();
  }).catch(e => {
    console.error('Scan error:', e);
    btn.textContent = '🔍 Scan for Devices';
    btn.disabled = false;
  });
}
function renderDevices() {
  const showAll = document.getElementById('showAllCheck').checked;
  const listEl = document.getElementById('deviceList');
  const filtered = devicesList.filter(d => showAll || d.is_mesh);
  const meshCount = devicesList.filter(d => d.is_mesh).length;
  document.getElementById('scanSummary').textContent = `Found ${devicesList.length} device(s) (${meshCount} MeshCore candidate(s))`;
  if (filtered.length === 0) {
    listEl.innerHTML = `<div style='padding:20px; text-align:center; color:#8b949e;'>${devicesList.length > 0 ? 'No MeshCore devices match candidate filter. Check "Show all Bluetooth devices" to see all ' + devicesList.length + ' nearby devices.' : 'No devices found yet. Click "Scan for Devices" above.'}</div>`;
    return;
  }
  filtered.sort((a,b) => b.rssi - a.rssi);
  let html = '';
  filtered.forEach(d => {
    const isConn = d.connected;
    const name = d.name || 'Unnamed Device';
    const tag = d.is_mesh ? `<span class='badge bg-green'>MeshCore / NUS</span>` : `<span class='badge' style='background:#30363d; color:#8b949e;'>Generic BLE</span>`;
    const rssiColor = d.rssi > -70 ? '#3fb950' : (d.rssi > -85 ? '#d29922' : '#da3633');
    const encName = encodeURIComponent(name);
    html += `<div class='device-card ${isConn ? "device-connected" : ""}' onclick='selectDevice("${d.address}", "${encName}")'>`;
    html += `<div style='display:flex; justify-content:space-between; align-items:center;'><div>`;
    html += `<div style='font-weight:600; font-size:14px; color:#f0f6fc; display:flex; align-items:center; gap:8px;'>${d.is_mesh ? '📻' : '📱'} ${name} ${tag} ${isConn ? '<span class="badge bg-green">Active</span>' : ''}</div>`;
    html += `<div style='font-family:monospace; font-size:12px; color:#8b949e; margin-top:3px;'>${d.address}</div></div>`;
    html += `<div style='text-align:right;'><span style='font-family:monospace; font-weight:bold; color:${rssiColor}; font-size:13px;'>${d.rssi} dBm</span>`;
    html += `<div style='margin-top:4px;'><button class='btn ${isConn ? "btn-blue" : "btn-primary"}' style='padding:4px 10px; font-size:12px;' onclick='event.stopPropagation(); selectAndConnect("${d.address}", "${encName}")'>${isConn ? 'Connected' : 'Connect 🔗'}</button></div>`;
    html += `</div></div></div>`;
  });
  listEl.innerHTML = html;
}
function selectDevice(addr, encName) {
  const name = decodeURIComponent(encName || '');
  document.getElementById('cfgTargetMac').value = addr;
  document.getElementById('connFeedback').innerHTML = `<span style='color:#58a6ff;'>Selected: <b>${name || addr}</b> (${addr})</span>`;
}
function selectAndConnect(addr, encName) {
  selectDevice(addr, encName);
  connectTarget();
}
function connectTarget() {
  const addr = document.getElementById('cfgTargetMac').value.trim();
  const pin = document.getElementById('cfgPin').value.trim() || '808978';
  const autoConn = document.getElementById('cfgAuto').checked;
  if (!addr) { alert('Please enter or select a Bluetooth device MAC address.'); return; }
  const fb = document.getElementById('connFeedback');
  fb.innerHTML = `<span style='color:#d29922;'>Connecting to ${addr} with PIN ${pin}...</span>`;
  const params = new URLSearchParams();
  params.append('address', addr);
  params.append('pin', pin);
  params.append('save', 'true');
  params.append('auto', autoConn ? 'true' : 'false');
  fetch('/api/connect', { method: 'POST', body: params }).then(r => r.json()).then(d => {
    fb.innerHTML = `<span style='color:#3fb950;'>Connection initiated to ${addr}!</span>`;
    setTimeout(fetchDevices, 2000);
    setTimeout(checkStatus, 2000);
  }).catch(e => { fb.innerHTML = `<span style='color:#da3633;'>Connection error: ${e}</span>`; });
}
function disconnectTarget() {
  const fb = document.getElementById('connFeedback');
  fb.innerHTML = `<span style='color:#d29922;'>Disconnecting...</span>`;
  fetch('/api/disconnect', { method: 'POST' }).then(r => r.json()).then(() => {
    fb.innerHTML = `<span style='color:#8b949e;'>Disconnected.</span>`;
    fetchDevices();
    checkStatus();
  });
}
function forgetTarget() {
  if (!confirm('Forget configured target Bluetooth device?')) return;
  fetch('/api/forget', { method: 'POST' }).then(r => r.json()).then(() => {
    document.getElementById('cfgTargetMac').value = '';
    document.getElementById('connFeedback').innerHTML = `<span style='color:#8b949e;'>Target device forgotten.</span>`;
    fetchDevices();
    checkStatus();
  });
}
function log(msg, type='info') {
  const time = new Date().toLocaleTimeString();
  const div = document.createElement('div');
  div.className = 'log-line log-' + type;
  div.textContent = `[${time}] ${msg}`;
  logBox.appendChild(div);
  logBox.scrollTop = logBox.scrollHeight;
}
function addChat(sender, text, type='rx', timeStr) {
  const time = timeStr || new Date().toLocaleTimeString();
  const div = document.createElement('div');
  div.className = 'chat-msg ' + type;
  div.innerHTML = `<div class='chat-sender'><span>${sender}</span><span>${time}</span></div><div>${text}</div>`;
  chatBox.appendChild(div);
  chatBox.scrollTop = chatBox.scrollHeight;
}
function wrap(payload) {
  const buf = new Uint8Array(3 + payload.length);
  buf[0] = 0x3c;
  buf[1] = payload.length & 0xff;
  buf[2] = (payload.length >> 8) & 0xff;
  buf.set(payload, 3);
  return buf;
}
function send(payload, label='CMD') {
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    log('Cannot send: WebSocket disconnected', 'err');
    return;
  }
  const packet = wrap(payload);
  ws.send(packet.buffer);
  const hex = Array.from(payload).map(b => b.toString(16).padStart(2,'0').toUpperCase()).join(' ');
  log(`TX [${label}]: ${hex}`, 'tx');
}
function cmdAppStart() {
  const nameBytes = new TextEncoder().encode('WebConsole');
  const payload = new Uint8Array(8 + nameBytes.length);
  payload[0] = 0x01; payload[1] = 0x04;
  payload.set(nameBytes, 8);
  send(payload, 'CMD_APP_START');
}
function cmdSendAdvert() {
  send(new Uint8Array([0x07]), 'CMD_SEND_ADVERT');
  addChat('System', 'Broadcasted node advertisement/beacon over mesh', 'sys');
}
function cmdGetBattery() {
  send(new Uint8Array([0x14]), 'CMD_GET_BATTERY');
}
function cmdSyncTime() {
  const now = Math.floor(Date.now() / 1000);
  const p = new Uint8Array([0x12, now & 0xff, (now>>8)&0xff, (now>>16)&0xff, (now>>24)&0xff]);
  send(p, 'CMD_SET_TIME');
  addChat('System', 'Synchronized node clock with local time', 'sys');
}
function cmdPollMessages() {
  send(new Uint8Array([0x0a]), 'CMD_SYNC_NEXT_MESSAGE');
}
function sendMessage() {
  const input = document.getElementById('msgInput');
  const text = input.value.trim();
  if (!text) return;
  const chan = parseInt(document.getElementById('channelSelect').value, 10);
  const txtBytes = new TextEncoder().encode(text);
  const ts = Math.floor(Date.now() / 1000);
  const p = new Uint8Array(7 + txtBytes.length);
  p[0] = 0x03; p[1] = 0x00; p[2] = chan & 0x07;
  p[3] = ts & 0xff; p[4] = (ts>>8)&0xff; p[5] = (ts>>16)&0xff; p[6] = (ts>>24)&0xff;
  p.set(txtBytes, 7);
  send(p, `SEND_CHAN_MSG[ch${chan}]`);
  addChat(`Me (Ch ${chan})`, text, 'tx');
  input.value = '';
}
function sendRawHex() {
  const str = document.getElementById('hexInput').value.replace(/\s+/g, '');
  if (!str) return;
  const bytes = new Uint8Array(str.match(/.{1,2}/g).map(byte => parseInt(byte, 16)));
  send(bytes, 'RAW_HEX');
}
function parsePacket(bytes) {
  let p = bytes;
  if (bytes.length >= 3 && bytes[0] === 0x3e) {
    const len = bytes[1] | (bytes[2] << 8);
    p = bytes.slice(3, 3 + len);
  }
  const hex = Array.from(p).map(b => b.toString(16).padStart(2,'0').toUpperCase()).join(' ');
  const code = p[0];
  log(`RX [Code 0x${code.toString(16).padStart(2,'0')}]: ${hex}`, 'rx');
  if (code === 0x05) {
    const pubKeyHex = Array.from(p.slice(4, 36)).map(b => b.toString(16).padStart(2,'0')).join('');
    document.getElementById('valPubKey').textContent = pubKeyHex;
    if (p.length >= 60) {
      const nameLen = p[57];
      if (nameLen > 0 && p.length >= 58 + nameLen) {
        const name = new TextDecoder().decode(p.slice(58, 58 + nameLen));
        document.getElementById('valNodeName').textContent = name;
      }
    }
    const freq = ((p[44] | (p[45]<<8) | (p[46]<<16) | (p[47]<<24)) / 1000).toFixed(2);
    const bw = ((p[48] | (p[49]<<8) | (p[50]<<16) | (p[51]<<24)) / 1000).toFixed(1);
    const sf = p[52];
    const cr = p[53];
    const txPower = p[2];
    document.getElementById('valFreq').textContent = `${freq} MHz`;
    document.getElementById('valRadioParams').textContent = `BW ${bw}k / SF${sf} / CR 4/${cr}`;
    document.getElementById('valTxPower').textContent = `${txPower} dBm`;
    addChat('System', 'Received node profile info from MeshCore', 'sys');
  } else if (code === 0x06) {
    log('Message broadcast queued on mesh radio', 'info');
  } else if (code === 0x08 || code === 0x11) {
    const chan = p[4] || 0;
    const senderBytes = p.slice(5, 37);
    const senderHex = Array.from(senderBytes.slice(0,4)).map(b => b.toString(16).padStart(2,'0')).join('');
    const text = new TextDecoder().decode(p.slice(37));
    addChat(`Node ${senderHex} (Ch ${chan})`, text, 'rx');
  } else if (code === 0x0c) {
    const mv = p[1] | (p[2] << 8);
    document.getElementById('valBattery').textContent = `${(mv/1000).toFixed(2)} V (${mv} mV)`;
  } else if (code === 0x0a) {
    log('No more queued messages on node', 'info');
  }
}
function connectWS() {
  const wsUrl = 'ws://' + location.hostname + ':5001';
  log(`Connecting WebSocket to ${wsUrl}...`);
  ws = new WebSocket(wsUrl);
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => {
    document.getElementById('wsBadge').className = 'badge bg-green';
    document.getElementById('wsBadge').textContent = 'WS: Connected';
    log('WebSocket connected successfully!', 'info');
    setTimeout(cmdAppStart, 200);
  };
  ws.onclose = () => {
    document.getElementById('wsBadge').className = 'badge bg-red';
    document.getElementById('wsBadge').textContent = 'WS: Disconnected';
    setTimeout(connectWS, 3000);
  };
  ws.onerror = (e) => log('WebSocket error', 'err');
  ws.onmessage = async (e) => {
    const buf = new Uint8Array(e.data);
    parsePacket(buf);
  };
}
function checkStatus() {
  fetch('/status').then(r => r.json()).then(d => {
    const b = document.getElementById('bleBadge');
    const aName = document.getElementById('activeDevName');
    const aAddr = document.getElementById('activeDevAddr');
    const aRssi = document.getElementById('activeDevRSSI');
    const btnDisc = document.getElementById('btnDisconnect');
    const wBadge = document.getElementById('wifiHeaderBadge');
    const wModeBadge = document.getElementById('wifiModeBadge');
    const wDesc = document.getElementById('wifiStatusDesc');
    const btnForget = document.getElementById('btnForgetWifi');
    const valNetMode = document.getElementById('valNetMode');
    const valWiFiSSID = document.getElementById('valWiFiSSID');
    const valBridgeIP = document.getElementById('valBridgeIP');
    const valTcpPort = document.getElementById('valTcpPort');
    const valWsPort = document.getElementById('valWsPort');
    const valBleDev = document.getElementById('valBleDev');
    const valBlePackets = document.getElementById('valBlePackets');
    if (valNetMode) valNetMode.textContent = d.is_ap_mode ? 'Soft AP Mode' : 'Station Mode';
    if (valWiFiSSID) valWiFiSSID.textContent = d.is_ap_mode ? d.ap_ssid : (d.wifi_ssid + (d.wifi_rssi ? ` (${d.wifi_rssi} dBm)` : ''));
    if (valBridgeIP) valBridgeIP.textContent = d.ip_address || '-';
    if (valTcpPort) valTcpPort.textContent = `${d.ip_address}:${d.tcp_port}`;
    if (valWsPort) valWsPort.textContent = `ws://${d.ip_address}:${d.ws_port}`;
    if (valBleDev) valBleDev.textContent = `${d.ble_device_name || '-'} (${d.ble_rssi || 0} dBm)`;
    if (valBlePackets) valBlePackets.textContent = `${d.ble_rx} / ${d.ble_tx}`;
    if (d.is_ap_mode) {
      if (wBadge) { wBadge.className = 'badge bg-yellow'; wBadge.textContent = 'AP: ' + (d.ap_ssid || 'Setup'); }
      if (wModeBadge) { wModeBadge.className = 'badge bg-yellow'; wModeBadge.textContent = 'Soft AP: ' + d.ap_ssid; }
      if (wDesc) wDesc.innerHTML = `Bridge is in Soft AP mode (<b>${d.ap_ssid}</b> at <b>192.168.4.1</b>). Connect to your home Wi-Fi network below.`;
      if (btnForget) btnForget.style.display = 'none';
    } else if (d.wifi_connected) {
      if (wBadge) { wBadge.className = 'badge bg-green'; wBadge.textContent = 'WiFi: ' + d.wifi_ssid; }
      if (wModeBadge) { wModeBadge.className = 'badge bg-green'; wModeBadge.textContent = 'Connected: ' + d.wifi_ssid; }
      if (wDesc) wDesc.innerHTML = `Connected to <b>${d.wifi_ssid}</b> (IP: <b>${d.ip_address}</b> | RSSI: <b>${d.wifi_rssi} dBm</b>).`;
      if (btnForget) btnForget.style.display = 'inline-flex';
    } else {
      if (wBadge) { wBadge.className = 'badge bg-red'; wBadge.textContent = 'WiFi: Disconnected'; }
      if (wModeBadge) { wModeBadge.className = 'badge bg-red'; wModeBadge.textContent = 'Connecting...'; }
      if (wDesc) wDesc.innerHTML = 'Connecting to Wi-Fi...';
      if (btnForget) btnForget.style.display = 'inline-flex';
    }
    if (d.ble_connected) {
      b.className = 'badge bg-green';
      b.textContent = `BLE: ${d.ble_device_name} (${d.ble_rssi} dBm)`;
      aName.textContent = `Connected: ${d.ble_device_name}`;
      aAddr.textContent = `${d.ble_mac} (PIN: ${d.ble_pin})`;
      aRssi.className = 'badge bg-green';
      aRssi.textContent = `${d.ble_rssi} dBm`;
      btnDisc.style.display = 'inline-flex';
    } else if (d.ble_connecting) {
      b.className = 'badge bg-yellow';
      b.textContent = 'BLE: Connecting...';
      aName.textContent = 'Connecting to Bluetooth device...';
      aAddr.textContent = d.target_mac || 'Searching...';
      aRssi.className = 'badge bg-yellow';
      aRssi.textContent = 'Connecting';
      btnDisc.style.display = 'none';
    } else {
      b.className = 'badge bg-yellow';
      b.textContent = 'BLE: Disconnected';
      aName.textContent = 'No device connected';
      aAddr.textContent = d.target_mac ? `Target: ${d.target_mac}` : 'No target configured';
      aRssi.className = 'badge bg-red';
      aRssi.textContent = 'Disconnected';
      btnDisc.style.display = 'none';
    }
  }).catch(() => {});
}
window.onload = () => {
  connectWS();
  checkStatus();
  fetchDevices();
  setInterval(checkStatus, 4000);
};
</script>
</div></body></html>)rawliteral";
