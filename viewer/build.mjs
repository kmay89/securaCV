// Build the single-file offline viewer by inlining verify_core.js,
// timeline_core.js and zip_core.js into template.html.
//
//   node viewer/build.mjs            # regenerate viewer/evidence_viewer.html
//   node viewer/build.mjs --check    # fail if the committed file is stale (CI drift guard)
//
// Keeping one source of verification logic (verify_core.js) and inlining it here prevents
// the two-verifier drift the project is explicitly trying to avoid.

import { readFileSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const CORE_PLACEHOLDER = '/*__VERIFY_CORE__*/';
const TIMELINE_PLACEHOLDER = '/*__TIMELINE_CORE__*/';
const ZIP_PLACEHOLDER = '/*__ZIP_CORE__*/';
// Includes the ` null` fallback so the un-built template is still valid JS, and the built file
// becomes `const SAMPLE_ENVELOPE = {…};` rather than `= {…} null;`.
const SAMPLE_PLACEHOLDER = '/*__SAMPLE_ENVELOPE__*/ null';
const SAMPLE_BUNDLE_PLACEHOLDER = '/*__SAMPLE_BUNDLE__*/ null';
const CSP_PLACEHOLDER = '/*__CSP__*/';

const template = readFileSync(join(here, 'template.html'), 'utf8');
const core = readFileSync(join(here, 'verify_core.js'), 'utf8');
const timeline = readFileSync(join(here, 'timeline_core.js'), 'utf8');
// The evidence-pack (.zip) reader: lets the viewer take the one file Home
// Assistant's export hands a recipient, and read the bundle inside it in place.
const zip = readFileSync(join(here, 'zip_core.js'), 'utf8');
// Real, valid fixtures double as the in-page "Try the sample …" demos, so the demos can
// never drift from what the verifiers actually accept.
const sample = readFileSync(join(here, '..', 'tests', 'fixtures', 'envelope', 'valid_envelope.json'), 'utf8').trim();
const sampleBundle = readFileSync(join(here, '..', 'tests', 'fixtures', 'export_bundle', 'valid_bundle.json'), 'utf8').trim();

for (const p of [CORE_PLACEHOLDER, TIMELINE_PLACEHOLDER, ZIP_PLACEHOLDER, SAMPLE_PLACEHOLDER, SAMPLE_BUNDLE_PLACEHOLDER, CSP_PLACEHOLDER]) {
  if (!template.includes(p)) {
    console.error('template.html is missing the ' + p + ' placeholder');
    process.exit(2);
  }
}

// The cores are inlined as separate classic <script> blocks, which share ONE
// global lexical scope: a top-level `const` declared in two of them is a
// SyntaxError that kills every script on the page ("Identifier 'x' has already
// been declared"), and the page then fails only at the moment the second core
// is used. Catch it here instead, at build time, by name.
const topLevelBindings = (src) => new Set(
  Array.from(src.matchAll(/^(?:const|let|class|function)\s+([A-Za-z_$][\w$]*)/gm), (m) => m[1]));
const cores = [['verify_core.js', core], ['timeline_core.js', timeline], ['zip_core.js', zip]];
for (let i = 0; i < cores.length; i++) {
  for (let j = i + 1; j < cores.length; j++) {
    const [aName, a] = cores[i];
    const [bName, b] = cores[j];
    const collisions = [...topLevelBindings(a)].filter((n) => topLevelBindings(b).has(n));
    if (collisions.length) {
      console.error(aName + ' and ' + bName + ' both declare top-level: ' + collisions.join(', ')
        + '\nThey are inlined into one shared script scope — rename one side.');
      process.exit(2);
    }
  }
}

const inlined = template
  .replace(CORE_PLACEHOLDER, () => core)
  .replace(TIMELINE_PLACEHOLDER, () => timeline)
  .replace(ZIP_PLACEHOLDER, () => zip)
  .replace(SAMPLE_PLACEHOLDER, () => sample)
  .replace(SAMPLE_BUNDLE_PLACEHOLDER, () => sampleBundle);

// The page's Content-Security-Policy, computed from the page itself: every
// inline <script> and the one <style> block are allowed by their sha256 and
// nothing else is — no network, no eval, no base or form target. The viewer
// renders strings from the bundle it is handed (names, zones, labels), so if
// an escape is ever missed, the browser still refuses to run what got through.
// `style-src` carries no 'unsafe-inline', so an inline style attribute in
// markup would be refused at runtime; refuse it here instead, where it is seen.
const cspFor = (html) => {
  const bodies = (tag) => Array.from(html.matchAll(new RegExp(`<${tag}>([\\s\\S]*?)</${tag}>`, 'g')), (m) => m[1]);
  const hash = (text) => `'sha256-${createHash('sha256').update(text, 'utf8').digest('base64')}'`;
  const scripts = bodies('script');
  const styles = bodies('style');
  if (scripts.length === 0 || styles.length !== 1) {
    throw new Error(`expected inline scripts and exactly one <style> block, found ${scripts.length} and ${styles.length}`);
  }
  return [
    "default-src 'none'",
    `script-src ${scripts.map(hash).join(' ')}`,
    `style-src ${styles.map(hash).join(' ')}`,
    'img-src data:',
    "connect-src 'none'",
    "base-uri 'none'",
    "form-action 'none'",
  ].join('; ');
};
const inlineStyle = inlined.match(/ style="[^"]*"/);
if (inlineStyle) {
  console.error('template.html carries an inline style attribute the page CSP would refuse: ' + inlineStyle[0]
    + '\nUse a class, or data-style painted through paint().');
  process.exit(2);
}
const output = inlined.replace(CSP_PLACEHOLDER, () => cspFor(inlined));
const outPath = join(here, 'evidence_viewer.html');

if (process.argv.includes('--check')) {
  let current = '';
  try { current = readFileSync(outPath, 'utf8'); } catch { /* missing */ }
  if (current !== output) {
    console.error('evidence_viewer.html is out of date — run `node viewer/build.mjs` and commit.');
    process.exit(1);
  }
  console.log('evidence_viewer.html is up to date.');
} else {
  writeFileSync(outPath, output);
  console.log('wrote ' + outPath);
}
