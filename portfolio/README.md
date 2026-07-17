# The Desk of Worlds — personal portfolio site

A single-file, self-contained resume/portfolio site (`index.html`): a lamp-lit desk
where each object is one shipped world — the ATM/hardware years, the award-winning
bilingual book + toy, Hive Mind, the Everything series, SCQCS, ERRER Labs / Aethra
Kairos, and securaCV.

## Features

- **Zero dependencies** — no frameworks, fonts, images, or network requests.
  One HTML file; works from `file://`, GitHub Pages, Netlify, or any static host.
- **Sound** — every object has a synthesized signature sound (Web Audio API):
  ATM keypress + card-reader chirp, marimba arpeggio, swarm buzz, page turn,
  alarm chirp, vinyl crackle + chord pad, camera shutter. Toggle in the hint bar.
- **Haptics** — per-object vibration patterns on supporting devices (`navigator.vibrate`).
- **Motion** — pointer parallax by object depth, ambient dust motes in the lamp light
  (canvas), floating idle animation. All disabled under `prefers-reduced-motion`.
- **Accessible** — objects are real buttons, the dossier is a labelled dialog with
  focus management and ESC-to-close, visible focus states throughout.

## Customizing

- Name / wordmark: search for `K. May` in `index.html` (`<title>`, header eyebrow,
  footer) and swap in the full name.
- Copy and links: everything lives in the `WORLDS` object near the top of the
  `<script>` block.
- Colors: the per-world accents are the `--c-*` custom properties in `:root`.

## Deploying

Any static host works. For GitHub Pages: Settings → Pages → deploy from branch,
folder `/portfolio` (or copy `index.html` to the site root of a dedicated repo).
