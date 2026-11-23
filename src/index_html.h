#pragma once
// The CGM's control panel (the board's own page at /cgm). It talks to the
// portable CGM core through /cgm/status and /cgm/command: the kind (Dexcom G6
// transmitter, Dexcom G7 sensor, off), a G6 transmitter id, the next reading
// and its trend, a calibration, the forced algorithm state, and the failure
// knob the radio applies.
static const char indexHTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CGM emulator (ESP32)</title>
<style>
  :root { color-scheme: dark; --bg:#14161a; --card:#1d2026; --line:#2b2f37; --fg:#e8eaed; --mut:#9aa0aa; --accent:#4f9cf8; --ok:#43c06d; --warn:#e0a53b; --bad:#e2574c; }
  * { box-sizing:border-box; }
  body { margin:0; font:15px/1.4 -apple-system,Segoe UI,Roboto,sans-serif; background:var(--bg); color:var(--fg); }
  header { padding:16px; border-bottom:1px solid var(--line); display:flex; flex-wrap:wrap; gap:4px 20px; align-items:baseline; }
  header h1 { font-size:18px; margin:0; }
  header .meta { color:var(--mut); font-size:13px; }
  nav { display:flex; flex-wrap:wrap; gap:2px 18px; align-items:baseline; padding:12px 16px; border-bottom:1px solid var(--line); max-width:760px; margin:0 auto; }
  nav .brand { font-weight:600; margin-right:auto; }
  nav a { color:var(--mut); text-decoration:none; font-size:14px; }
  nav a:hover { color:var(--fg); }
  nav a.active { color:var(--accent); }
  main { max-width:760px; margin:0 auto; padding:16px; display:grid; gap:16px; }
  .card { background:var(--card); border:1px solid var(--line); border-radius:10px; padding:16px; }
  .card h2 { font-size:13px; text-transform:uppercase; letter-spacing:.05em; color:var(--mut); margin:0 0 12px; }
  .glucose { display:flex; align-items:baseline; gap:12px; }
  .glucose .val { font-size:44px; font-weight:600; }
  .glucose .unit { color:var(--mut); }
  .glucose .state { margin-left:auto; padding:3px 10px; border-radius:20px; font-size:13px; background:#2b2f37; }
  .state.ok { background:rgba(67,192,109,.18); color:var(--ok); }
  .state.warn { background:rgba(224,165,59,.18); color:var(--warn); }
  .state.bad { background:rgba(226,87,76,.18); color:var(--bad); }
  label { display:block; font-size:13px; color:var(--mut); margin:10px 0 4px; }
  input[type=number], input[type=text], select { width:100%; padding:8px; background:#14161a; border:1px solid var(--line); border-radius:7px; color:var(--fg); font-size:15px; }
  input[type=range] { width:100%; }
  .row { display:flex; gap:12px; flex-wrap:wrap; }
  .row > div { flex:1; min-width:120px; }
  .seg { display:flex; gap:8px; }
  .seg button { flex:1; }
  button { padding:9px 14px; background:#2b2f37; color:var(--fg); border:1px solid var(--line); border-radius:7px; font-size:14px; cursor:pointer; }
  button:hover { border-color:var(--accent); }
  button.active { background:var(--accent); border-color:var(--accent); color:#0b0d10; font-weight:600; }
  button:disabled { opacity:.45; cursor:default; }
  button.primary { background:var(--accent); border-color:var(--accent); color:#0b0d10; font-weight:600; }
  button.danger:hover { border-color:var(--bad); }
  .muted { color:var(--mut); font-size:13px; }
  footer { max-width:760px; margin:0 auto; padding:4px 16px 24px; color:var(--mut); font-size:12px; }
</style>
</head>
<body>
<nav>
  <span class="brand">ESP-Sim</span>
  <a href="/">Home</a>
  <a class="active" href="/cgm">CGM</a>
  <a href="/pump">Pump</a>
  <a href="/settings">Settings</a>
</nav>
<header>
  <h1 id="name">CGM</h1>
  <span class="meta" id="model"></span>
  <span class="meta" id="id"></span>
  <span class="meta" id="age"></span>
  <span class="meta" id="link"></span>
</header>
<main>
  <div class="card">
    <h2>Simulate</h2>
    <div class="seg" id="kind">
      <button data-kind="0">Off</button>
      <button data-kind="6">Dexcom G6</button>
      <button data-kind="7">Dexcom G7</button>
    </div>
    <div class="muted" id="kindMsg" style="margin-top:8px"></div>
    <div class="muted" style="margin-top:6px">Changing the kind starts a new sensor with a new id, so the app pairs it again: a G6's id is set below, a G7's two characters are random.</div>
  </div>

  <div class="card">
    <h2>Current reading</h2>
    <div class="glucose">
      <span class="val" id="glucose">—</span>
      <span class="unit">mg/dL</span>
      <span id="trend" class="unit"></span>
      <span class="state" id="statelabel">—</span>
    </div>
    <div class="muted" id="window" style="margin-top:10px"></div>
  </div>

  <div class="card">
    <h2>Sensor</h2>
    <div class="row">
      <div>
        <label for="g6id">G6 transmitter id (6 characters: 0-9, A-Z without I/O)</label>
        <input type="text" id="g6id" maxlength="6" spellcheck="false" placeholder="81ABC2">
      </div>
      <div style="flex:0 0 auto; display:flex; align-items:flex-end"><button id="applyId">Set id</button></div>
    </div>
    <div style="margin-top:14px"><button class="danger" id="newSensor">New sensor, same kind</button></div>
    <div class="muted" id="sensorMsg" style="margin-top:8px"></div>
    <div class="muted" style="margin-top:6px">"New sensor" restarts the session of the kind now in use.</div>
  </div>

  <div class="card">
    <h2>Glucose</h2>
    <div class="row">
      <div>
        <label>Next reading (mg/dL)</label>
        <input type="number" id="glucoseIn" min="40" max="400" step="1" value="120">
        <input type="range" id="glucoseRange" min="40" max="400" step="1" value="120">
      </div>
      <div>
        <label>Trend (mg/dL per minute)</label>
        <input type="number" id="trendIn" min="-12.7" max="12.6" step="0.1" value="0">
      </div>
    </div>
    <div style="margin-top:14px; display:flex; align-items:center; gap:12px">
      <button class="primary" id="applyGlucose">Apply</button>
      <span class="muted" id="glucoseMsg"></span>
    </div>
    <div class="muted" style="margin-top:10px">The core holds one next reading and a trend: the next reading is set to this glucose and then moves by the trend each 5 minutes. 
    g7sim's sine and random walk are not in the core.</div>
  </div>

  <div class="card">
    <h2>Calibration</h2>
    <div class="row">
      <div><label>Calibrate to (mg/dL)</label><input type="number" id="calIn" min="40" max="400" step="1" value="120"></div>
      <div style="flex:0 0 auto; display:flex; align-items:flex-end"><button id="calApply">Calibrate</button></div>
    </div>
    <div class="muted" style="margin-top:8px">What a G6 calibration on the control characteristic does: the reading the app last calibrated to, and the next reading set to it. A G7 reports no calibration.</div>
  </div>

  <div class="card">
    <h2>Sensor state / error</h2>
    <label for="algostate">Algorithm state reported in every reading</label>
    <select id="algostate">
      <option value="0">Normal (from the session's age)</option>
      <option value="1">Stopped</option>
      <option value="2">Warm-up (no glucose)</option>
      <option value="6">OK</option>
      <option value="16">G6 session failure</option>
      <option value="18">Temporary issue (G7)</option>
      <option value="24">Expired (G7)</option>
      <option value="25">Sensor failed</option>
      <option value="26">Session ended (G7)</option>
    </select>
    <div style="margin-top:12px; display:flex; align-items:center; gap:12px">
      <span class="muted" id="stateMsg"></span>
    </div>
    <div class="muted" style="margin-top:8px">Any non-normal state is sent as-is and the error states report no glucose; 0 lets the core derive it from the sensor's age. Another state can be posted to /cgm/command.</div>
  </div>

  <div class="card">
    <h2>Signal loss and warm-up</h2>
    <div class="row">
      <div>
        <label for="signalLoss">Signal loss</label>
        <div style="display:flex; align-items:center; gap:8px">
          <input type="checkbox" id="signalLoss" style="width:auto">
          <span class="muted">stop advertising; the phone misses the readings</span>
        </div>
      </div>
      <div style="flex:0 0 auto; display:flex; align-items:flex-end"><button id="skipWarmup">Skip warm-up</button></div>
    </div>
    <div class="muted" id="faultMsg" style="margin-top:8px"></div>
    <div class="muted" style="margin-top:6px">Signal loss stops the advertising, so the phone misses the readings; the core still makes them, and the app is sent the missed ones (backfill) when the loss ends. Skipping the warm-up keeps the sensor and its pairing: it moves the start back so the warm-up is over now.</div>
  </div>

  <div class="card">
    <h2>Failure injection</h2>
    <div class="row">
      <div><label for="failRefuse">Refuse connects</label><input type="number" id="failRefuse" min="0" max="1000" step="1" value="0"></div>
      <div><label for="failQuiet">Go quiet (s)</label><input type="number" id="failQuiet" min="0" max="86400" step="1" value="0"></div>
      <div><label for="failDelay">Delay each notification (ms)</label><input type="number" id="failDelay" min="0" max="60000" step="10" value="0"></div>
      <div><label for="failIgnore">Ignore N messages</label><input type="number" id="failIgnore" min="0" max="1000" step="1" value="0"></div>
    </div>
    <div style="margin-top:14px; display:flex; flex-wrap:wrap; gap:8px 12px">
      <button class="danger" id="failDrop" type="button">Drop next connect</button>
      <button class="primary" id="failApply" type="button">Apply</button>
      <button id="failClear" type="button">Clear</button>
    </div>
    <div class="muted" id="failMsg" style="margin-top:8px">One BLE link is shared by all three simulators, so drop and refuse act on the next connection whichever device asked; quiet removes only this device from advertising for the chosen time; delay holds each of this device's notifications; ignore drops that many messages from the phone, then resumes.</div>
  </div>
</main>
<footer>For development use only — not a Dexcom product. This sensor emits simulated, made-up data; never use it for therapy.</footer>
<script>
const KIND = {0:"off", 6:"Dexcom G6 transmitter", 7:"Dexcom G7 sensor"};
const STATE = {1:["stopped","bad"],2:["warm-up","warn"],6:["ok","ok"],16:["session failure","bad"],
  18:["temporary issue","warn"],24:["expired","bad"],25:["sensor failed","bad"],26:["session ended","bad"]};
let s = {};

function ago(sec){ if(sec==null) return "—"; if(sec<0) sec=0;
  if(sec<60) return Math.round(sec)+"s"; if(sec<3600) return Math.round(sec/60)+"m";
  if(sec<172800) return Math.round(sec/3600)+"h"; return (sec/86400).toFixed(1)+"d"; }

async function cmd(body, msg, done){
  const el = document.getElementById(msg);
  el.textContent = 'Applying…';
  try {
    const r = await fetch('/cgm/command',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
    const t = await r.text();
    if(!r.ok) throw new Error(t.trim());
    el.textContent = done;
    setTimeout(()=>{ if(el.textContent===done) el.textContent=''; }, 2500);
    await refresh();
  } catch(e){ el.textContent = 'Failed — '+e.message; }
}

function sync(id, val){ const el = document.getElementById(id); if(document.activeElement !== el) el.value = val; }

document.querySelectorAll('#kind button').forEach(b=>b.onclick=()=>
  cmd({command:'cgmSetKind', kind:+b.dataset.kind}, 'kindMsg', 'Set'));
document.getElementById('applyId').onclick = ()=>cmd({command:'cgmSetId', id:document.getElementById('g6id').value.trim()}, 'sensorMsg', 'Id set');
document.getElementById('newSensor').onclick = ()=>cmd({command:'cgmNewSensor', kind:(s.kind||7)}, 'sensorMsg', 'New sensor started');
document.getElementById('applyGlucose').onclick = ()=>cmd({command:'cgmSetGlucose',
  glucose:+document.getElementById('glucoseIn').value,
  trend:Math.round(+document.getElementById('trendIn').value*10)}, 'glucoseMsg', 'Applied ✓');
document.getElementById('calApply').onclick = ()=>cmd({command:'cgmSetCalibration',
  glucose:+document.getElementById('calIn').value}, 'glucoseMsg', 'Calibrated ✓');
document.getElementById('algostate').onchange = ()=>cmd({command:'cgmSetState',
  state:+document.getElementById('algostate').value}, 'stateMsg', 'State set');
document.getElementById('signalLoss').onchange = e=>cmd({command:'cgmSetSignalLoss',
  on:e.target.checked}, 'faultMsg', e.target.checked ? 'Signal loss on' : 'Signal loss off');
document.getElementById('skipWarmup').onclick = ()=>cmd({command:'cgmSkipWarmup'}, 'faultMsg', 'Warm-up skipped');
document.getElementById('glucoseIn').oninput = e=>{ document.getElementById('glucoseRange').value = e.target.value; };
document.getElementById('glucoseRange').oninput = e=>{ document.getElementById('glucoseIn').value = e.target.value; };

async function refresh(){
  let st; try { st = await (await fetch('/cgm/status')).json(); } catch(e){ return; }
  s = st;
  document.getElementById('name').textContent = st.name || 'CGM';
  document.getElementById('model').textContent = KIND[st.kind] || 'CGM';
  document.getElementById('id').textContent = st.id ? 'id '+st.id : '';
  document.getElementById('age').textContent = st.activated ? 'sensor '+ago(st.ageSeconds) : '';
  document.getElementById('link').textContent = (st.connected ? 'link up' : st.advertising ? 'advertising' : 'idle')
    + (st.radioConnected ? ' · phone connected' : '');
  const last = st.last;
  const g = last && last.glucose ? last.glucose : (st.glucose || null);
  document.getElementById('glucose').textContent = (!st.kind || g==null) ? '—' : g;
  const tr = (st.trend||0)/10;
  document.getElementById('trend').textContent = st.kind ? (tr>0?'+':'')+tr.toFixed(1)+'/min' : '';
  const sb = STATE[st.state] || ['state '+(st.state==null?'?':st.state),''];
  const lab = document.getElementById('statelabel');
  lab.textContent = st.kind ? sb[0] : 'off';
  lab.className = 'state ' + (st.kind ? sb[1] : '');
  const nextIn = st.nextReading ? Math.round((st.nextReading - st.now)/60) : null;
  document.getElementById('window').textContent = st.kind
    ? ((st.warmupLeftSeconds>0 ? 'warm-up ends in '+ago(st.warmupLeftSeconds) : 'next reading in '+(nextIn==null?'—':nextIn+' min'))
       + ' · ' + (st.signalLoss ? 'signal loss: not advertising' : st.advertising ? 'advertising for a phone' : 'quiet')
       + ' · ' + (st.connected ? 'a phone has the link' : 'no phone'))
    : 'the CGM is off: it neither advertises nor serves a reading';
  document.querySelectorAll('#kind button').forEach(b=>b.classList.toggle('active', +b.dataset.kind === st.kind));
  sync('g6id', st.kind===6 ? (st.id||'') : '');
  document.getElementById('g6id').disabled = st.kind!==6;
  document.getElementById('applyId').disabled = st.kind!==6;
  document.getElementById('newSensor').disabled = !st.kind;
  document.getElementById('signalLoss').checked = !!st.signalLoss;
  document.getElementById('signalLoss').disabled = !st.kind;
  document.getElementById('skipWarmup').disabled = !st.kind || (st.warmupLeftSeconds||0) <= 0;
  sync('glucoseIn', st.glucose||120);
  sync('glucoseRange', st.glucose||120);
  sync('trendIn', ((st.trend||0)/10).toFixed(1));
  document.getElementById('algostate').value = st.forcedState || 0;
}

async function fail(body, done) {
  const msg = document.getElementById('failMsg');
  msg.textContent = 'Applying…';
  try {
    const r = await fetch('/fail',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
    const t = await r.text();
    if(!r.ok) throw new Error(t.trim());
    msg.textContent = done;
    setTimeout(()=>{ if(msg.textContent===done) msg.textContent=''; }, 2500);
  } catch(e){ msg.textContent = 'Failed — '+e.message; }
}
document.getElementById('failDrop').onclick = ()=>fail({device:'g7', drop:true}, 'Next connection will be dropped');
document.getElementById('failApply').onclick = ()=>fail({device:'g7',
  refuse:+document.getElementById('failRefuse').value,
  quietMs:+document.getElementById('failQuiet').value*1000,
  delayMs:+document.getElementById('failDelay').value,
  ignore:+document.getElementById('failIgnore').value}, 'Failure set');
document.getElementById('failClear').onclick = ()=>fail({device:'g7', drop:false, refuse:0, quietMs:0, delayMs:0, ignore:0}, 'Failures cleared');
refresh(); setInterval(refresh, 2000);
</script>
</body>
</html>)HTML";
