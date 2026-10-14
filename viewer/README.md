# The offline evidence viewer

`evidence_viewer.html` is a single file that checks a SecuraCV evidence export
in your browser, with the network off. Nothing is uploaded; every signature and
hash is re-derived on your own computer by the same algorithm as the project's
Rust verifier, and the two are pinned to identical test fixtures in CI.

This page is written for the person who **receives** an export — a neighbor, a
tenant, an insurer, a lawyer, a journalist — not for the owner who made it.

## What you were given

Home Assistant's **Export evidence** action hands the owner one zip,
`securacv-evidence-<date>-<id>.zip`, and the owner sends you the whole zip. Inside:

| File | What it is |
|---|---|
| `securacv-events-<window>.json` | the sealed export, byte for byte as the witness kernel wrote it — **this is the evidence** |
| `evidence_viewer.html` | this viewer |
| `WITNESS_KEY.txt` | the owner's witness key (64 hex) and its fingerprint (8 hex) |
| `README.txt` | the two-paragraph recipe, the same as below |

An export made from the command line (`export_events`) is the `.json` alone;
an older full *evidence envelope* is a `.json` too. The viewer recognizes all of
them.

## Check it (two minutes)

1. Open `evidence_viewer.html` in a current browser — Chrome, Edge, Firefox,
   or Safari 17 and later. It works with the network unplugged; open the
   browser's Network tab if you want to watch it stay empty.
2. Drop the **zip itself** onto the page (the bundle inside is read in place),
   or drop the `.json`. Nothing is uploaded and nothing is changed; checking
   cannot hurt the file.
3. Read the verdict at the top. Keep the zip exactly as you received it — it
   is the evidence; the page is only a check of it.

| Verdict | Meaning |
|---|---|
| **Verified — integrity intact** (all N checks passed) | every event carries a valid signature, is chained to the one before it, and nothing was added, removed or reordered since it was sealed; this file is exactly what was exported |
| **Verified — with notes** | integrity intact; read the notes. The usual one is *self-attested* (below) |
| **Not verified — rejected** | a check failed. A truncated download fails the same way as tampering, so ask for a fresh export; if a fresh copy fails too, keep both and treat that as significant |
| **Inconclusive** | this browser could not run the Ed25519 check; open the same file in a current browser. Nothing valid is ever shown as rejected for this reason |

### "Self-attested" → "authorship confirmed"

The key that signed the record travels inside the record, so on its own the
export proves it is *internally consistent and signed by the key it names*,
not *who* that key belongs to. The viewer says so ("self-attested") until you
give it a key you obtained independently:

1. Ask the owner for their **witness key fingerprint** by phone or in person.
   Home Assistant shows it on the SecuraCV device page (the *Witness key
   fingerprint* sensor), and it is printed when the owner pairs a Docker
   sidecar (`docker exec securacv pair`).
2. Compare it with the fingerprint the viewer shows next to the bundle's key.
   `WITNESS_KEY.txt` is the owner's own copy of the same key: the viewer shows
   what it says and whether it matches the bundle, but a file that traveled in
   the pack can never be the independent confirmation — the phone call is.
3. Paste the 64-character key into the **expected device key** field and
   verify again. The verdict now reads **authorship confirmed**.

## What the viewer does not prove

- **What was in the video.** SecuraCV never sees pixels; Frigate or the camera
  keeps those. The record says *a person or vehicle crossed the front-door
  zone in the 14:00–14:10 window*, never who.
- **The exact second.** Times are deliberately 10-minute windows (and may be
  jittered); the export proves *what* was witnessed without exposing precise
  timing.
- **What happened outside the device's coverage**, or while it was off —
  though such gaps are recorded as explicit failure records, not hidden.
- **Who physically held the owner's keys.**

These limits are by design: the device records less so it can prove what it
does record. The page's *What verification proves — and what it can't* panel
repeats them with every result.

## Next step, if you need one

For a formal filing the owner can produce the **court kit** — RFC 3161
timestamps, a `sha256sum` / `openssl` walk-through anyone can repeat, an
affidavit draft — with SecuraCV's `court_export` tool
([`docs/court_export.md`](../docs/court_export.md)). It checks the same bytes a
different way; the zip you hold is the self-verifying bundle, not the kit.

## For developers

- `template.html` + `verify_core.js` + `timeline_core.js` + `zip_core.js` →
  `node viewer/build.mjs` → `evidence_viewer.html` (committed; `--check` is the
  CI drift gate). `verify_core.js` is the single source of verification logic
  and is also inlined into the Witness Wall.
- `zip_core.js` reads the pack's central directory and inflates the one member
  that matters with the browser's own `DecompressionStream('deflate-raw')` — no
  dependency, no network. A browser without it is told to unzip first.
- The Home Assistant integration ships a byte-identical copy as
  `custom_components/securacv/www/securacv-evidence-viewer.html` and puts it
  in every pack; `custom_components/securacv/www/securacv-evidence-viewer.test.js`
  fails if the copy drifts. Refresh it with
  `node viewer/build.mjs && cp viewer/evidence_viewer.html custom_components/securacv/www/securacv-evidence-viewer.html`.
- Tests: `node --test viewer/verify_core.test.js viewer/timeline_core.test.js viewer/zip_core.test.js`.
