/**
 * @file tune_ui.h
 * @brief PROGMEM HTML for the hidden Tuning Lab at /tune.
 *
 * This page is **P2** (developer / power-user). It exposes every NVS-
 * backed coefficient that drives the v1 sensing modules as a labeled
 * slider with min, max, default, and a Save Preset / Load Preset row.
 *
 * Reveal paths:
 *   1. Long-press on the device-id chip in the dashboard topbar.
 *   2. Navigate directly to http://canary.local/tune
 *   3. Append ?tune=1 to any dashboard URL.
 *
 * Privacy:
 *   - No data leaves the device. Save Preset writes a local JSON
 *     download; Load Preset reads from a local file picker.
 *   - Every POST is rate-limited to one in-flight write at a time.
 *   - Every GET / POST goes through the same chokepoint as the
 *     dashboard, so the privacy budget pill on the headline page
 *     reflects any (deliberate) export.
 *
 * Style: deliberately *not* the polished pearlescent dashboard — this
 * page is a console for tinkerers. Monospace, dense, frosted-glass
 * card framing so it visually reads as "you're in the engine bay."
 *
 * Microcopy lint scope: this page is intentionally outside the lint's
 * banned-words list (`csi`, `threshold`, `preset`, `nvs`, etc.) because
 * its audience is OSS contributors, not grandma. The lint script
 * scans only csi_dashboard_html.h.
 */

#ifndef SECURACV_TUNE_UI_H
#define SECURACV_TUNE_UI_H

#include <pgmspace.h>

const char TUNE_UI_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>Canary · Tuning Lab</title>
<style>
:root{
  --bg:#0b0d10; --fg:#e6edf3; --muted:#9ba3ab; --accent:#7cdcff;
  --warn:#ffb454; --good:#9ddb73; --line:rgba(255,255,255,.08);
  --card:rgba(255,255,255,.04); --card-hi:rgba(255,255,255,.06);
}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.4 ui-monospace,SFMono-Regular,Menlo,monospace}
body{min-height:100vh;padding:24px;max-width:1100px;margin:0 auto}
header{display:flex;align-items:baseline;gap:12px;flex-wrap:wrap;margin-bottom:20px}
header .lead{font-size:18px;font-weight:600;letter-spacing:-.01em}
header .crumb{color:var(--muted);font-size:12px}
header a{color:var(--accent);text-decoration:none}
header a:hover{text-decoration:underline}

.warn{
  background:rgba(255,180,84,.08);border:1px solid rgba(255,180,84,.25);
  color:var(--warn);padding:10px 12px;border-radius:8px;margin-bottom:18px;font-size:12px;
}

.toolbar{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:20px}
.btn{
  background:var(--card-hi);border:1px solid var(--line);color:var(--fg);
  padding:8px 14px;border-radius:8px;font:inherit;cursor:pointer;transition:background .15s
}
.btn:hover{background:rgba(255,255,255,.08)}
.btn.primary{background:rgba(124,220,255,.15);border-color:rgba(124,220,255,.35);color:var(--accent)}
.btn.primary:hover{background:rgba(124,220,255,.22)}

.group{
  background:var(--card);border:1px solid var(--line);border-radius:12px;
  padding:14px 16px;margin-bottom:14px;backdrop-filter:blur(12px) saturate(140%);
}
.group h2{margin:0 0 10px 0;font-size:13px;font-weight:600;color:var(--accent);letter-spacing:.04em;text-transform:uppercase}

.row{display:grid;grid-template-columns:220px 1fr 90px 70px;gap:10px;align-items:center;padding:8px 0;border-bottom:1px dashed var(--line)}
.row:last-child{border-bottom:none}
.row .key{color:var(--muted);font-size:12px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.row .key .label{color:var(--fg);display:block;font-size:13px}
.row input[type=range]{width:100%;accent-color:var(--accent)}
.row .v{text-align:right;font-variant-numeric:tabular-nums;font-size:13px}
.row .reset{font-size:11px;color:var(--muted);background:none;border:1px solid var(--line);border-radius:6px;padding:2px 6px;cursor:pointer}
.row .reset:hover{color:var(--fg)}

.row.bool input[type=range]{display:none}
.row.bool .toggle{display:inline-flex;align-items:center;gap:8px;cursor:pointer;user-select:none}
.row.bool .toggle .pill{width:36px;height:20px;background:rgba(255,255,255,.12);border-radius:999px;position:relative;transition:background .15s}
.row.bool .toggle .pill::after{content:'';position:absolute;top:2px;left:2px;width:16px;height:16px;background:var(--fg);border-radius:50%;transition:left .15s}
.row.bool .toggle.on .pill{background:rgba(124,220,255,.45)}
.row.bool .toggle.on .pill::after{left:18px}

.status{margin-top:18px;color:var(--muted);font-size:12px}
.status .good{color:var(--good)}
.status .warn{color:var(--warn)}

footer{margin-top:24px;padding-top:14px;border-top:1px solid var(--line);color:var(--muted);font-size:11px;line-height:1.6}
footer code{background:var(--card-hi);padding:1px 5px;border-radius:4px}
</style>
</head>
<body>

<header>
  <span class="lead">Canary · Tuning Lab</span>
  <span class="crumb">P2 / developer surface</span>
  <span style="flex:1"></span>
  <a href="/">← back to dashboard</a>
</header>

<div class="warn">
  Power-user controls. Values you change here persist to NVS and take effect
  on the next module tick. Out-of-range values are clamped at read time.
  Use <span style="font-weight:600">Reset</span> on a row to restore the default.
</div>

<div class="toolbar">
  <button class="btn primary" id="savePreset">Save preset (.json)</button>
  <label class="btn" style="cursor:pointer">
    Load preset…
    <input type="file" id="loadPreset" accept="application/json,.json" style="display:none">
  </label>
  <button class="btn" id="resetAll">Reset all to defaults</button>
</div>

<div id="groups"></div>

<p class="status" id="status">Loading coefficients…</p>

<footer>
  <strong>Notes.</strong>
  Settings are read with <code>csi_module_settings_int / _bool</code>; the host
  override at <code>csi_integration.cpp</code> backs them with NVS via Preferences.
  Each POST triggers a re-init of the affected module so the new value lands on
  the next tick. The presets file is plain JSON: <code>{ "&lt;full.key&gt;": &lt;value&gt;, ... }</code>.
  Open <code>/tune?tune=1</code> from anywhere to land here.
</footer>

<script>
/* Auth helper — handle_tune_page injects window.__CV_TOKEN before this
 * script runs; cvFetch wraps fetch() so each /api/tune/* call adds the
 * Authorization header automatically. Falls back to plain fetch if the
 * token wasn't injected (those requests will then 401 from the device,
 * which is the correct fail-closed behavior). Mirrors the helper in
 * the headline dashboard. */
function cvFetch(url, opts) {
  opts = opts || {};
  if (window.__CV_TOKEN) {
    opts.headers = Object.assign({}, opts.headers || {},
      {'Authorization': 'Bearer ' + window.__CV_TOKEN});
  }
  return fetch(url, opts);
}

const $ = sel => document.querySelector(sel);
const groupsEl = $('#groups');
const status = $('#status');

let state = { coeffs: [], byKey: {} };

const fmtVal = (c, v) => {
  if (c.kind === 'bool') return v ? 'on' : 'off';
  if (c.kind === 'minutes') {
    const m = Math.max(0, Math.min(1439, v|0));
    const hh = String((m/60)|0).padStart(2,'0');
    const mm = String(m%60).padStart(2,'0');
    return hh + ':' + mm;
  }
  return String(v);
};

function setStatus(msg, cls){
  status.className = 'status ' + (cls||'');
  status.textContent = msg;
}

function renderGroups(){
  // Bucket by `group` field, preserving array order within each group.
  const buckets = {};
  for (const c of state.coeffs){
    (buckets[c.group] = buckets[c.group] || []).push(c);
  }
  const order = ['core.presence','core.breathing','core.quiet_hours','anomaly.baseline'];
  const seen = new Set();
  const html = [];
  const renderRow = c => {
    if (c.kind === 'bool'){
      return (
        '<div class="row bool" data-key="'+c.full_key+'">'+
          '<div class="key"><span class="label">'+c.label+'</span>'+c.full_key+'</div>'+
          '<div class="toggle'+(c.value?' on':'')+'" tabindex="0" role="switch" aria-checked="'+!!c.value+'">'+
            '<span class="pill"></span><span>'+(c.value?'enabled':'disabled')+'</span>'+
          '</div>'+
          '<div class="v">'+fmtVal(c, c.value)+'</div>'+
          '<button class="reset">reset</button>'+
        '</div>'
      );
    }
    return (
      '<div class="row" data-key="'+c.full_key+'">'+
        '<div class="key"><span class="label">'+c.label+'</span>'+c.full_key+'</div>'+
        '<input type="range" min="'+c.min+'" max="'+c.max+'" step="'+(c.step||1)+'" value="'+c.value+'">'+
        '<div class="v">'+fmtVal(c, c.value)+'</div>'+
        '<button class="reset">reset</button>'+
      '</div>'
    );
  };
  const group_label = id => ({
    'core.presence':'Presence detection',
    'core.breathing':'Breathing rhythm',
    'core.quiet_hours':'Quiet hours',
    'anomaly.baseline':'Out-of-pattern detector',
  })[id] || id;
  for (const id of order){
    if (!buckets[id]) continue;
    seen.add(id);
    html.push('<div class="group"><h2>'+group_label(id)+'</h2>'+ buckets[id].map(renderRow).join('') +'</div>');
  }
  // Any future group not in the canonical order falls in last.
  for (const id of Object.keys(buckets)){
    if (seen.has(id)) continue;
    html.push('<div class="group"><h2>'+group_label(id)+'</h2>'+ buckets[id].map(renderRow).join('') +'</div>');
  }
  groupsEl.innerHTML = html.join('');
  bindRowEvents();
}

let pendingTimer = 0;
let inflight = false;
function postCoeff(c){
  // Coalesce rapid drags: only the last value within 300ms is sent.
  if (pendingTimer) clearTimeout(pendingTimer);
  pendingTimer = setTimeout(async () => {
    if (inflight) return;
    inflight = true;
    try {
      const body = {}; body[c.full_key] = c.value;
      const r = await cvFetch('/api/tune/coefficients', {
        method:'POST', headers:{'content-type':'application/json'},
        body: JSON.stringify(body)
      });
      if (!r.ok) throw new Error('HTTP '+r.status);
      setStatus('Saved '+c.full_key+' = '+fmtVal(c, c.value), 'good');
    } catch(e){
      setStatus('Save failed: '+e.message, 'warn');
    } finally {
      inflight = false;
    }
  }, 300);
}

function bindRowEvents(){
  groupsEl.querySelectorAll('.row').forEach(row => {
    const key = row.dataset.key;
    const c = state.byKey[key];
    if (!c) return;

    if (c.kind === 'bool'){
      const toggle = row.querySelector('.toggle');
      toggle.addEventListener('click', () => {
        c.value = c.value ? 0 : 1;
        toggle.classList.toggle('on', !!c.value);
        toggle.setAttribute('aria-checked', !!c.value);
        toggle.querySelector('span:last-child').textContent = c.value?'enabled':'disabled';
        row.querySelector('.v').textContent = fmtVal(c, c.value);
        postCoeff(c);
      });
    } else {
      const input = row.querySelector('input[type=range]');
      const v = row.querySelector('.v');
      input.addEventListener('input', () => {
        c.value = Number(input.value);
        v.textContent = fmtVal(c, c.value);
      });
      input.addEventListener('change', () => postCoeff(c));
    }

    row.querySelector('.reset').addEventListener('click', () => {
      c.value = c.default;
      if (c.kind === 'bool'){
        const t = row.querySelector('.toggle');
        t.classList.toggle('on', !!c.value);
        t.setAttribute('aria-checked', !!c.value);
        t.querySelector('span:last-child').textContent = c.value?'enabled':'disabled';
      } else {
        row.querySelector('input[type=range]').value = c.value;
      }
      row.querySelector('.v').textContent = fmtVal(c, c.value);
      postCoeff(c);
    });
  });
}

async function load(){
  try {
    const r = await cvFetch('/api/tune/coefficients');
    if (!r.ok) throw new Error('HTTP '+r.status);
    const j = await r.json();
    state.coeffs = j.coefficients || [];
    state.byKey = {};
    for (const c of state.coeffs) state.byKey[c.full_key] = c;
    renderGroups();
    setStatus(state.coeffs.length+' coefficients loaded.');
  } catch(e){
    setStatus('Failed to load: '+e.message, 'warn');
  }
}

$('#savePreset').addEventListener('click', async () => {
  try {
    const r = await cvFetch('/api/tune/preset');
    if (!r.ok) throw new Error('HTTP '+r.status);
    const blob = await r.blob();
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'canary-tuning-' + Date.now() + '.json';
    document.body.appendChild(a); a.click(); a.remove();
    URL.revokeObjectURL(url);
    setStatus('Preset saved to disk.', 'good');
  } catch(e){
    setStatus('Save preset failed: '+e.message, 'warn');
  }
});

$('#loadPreset').addEventListener('change', async ev => {
  const f = ev.target.files && ev.target.files[0];
  if (!f) return;
  try {
    const text = await f.text();
    JSON.parse(text); // validate locally before sending
    const r = await cvFetch('/api/tune/preset', {
      method:'POST', headers:{'content-type':'application/json'}, body:text
    });
    if (!r.ok) throw new Error('HTTP '+r.status);
    setStatus('Preset loaded. Reloading values…', 'good');
    await load();
  } catch(e){
    setStatus('Load preset failed: '+e.message, 'warn');
  } finally {
    ev.target.value = '';
  }
});

$('#resetAll').addEventListener('click', async () => {
  if (!confirm('Reset every coefficient to its default? This is one POST per row.')) return;
  for (const c of state.coeffs){
    c.value = c.default;
    try {
      const body = {}; body[c.full_key] = c.value;
      await cvFetch('/api/tune/coefficients', {
        method:'POST', headers:{'content-type':'application/json'}, body: JSON.stringify(body)
      });
    } catch(_){}
  }
  await load();
  setStatus('All coefficients reset to defaults.', 'good');
});

load();
</script>
</body>
</html>
)HTML";

#endif /* SECURACV_TUNE_UI_H */
