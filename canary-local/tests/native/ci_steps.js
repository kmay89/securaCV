// canary-local/tests/native/ci_steps.js — one canary-local.yml job's keys and
// steps, read the way the runner reads them, for the tests that hold CI's
// half of the Lab's core suites (sweeps A50, A58; not a test itself).
//
//   const ci = require("./native/ci_steps.js");
//   const { keys, steps } = ci.job("logic-tests");
//   steps[i].name, steps[i].keys.if, steps[i].env.LAB_CORES, steps[i].run
//
// Why not a substring of the step's text: a check that the text includes
// "LAB_CORES: native" passes when that line is a comment, and one that it
// includes the run line passes on "<run line> || true". Here comment lines are
// dropped first, each step is its keys (exactly as written, one each: a
// second `if:` throws), `env:` and `with:` are maps, and `run:` is the
// command text, so a test can ask for the whole key set (no
// continue-on-error), the exact env and the exact command.
//
// It reads the subset of YAML this workflow's jobs use: block mappings at
// two-space steps, `- ` step items, plain, quoted and `|` scalars, and
// trailing comments after a space. Anything else in a step throws, naming
// the line, rather than being read as something it is not.

"use strict";

const fs = require("node:fs");
const { join, relative } = require("node:path");

const REPO = join(__dirname, "..", "..", "..");
const WORKFLOW = join(REPO, ".github/workflows/canary-local.yml");

// The `if:` every step that reads the sources carries: it runs whenever the
// dist step ran, red or green, and not when the run is stopped.
const RAN = "${{ (success() || failure()) && steps.dist-node-tests.outcome != 'skipped' }}";

// The path-filter gate's recorder env (sweep CI2), all three lines.
const RECORDER = {
  PATH_FILTER_READS_DIR: "${{ runner.temp }}/path-filter-reads",
  NODE_OPTIONS: "--require ${{ github.workspace }}/scripts/path_filter_reads/record.cjs",
  PYTHONPATH: "${{ github.workspace }}/scripts/path_filter_reads",
};

const indentOf = (line) => /^ */.exec(line)[0].length;

function scalar(raw, where) {
  const v = raw.trim();
  const q = /^"((?:[^"\\]|\\.)*)"(?:\s+#.*)?$/.exec(v) || /^'((?:[^']|'')*)'(?:\s+#.*)?$/.exec(v);
  if (q) return q[1];
  if (/^["']/.test(v)) throw new Error(`${where}: a quoted value this reader cannot read: ${v}`);
  return v.replace(/\s+#.*$/, "");
}

// A block mapping's lines (comments and blank lines already dropped), at
// `indent`: { key: value }, where a value is a string, a map (the next
// level), or a `|` block's text.
function mapping(lines, indent, where) {
  const out = {};
  for (let i = 0; i < lines.length;) {
    const line = lines[i];
    const m = new RegExp(`^ {${indent}}([\\w-]+):(?: (.*))?$`).exec(line);
    if (!m) throw new Error(`${where}: cannot read this line at indent ${indent}: ${JSON.stringify(line)}`);
    const [, key, rest = ""] = m;
    if (Object.prototype.hasOwnProperty.call(out, key)) throw new Error(`${where}: ${key} appears twice`);
    let j = i + 1;
    while (j < lines.length && indentOf(lines[j]) > indent) j++;
    const block = lines.slice(i + 1, j);
    const head = rest.replace(/\s+#.*$/, "").trim();
    if (head === "|") {
      const pad = block.length ? Math.min(...block.map(indentOf)) : 0;
      out[key] = block.map((l) => l.slice(pad)).join("\n");
    } else if (head === "") {
      out[key] = block.length ? mapping(block, indentOf(block[0]), `${where} ${key}`) : "";
    } else {
      if (block.length) throw new Error(`${where}: ${key} has a value and lines under it`);
      out[key] = scalar(rest, `${where} ${key}`);
    }
    i = j;
  }
  return out;
}

// One job of the workflow: its own keys (runs-on, timeout-minutes, ...) and
// its steps, each { name, keys, env, with, run }.
function job(id, file = WORKFLOW) {
  const where = relative(REPO, file);
  const lines = fs.readFileSync(file, "utf8").split("\n");
  const start = lines.indexOf(`  ${id}:`);
  if (start < 0) throw new Error(`${where} has no job ${id}`);
  let end = lines.findIndex((l, i) => i > start && /^ {0,2}\S/.test(l));
  if (end < 0) end = lines.length;
  const body = lines.slice(start + 1, end).filter((l) => l.trim() !== "" && !/^\s*#/.test(l));
  const at = body.indexOf("    steps:");
  if (at < 0) throw new Error(`${where}: job ${id} has no steps`);
  const keys = mapping(body.filter((l, i) => i < at || (i > at && indentOf(l) <= 4)), 4, `${where} ${id}`);
  const items = [];
  for (const line of body.slice(at + 1)) {
    if (indentOf(line) <= 4) break;
    if (/^ {6}- /.test(line)) items.push([" ".repeat(8) + line.slice(8)]);
    else if (items.length) items[items.length - 1].push(line);
    else throw new Error(`${where} ${id}: a line before the first step: ${JSON.stringify(line)}`);
  }
  const steps = items.map((step, n) => {
    const k = mapping(step, 8, `${where} ${id} step ${n + 1}`);
    return { name: k.name || null, keys: k, env: k.env || {}, with: k.with || {}, run: k.run === undefined ? null : k.run };
  });
  return { keys, steps };
}

module.exports = { job, RAN, RECORDER, WORKFLOW };
