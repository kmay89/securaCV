# The Desk of Worlds — personal portfolio site

A single-file, self-contained resume/portfolio site (`index.html`): an isometric,
cartoon-rendered 2000s desk — beige CRT running an ATM diagnostic, ball webcam,
Game Boy, CD jewel case, alarm keypad, book stack — where each object is one
shipped world: the ATM/hardware years, the award-winning bilingual book + toy,
Hive Mind, the Everything series, SCQCS, ERRER Labs / Aethra Kairos, and securaCV.
The scene is generated programmatically (one 2:1 projection function, consistent
lighting and outlines) — no images.

## Features

- **Zero dependencies** — no frameworks, fonts, images, or network requests.
  One HTML file; works from `file://`, GitHub Pages, Netlify, or any static host.
- **Sound** — every object has a synthesized signature sound (Web Audio API):
  ATM keypress + card-reader chirp, marimba arpeggio, swarm buzz, page turn,
  alarm chirp, vinyl crackle + chord pad, camera shutter. Toggle in the hint bar.
- **Haptics** — per-object vibration patterns on supporting devices (`navigator.vibrate`).
- **Motion** — pointer parallax by object depth, ambient dust motes in the lamp light
  (canvas), floating idle animation. All disabled under `prefers-reduced-motion`.
- **Accessible** — objects are real buttons, the dossier is a labeled dialog with
  focus management and ESC-to-close, visible focus states throughout.

## Customizing

- Name / wordmark: search for `K. May` in `index.html` (`<title>`, header eyebrow,
  footer) and swap in the full name.
- Copy and links: everything lives in the `WORLDS` object in the `<script>` block.
- Desk layout: objects are built in the "build the desk" section — world coords are
  plain numbers on a 560×380 desktop plane (`+x` right-down, `+y` left-down, `+z` up).
- Colors: the per-world accents are the `--c-*` custom properties in `:root`.

## Deploying

Any static host works. For GitHub Pages: Settings → Pages → deploy from branch,
folder `/portfolio` (or copy `index.html` to the site root of a dedicated repo).
