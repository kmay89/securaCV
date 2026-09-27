// canary-local/tests/probe_server.mjs — the static file index the browser
// probes serve the repository through.
//
// A probe's server must never turn a request URL into a filesystem path: the
// URL is untrusted input, and a resolve-then-check dance is exactly the shape
// CodeQL's path-injection query flags (twice, on #1737). So the tree is
// indexed ONCE, up front, and a request is a Map lookup — the path that
// reaches readFile is the index's own string, never the request's.
import { readdirSync, statSync } from "node:fs";
import { join, relative, sep } from "node:path";

const SKIP = new Set([".git", "node_modules", "third_party", ".build", "target", "__pycache__"]);

/** Every regular file under root, keyed by its URL path ("/a/b.js" → absolute path). */
export function indexTree(root) {
  const files = new Map();
  const walk = (dir) => {
    for (const name of readdirSync(dir)) {
      if (SKIP.has(name)) continue;
      const abs = join(dir, name);
      let st;
      try { st = statSync(abs); } catch { continue; }
      if (st.isDirectory()) walk(abs);
      else if (st.isFile()) files.set("/" + relative(root, abs).split(sep).join("/"), abs);
    }
  };
  walk(root);
  return files;
}

/** The absolute path a request URL names, or null when the tree has no such file. */
export function lookup(files, url) {
  let path = url.split("?")[0];
  try { path = decodeURIComponent(path); } catch { return null; }
  return files.get(path) ?? null;
}
