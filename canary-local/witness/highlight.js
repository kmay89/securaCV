/* Tiny, dependency-free syntax highlighter — so code/terminal previews read as
 * colorized code, not a wall of text. CSP-safe (no eval, no CDN). Returns HTML
 * with <span class="hl-*"> tokens; callers set innerHTML on a trusted element.
 *
 * Exposed: highlightJSON(value) — the Witness Wall emulator's JSON preview
 * (js/tv-emulator.js) is its one caller.
 *
 * This copy is the canonical one: securaCV's scripts/vendor_witness_emulator.sh
 * copies it byte-for-byte into the Flasher and the Lab, so a change here is
 * theirs after the next re-vendor.
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
