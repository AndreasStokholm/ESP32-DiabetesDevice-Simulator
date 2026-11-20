#pragma once
// The home page: one card per simulator, and the one nav every page shares.
// Hand-written in the style of src/index_html.h; the cards poll the three
// status endpoints every 2 seconds and show "--" if a board part is down.
static const char homeHTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP-Sim</title>
<style>
  :root { color-scheme: dark; --bg:#14161a; --card:#1d2026; --line:#2b2f37; --fg:#e8eaed; --mut:#9aa0aa; --accent:#4f9cf8; --ok:#43c06d; --warn:#e0a53b; --bad:#e2574c; }
  * { box-sizing:border-box; }
  body { margin:0; font:15px/1.45 -apple-system,Segoe UI,Roboto,sans-serif; background:var(--bg); color:var(--fg); }
  nav { display:flex; flex-wrap:wrap; gap:2px 18px; align-items:baseline; padding:12px 16px; border-bottom:1px solid var(--line); max-width:760px; margin:0 auto; }
  nav .brand { font-weight:600; margin-right:auto; }
  nav a { color:var(--mut); text-decoration:none; font-size:14px; }
  nav a:hover { color:var(--fg); }
  nav a.active { color:var(--accent); }
  main { max-width:760px; margin:0 auto; padding:16px; display:grid; gap:14px; }
  .banner { display:none; gap:10px; align-items:center; background:rgba(224,165,59,.12); border:1px solid rgba(224,165,59,.35); color:var(--warn); border-radius:10px; padding:10px 12px; font-size:13px; }
  .banner.show { display:flex; }
  .banner a { color:var(--accent); margin-left:auto; text-decoration:none; }
  .card { background:var(--card); border:1px solid var(--line); border-radius:10px; padding:16px; display:block; text-decoration:none; color:inherit; }
  .card:hover { border-color:var(--accent); }
  .t { display:flex; align-items:baseline; gap:10px; font-size:15px; font-weight:600; }
  .t .go { margin-left:auto; color:var(--accent); font-weight:500; font-size:13px; }
  .s { color:var(--mut); font-size:13px; margin-top:6px; min-height:18px; }
  .dot { display:inline-block; width:8px; height:8px; border-radius:50%; margin-right:6px; background:#3a3f48; }
  .dot.ok { background:var(--ok); }
  .dot.warn { background:var(--warn); }
  .dot.bad { background:var(--bad); }
  footer { max-width:760px; margin:0 auto; padding:4px 16px 24px; color:var(--mut); font-size:12px; }
</style>
</head>
<body>
<nav>
  <span class="brand">ESP-Sim</span>
  <a class="active" href="/">Home</a>
  <a href="/g7">G7</a>
  <a href="/pod">Pod</a>
  <a href="/medtrum">Medtrum</a>
  <a href="/settings">Settings</a>
</nav>
<main>
  <div class="banner" id="wallclock">No wall clock: NTP never ran.<a href="/setup">Set up Wi-Fi &rarr;</a></div>
  <a class="card" href="/g7">
    <div class="t">Dexcom G7<span class="go">Open &rarr;</span></div>
    <div class="s"><span class="dot" id="g7dot"></span><span id="g7line">--</span></div>
  </a>
  <a class="card" href="/pod">
    <div class="t"><span id="podTitle">Pod</span><span class="go">Open &rarr;</span></div>
    <div class="s"><span class="dot" id="poddot"></span><span id="podline">--</span></div>
  </a>
  <a class="card" href="/medtrum">
    <div class="t">Medtrum patch<span class="go">Open &rarr;</span></div>
    <div class="s"><span class="dot" id="medtrumdot"></span><span id="medtrumline">--</span></div>
  </a>
</main>
<footer>One ESP32-S3 &middot; test equipment only &middot; http://esp-sim.local/</footer>
<script>
  const $ = id => document.getElementById(id);
  const set = (id, t) => { $(id).textContent = t; };
  const dot = (id, cls) => { $(id).className = 'dot' + (cls ? ' ' + cls : ''); };
  const units = n => Math.round(n * 100) / 100;
  const popcount = n => { let c = 0; while (n) { c += n & 1; n >>= 1; } return c; };

  const PATCH_STATES = {
    0:'none',1:'idle',2:'filled',3:'priming',4:'primed',5:'ejecting',6:'ejected',
    32:'active',33:'active (alt)',64:'suspended (low BG)',65:'suspended (low BG)',
    66:'suspended (auto)',67:'suspended (hourly max)',68:'suspended (daily max)',
    69:'suspended',70:'paused',96:'occlusion',97:'expired',98:'reservoir empty',
    99:'patch fault',100:'patch fault 2',101:'base fault',102:'battery empty',
    103:'no calibration',128:'stopped'
  };
  const PATCH_FAULTS = [96,97,98,99,100,101,102,103];
  const stateName = s => PATCH_STATES[s] || ('state ' + s);

  async function get(url) {
    const r = await fetch(url);
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.json();
  }

  async function pollG7() {
    try {
      const s = await get('/status');
      const glucose = s.latest && s.latest.glucose !== undefined ? s.latest.glucose + ' mg/dL' : '--';
      const displays = Object.keys(s.slots || {}).length;
      set('g7line', (s.name || '--') + ' · code ' + (s.code || '--') + ' · ' + glucose + ' · ' + displays + (displays === 1 ? ' display' : ' displays'));
      dot('g7dot', (s.latest && s.latest.state === 6 && s.latest.glucose !== undefined) ? 'ok' : 'warn');
    } catch (err) {
      set('g7line', '--');
      dot('g7dot', '');
    }
  }

  async function pollPod() {
    try {
      const s = await get('/pod/status');
      set('podTitle', s.podType || 'Pod');
      const alerts = popcount(s.alerts || 0);
      set('podline', (s.paired ? 'Paired' : 'Not paired') + ' · ' + units(s.reservoir || 0) + ' U · active ' +
          (s.minutesActive || 0) + ' m · ' + (alerts ? alerts + (alerts === 1 ? ' alert' : ' alerts') : 'no alerts'));
      dot('poddot', s.connected ? 'ok' : 'warn');
    } catch (err) {
      set('podline', '--');
      dot('poddot', '');
    }
  }

  async function pollMedtrum() {
    try {
      const s = await get('/medtrum/status');
      const fault = PATCH_FAULTS.indexOf(s.state) >= 0;
      set('medtrumline', (s.serial || '--') + ' · ' + (s.model || '--') + ' ' +
          (s.model === 'MD8301' ? '300' : '200') + ' U · ' + stateName(s.state) + ' · ' +
          units(s.reservoir || 0) + ' U · ' + (s.connected ? 'connected' : 'not connected'));
      dot('medtrumdot', fault ? 'bad' : s.connected ? 'ok' : 'warn');
    } catch (err) {
      set('medtrumline', '--');
      dot('medtrumdot', '');
    }
  }

  async function pollWallClock() {
    try {
      const s = await get('/settings/status');
      $('wallclock').className = s.clock === 'set from NTP' ? 'banner' : 'banner show';
    } catch (err) {
      /* leave the banner alone until the board answers */
    }
  }

  async function poll() {
    await pollG7();
    await pollPod();
    await pollMedtrum();
  }

  poll();
  setInterval(poll, 2000);
  pollWallClock();
  setInterval(pollWallClock, 5000);
</script>
</body>
</html>)HTML";
