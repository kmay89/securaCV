// Build the single-file offline viewer by inlining verify_core.js and
// timeline_core.js into template.html.
//
//   node viewer/build.mjs            # regenerate viewer/evidence_viewer.html
//   node viewer/build.mjs --check    # fail if the committed file is stale (CI drift guard)
//
// Keeping one source of verification logic (verify_core.js) and inlining it here prevents
// the two-verifier drift the project is explicitly trying to avoid.

import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const CORE_PLACEHOLDER = '/*__VERIFY_CORE__*/';
const TIMELINE_PLACEHOLDER = '/*__TIMELINE_CORE__*/';
// Includes the ` null` fallback so the un-built template is still valid JS, and the built file
// becomes `const SAMPLE_ENVELOPE = {…};` rather than `= {…} null;`.
const SAMPLE_PLACEHOLDER = '/*__SAMPLE_ENVELOPE__*/ null';
const SAMPLE_BUNDLE_PLACEHOLDER = '/*__SAMPLE_BUNDLE__*/ null';

const template = readFileSync(join(here, 'template.html'), 'utf8');
const core = readFileSync(join(here, 'verify_core.js'), 'utf8');
const timeline = readFileSync(join(here, 'timeline_core.js'), 'utf8');
// Real, valid fixtures double as the in-page "Try the sample …" demos, so the demos can
// never drift from what the verifiers actually accept.
const sample = readFileSync(join(here, '..', 'tests', 'fixtures', 'envelope', 'valid_envelope.json'), 'utf8').trim();
const sampleBundle = readFileSync(join(here, '..', 'tests', 'fixtures', 'export_bundle', 'valid_bundle.json'), 'utf8').trim();

for (const p of [CORE_PLACEHOLDER, TIMELINE_PLACEHOLDER, SAMPLE_PLACEHOLDER, SAMPLE_BUNDLE_PLACEHOLDER]) {
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
const collisions = [...topLevelBindings(core)].filter((n) => topLevelBindings(timeline).has(n));
if (collisions.length) {
  console.error('verify_core.js and timeline_core.js both declare top-level: ' + collisions.join(', ')
    + '\nThey are inlined into one shared script scope — rename one side.');
  process.exit(2);
}

const output = template
  .replace(CORE_PLACEHOLDER, () => core)
  .replace(TIMELINE_PLACEHOLDER, () => timeline)
  .replace(SAMPLE_PLACEHOLDER, () => sample)
  .replace(SAMPLE_BUNDLE_PLACEHOLDER, () => sampleBundle);
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
