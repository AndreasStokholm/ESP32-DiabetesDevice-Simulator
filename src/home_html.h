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
  <a href="/cgm">CGM</a>
  <a href="/pump">Pump</a>
  <a href="/settings">Settings</a>
</nav>
<main>
  <div class="banner" id="wallclock">No wall clock: NTP never ran.<a href="/setup">Set up Wi-Fi &rarr;</a></div>
  <a class="card" href="/cgm">
    <div class="t"><span id="cgmTitle">CGM</span><span class="go">Open &rarr;</span></div>
    <div class="s"><span class="dot" id="cgmdot"></span><span id="cgmline">--</span></div>
  </a>
  <a class="card" href="/pump">
    <div class="t"><span id="pumpTitle">Pump</span><span class="go">Open &rarr;</span></div>
    <div class="s"><span class="dot" id="pumpdot"></span><span id="pumpline">--</span></div>
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

  const CGM_KINDS = {0:'CGM (off)', 6:'Dexcom G6', 7:'Dexcom G7'};
  const CGM_STATES = {1:'stopped',2:'warm-up',6:'ok',16:'session failure',18:'temporary issue',
    24:'expired',25:'failed',26:'session ended'};

  async function get(url) {
    const r = await fetch(url);
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.json();
  }

  async function pollCgm() {
    try {
      // The CGM's own view (/cgm/status): the kind, name, reading and state. The
      // g7sim-shaped /status has no code or display slots to show any more.
      const s = await get('/cgm/status');
      set('cgmTitle', CGM_KINDS[s.kind] || 'CGM');
      const glucose = s.last && s.last.glucose ? s.last.glucose + ' mg/dL' : '--';
      const st = CGM_STATES[s.state] || ('state ' + s.state);
      set('cgmline', (s.name || '--') + ' · ' + glucose + ' · ' + st + (s.signalLoss ? ' · signal loss' : ''));
      dot('cgmdot', (s.kind && s.state === 6 && s.last && s.last.glucose) ? 'ok' : 'warn');
    } catch (err) {
      set('cgmline', '--');
      dot('cgmdot', '');
    }
  }

  // The one pump selected on the pump page: its name and live status.
  async function pollPump() {
    try {
      const sel = await get('/settings/status');
      const pump = sel.pump || 'pod-o5';
      if (pump === 'off') {
        set('pumpTitle', 'Pump (off)');
        set('pumpline', 'no pump is simulated');
        dot('pumpdot', '');
        return;
      }
      if (pump === 'medtrum') {
        const s = await get('/medtrum/status');
        const fault = PATCH_FAULTS.indexOf(s.state) >= 0;
        set('pumpTitle', 'Medtrum patch');
        set('pumpline', (s.serial || '--') + ' · ' + (s.model || '--') + ' ' +
            (s.model === 'MD8301' ? '300' : '200') + ' U · ' + stateName(s.state) + ' · ' +
            units(s.reservoir || 0) + ' U · ' + (s.connected ? 'connected' : 'not connected'));
        dot('pumpdot', fault ? 'bad' : s.connected ? 'ok' : 'warn');
        return;
      }
      const s = await get('/pod/status');
      const alerts = popcount(s.alerts || 0);
      set('pumpTitle', s.podType || 'Pod');
      set('pumpline', (s.paired ? 'Paired' : 'Not paired') + ' · ' + units(s.reservoir || 0) + ' U · active ' +
          (s.minutesActive || 0) + ' m · ' + (alerts ? alerts + (alerts === 1 ? ' alert' : ' alerts') : 'no alerts'));
      dot('pumpdot', s.connected ? 'ok' : 'warn');
    } catch (err) {
      set('pumpline', '--');
      dot('pumpdot', '');
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
    await pollCgm();
    await pollPump();
  }

  poll();
  setInterval(poll, 2000);
  pollWallClock();
  setInterval(pollWallClock, 5000);
</script>
</body>
</html>)HTML";
