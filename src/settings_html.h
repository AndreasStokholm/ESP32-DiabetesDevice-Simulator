#pragma once
// The board's own page: Network and Board only. Hand-written in the style of
// src/index_html.h; the device pages own their devices, this owns the board.
static const char settingsHTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Settings (ESP-Sim)</title>
<style>
  :root { color-scheme: dark; --bg:#14161a; --card:#1d2026; --line:#2b2f37; --fg:#e8eaed; --mut:#9aa0aa; --accent:#4f9cf8; --ok:#43c06d; --warn:#e0a53b; --bad:#e2574c; }
  * { box-sizing:border-box; }
  body { margin:0; font:15px/1.4 -apple-system,Segoe UI,Roboto,sans-serif; background:var(--bg); color:var(--fg); }
  nav { display:flex; flex-wrap:wrap; gap:2px 18px; align-items:baseline; padding:12px 16px; border-bottom:1px solid var(--line); max-width:760px; margin:0 auto; }
  nav .brand { font-weight:600; margin-right:auto; }
  nav a { color:var(--mut); text-decoration:none; font-size:14px; }
  nav a:hover { color:var(--fg); }
  nav a.active { color:var(--accent); }
  main { max-width:760px; margin:0 auto; padding:16px; display:grid; gap:14px; }
  .card { background:var(--card); border:1px solid var(--line); border-radius:10px; padding:16px; }
  .card h2 { font-size:12px; text-transform:uppercase; letter-spacing:.06em; color:var(--mut); margin:0 0 12px; }
  .kv { display:flex; justify-content:space-between; gap:12px; padding:7px 0; border-bottom:1px solid var(--line); font-size:14px; }
  .kv:last-of-type { border-bottom:0; }
  .kv span:first-child { color:var(--mut); }
  label { display:block; font-size:13px; color:var(--mut); margin:10px 0 4px; }
  input[type=text], input[type=password] { width:100%; padding:9px; background:#14161a; border:1px solid var(--line); border-radius:8px; color:var(--fg); font-size:15px; }
  .row { display:flex; gap:12px; flex-wrap:wrap; }
  .row > div { flex:1; min-width:140px; }
  .actions { display:flex; gap:10px; flex-wrap:wrap; margin-top:12px; }
  button { padding:9px 14px; background:#2b2f37; color:var(--fg); border:1px solid var(--line); border-radius:8px; font-size:14px; cursor:pointer; }
  button:hover { border-color:var(--accent); }
  button.primary { background:var(--accent); border-color:var(--accent); color:#0b0d10; font-weight:600; }
  button.danger:hover { border-color:var(--bad); }
  .muted { color:var(--mut); font-size:13px; margin-top:10px; }
  code { font-family:ui-monospace,Menlo,monospace; }
  footer { max-width:760px; margin:0 auto; padding:4px 16px 24px; color:var(--mut); font-size:12px; }
</style>
</head>
<body>
<nav>
  <span class="brand">ESP-Sim</span>
  <a href="/">Home</a>
  <a href="/g7">G7</a>
  <a href="/pod">Pod</a>
  <a href="/medtrum">Medtrum</a>
  <a class="active" href="/settings">Settings</a>
</nav>
<main>
  <div class="card">
    <h2>Network</h2>
    <div class="kv"><span>Joined</span><span id="joined">—</span></div>
    <div class="kv"><span>Address</span><span><code>esp-sim.local</code> &middot; <span id="ip">—</span></span></div>
    <div class="kv"><span>Clock</span><span id="clock">—</span></div>
    <div class="row" style="margin-top:12px">
      <div><label for="ssid">Network name</label><input id="ssid" type="text" autocomplete="off" autocapitalize="none" spellcheck="false" placeholder="2.4 GHz network name"></div>
      <div><label for="pass">Password</label><input id="pass" type="password" autocomplete="off" placeholder="network password"></div>
    </div>
    <div class="actions">
      <button class="primary" id="join" type="button">Join and restart</button>
      <button class="danger" id="forget" type="button">Forget network</button>
    </div>
    <div class="muted">Without a saved network the board runs its own access point and shows the Wi-Fi sheet first, as a captive-portal page you can dismiss.</div>
  </div>
  <div class="card">
    <h2>Simulated pumps</h2>
    <div class="kv"><span>Now</span><span id="pumpNow">—</span></div>
    <div class="actions">
      <button type="button" id="pumpBoth">Both, taking turns</button>
      <button type="button" id="pumpPod">Pod only</button>
      <button type="button" id="pumpMed">Medtrum only</button>
    </div>
    <div class="muted" id="pumpNote">A phone runs one pump app. With both selected the pod's and the Medtrum base's advertisements share the one set, so a Medtrum app that keys a discovered device by address may never list the base — choose Medtrum only for that test, and the base advertises continuously.</div>
  </div>
  <div class="card">
    <h2>Board</h2>
    <div class="kv"><span>Firmware</span><span><code id="commit">—</code></span></div>
    <div class="kv"><span>Hardware</span><span>ESP32-S3 &middot; 8 MB</span></div>
    <div class="kv"><span>Address</span><span><code>esp-sim.local</code> &middot; <span id="boardIp">—</span></span></div>
    <div class="kv"><span>Serial console</span><span>115200 &middot; type <code>help</code></span></div>
    <div class="actions"><button id="reboot" type="button">Reboot</button></div>
  </div>
  <div class="muted" id="msg" role="status" aria-live="polite"></div>
</main>
<footer>One ESP32-S3 &middot; test equipment only.</footer>
<script>
  const $ = id => document.getElementById(id);
  let edited = false;
  $('ssid').oninput = () => { edited = true; };
  function say(t) { $('msg').textContent = t; if (t) setTimeout(() => { if ($('msg').textContent === t) $('msg').textContent = ''; }, 4000); }
  async function call(url, body) {
    say('Working...');
    try {
      const opt = { method:'POST' };
      if (body) { opt.headers = { 'Content-Type':'application/json' }; opt.body = JSON.stringify(body); }
      const r = await fetch(url, opt);
      const t = (await r.text()).trim();
      if (!r.ok) throw new Error(t);
      say(t);
    } catch (err) { say('Failed — ' + err.message); }
  }
  $('join').onclick = () => {
    const ssid = $('ssid').value.trim(), password = $('pass').value;
    if (!ssid) { say('Enter the network name.'); return; }
    call('/settings/wifi', { ssid, password });
  };
  $('forget').onclick = () => call('/settings/forget');
  $('reboot').onclick = () => call('/settings/reboot');
  $('pumpBoth').onclick = () => call('/settings/pump', { pump:'both' }).then(refresh);
  $('pumpPod').onclick = () => call('/settings/pump', { pump:'pod' }).then(refresh);
  $('pumpMed').onclick = () => call('/settings/pump', { pump:'medtrum' }).then(refresh);

  async function refresh() {
    try {
      const s = await (await fetch('/settings/status')).json();
      const joined = !!(s.wifi && s.wifi.joined), ssid = (s.wifi && s.wifi.ssid) || '';
      $('joined').textContent = ssid ? (joined ? ssid : ssid + ' (not joined)') : '—';
      $('ip').textContent = s.ip || '—';
      $('boardIp').textContent = s.ip || '—';
      $('clock').textContent = s.clock || '—';
      $('commit').textContent = s.firmware ? s.firmware.commit : '—';
      if (s.firmware && s.firmware.built) $('commit').title = 'built ' + s.firmware.built;
      if (!edited && ssid && document.activeElement !== $('ssid')) $('ssid').value = ssid;
      $('pumpNow').textContent = s.pump === 'pod' ? 'pod only' : s.pump === 'medtrum' ? 'Medtrum only' : 'both, taking turns';
    } catch (err) {
      say('Could not reach the board.');
    }
  }
  refresh();
  setInterval(refresh, 5000);
</script>
</body>
</html>)HTML";
