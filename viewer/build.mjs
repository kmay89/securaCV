// Build the single-file offline viewer by inlining verify_core.js into template.html.
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
const PLACEHOLDER = '/*__VERIFY_CORE__*/';

const template = readFileSync(join(here, 'template.html'), 'utf8');
const core = readFileSync(join(here, 'verify_core.js'), 'utf8');

if (!template.includes(PLACEHOLDER)) {
  console.error('template.html is missing the ' + PLACEHOLDER + ' placeholder');
  process.exit(2);
}

const output = template.replace(PLACEHOLDER, () => core);
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
