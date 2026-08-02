/* Tiny, dependency-free syntax highlighter — so code/terminal previews read as
 * colorized code, not a wall of text. CSP-safe (no eval, no CDN). Returns HTML
 * with <span class="hl-*"> tokens; callers set innerHTML on a trusted element.
 *
 * Exposed: highlightJSON(value), highlightLog(lines), and highlightPre(root)
 * which upgrades any <pre data-hl="json|log"> under `root`.
 */
const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

export function highlightJSON(value) {
  const text = typeof value === 'string' ? value : JSON.stringify(value, null, 2);
  // Tokenize on strings / numbers / booleans / null / punctuation.
  const re = /("(?:\\.|[^"\\])*")(\s*:)?|\b(true|false|null)\b|(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)|([{}\[\],])/g;
  let out = '', last = 0, m;
  while ((m = re.exec(text)) !== null) {
    out += esc(text.slice(last, m.index));
    last = re.lastIndex;
    if (m[1] !== undefined) {
      // string — a key if followed by a colon
      out += m[2] !== undefined
        ? `<span class="hl-key">${esc(m[1])}</span><span class="hl-punct">${esc(m[2])}</span>`
        : `<span class="hl-str">${esc(m[1])}</span>`;
    } else if (m[3] !== undefined) out += `<span class="hl-bool">${esc(m[3])}</span>`;
    else if (m[4] !== undefined) out += `<span class="hl-num">${esc(m[4])}</span>`;
    else if (m[5] !== undefined) out += `<span class="hl-punct">${esc(m[5])}</span>`;
  }
  out += esc(text.slice(last));
  return out;
}

const LOG_KW = /\b(Motion|Presence|Door|Glass|Line|Quiet|Incident|Knock|Delivery|Package|sealed|verified|online|offline|chain)\b/g;

export function highlightLog(lines) {
  const arr = Array.isArray(lines) ? lines : String(lines).split('\n');
  return arr.map((raw) => {
    let s = esc(raw);
    s = s.replace(/(^|\s)(\d{1,2}:\d{2}(?::\d{2})?(?:\s?[AP]M)?)/g, (all, pre, t) => `${pre}<span class="hl-time">${t}</span>`);
    s = s.replace(/\b([0-9a-f]{4,}(?:…[0-9a-f]{4,})?)\b/gi, '<span class="hl-hash">$1</span>');
    s = s.replace(/(✓)/g, '<span class="hl-ok">$1</span>');
    s = s.replace(LOG_KW, '<span class="hl-kw">$1</span>');
    return `<span class="hl-line">${s}</span>`;
  }).join('\n');
}

export function highlightPre(root) {
  (root || document).querySelectorAll('pre[data-hl]').forEach((pre) => {
    if (pre.dataset.hlDone) return;
    const lang = pre.getAttribute('data-hl');
    const raw = pre.textContent;
    pre.innerHTML = lang === 'json' ? highlightJSON(raw) : highlightLog(raw);
    pre.dataset.hlDone = '1';
  });
}
