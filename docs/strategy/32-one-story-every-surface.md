# 32 — One story on every surface

**Thesis.** Thirty-one documents in this series decided what SecuraCV is, who
it is for and how it makes a living. None of them decided what a stranger
reads first. By November 2026 the project spoke from six places — the website,
the Lab, three READMEs, two Home Assistant store pages, the apps' own atlases
and the devices' screens — and each had been written when it was built, by
whoever built it. The product was coherent. The account of it was not.

This document is the read of all of it, the direction that read supports, and
what was changed the same week. It is written the way docs 20 and 31 are: the
decision first, then the evidence, then what holds it in place.

## What was read

- every page on securacv.com (sixty on its map at the start, sixty-one now),
  with a census of each page's title, description, canonical link, share
  tags and structured data;
- the Lab (`canary-local/`, twenty-six pages), its manifest and its room;
- the three READMEs a newcomer lands on, the Home Assistant app's store page
  and the HACS mirror's;
- the desktop Flasher's atlas, the iPhone app's family map, the firmware's
  boot banner and help QR codes, and the back office's outgoing mail;
- docs 01–31 here, [`BRAND.md`](../BRAND.md), the claims and legal audits and
  the 2026-09 TODO sweep.

Eight reviewers read in parallel, each with one surface and the same rules;
every defect below was then checked against the code before anything moved.

## The diagnosis

**1. The people the project reaches first had no door.** Doc 08 names the
beachhead (a household already running Home Assistant and cameras); doc 17
names Home Assistant as the distribution. The app and the integration are
released, and the install is two buttons with nothing to type. On the
website, fourteen pages mentioned Home Assistant and none of them was about
it. The site's own search answered "home assistant" with the Apple Home
page. The HACS store page had no link to the website at all, and "set up
Home Assistant" led to five different places depending on where you asked.

**2. Three families of pitch.** GitHub and the store pages said *witness
layer* and *witness log*. `BRAND.md` and the launch kit said *a security
camera that…*. The website said *a camera, a notary and a vault*. The word
"notary" appeared nowhere in this repository. `BRAND.md` itself warns that
five taglines are zero; there were more than twenty.

**3. Four maps, and names that changed between them.** The site's journey
page, the Lab's shell, the Lab's room and the Lab's site map each drew the
six stages with different stage names. A page was "Makers" in the footer,
"Build" in the header and "Are you a maker?" at the top of itself. "Help"
in the Lab's live bar and "Help" in its static bar opened different pages.

**4. Whatever was typed by hand had drifted.** Every page's `<head>` was
hand-written: six had no share URL, none named the site, one page of sixty
had structured data, the privacy policy was marked `noindex` while the
sitemap offered it, and the page a customer sees after paying was offered to
search engines. Across repositories it was worse, because nothing connected
them: the Lab's footer opened a page that had never existed, the welcome
mail linked a 404, the desktop atlas opened an anchor the home page lacks.

**5. Claims the code does not back.** Not many, and each one specific:
a demo on the home page printing event types the kernel does not have; a
store listing saying the WAP has no camera and no microphone (it has both);
an untested ingress rating on two cases; "signed self-test" (the self-test
is plain JSON; the boot record is what is signed); a first-device answer
recommending a Canary with no released firmware; "verified" used for a
board tier on a site that reserves the word for a checked signature; and
one sentence, repeated on four pages, that fused a Canary's single-key
snapshot with the kernel's quorum vault.

**6. Things a visitor could not open.** The website's repository is private,
and seventeen public links pointed into it: the security policy named in
`security.txt`, the feedback desk's only button, the gallery's "share your
build", the vendors' "add your listing". The hosted copy of the Witness Wall
cannot reach a hub (a browser rule), and four pages offered it as working.
With scripts off, seven pages rendered blank. And every page fetched its
typeface from Google while the privacy policy said it fetched nothing.

None of this is a product problem. All of it is what happens when a project
that builds carefully publishes one surface at a time.

## The direction

**SecuraCV is the witness layer.** Home Assistant is how it reaches people
today. Canaries are the hardware you build now and buy later. The Lab is the
proof. One sentence says it, everywhere:

> SecuraCV is an open-source witness for the cameras and sensors you already
> own: it keeps a signed, tamper-evident record of what happened — never a
> face, a plate or a cloud archive.

This is doc 17's "kernel with a Prusa attached" turned to face the reader:
neutral software first, hardware as the thing that makes it tangible.

### Decision 1 — one message ladder

| Rung | Words | Where it goes |
|---|---|---|
| Signature | Witnessing without watching. | under the name, anywhere |
| Hook | Records what happened. Never who. | the home page headline, the share card |
| The sentence | (above) | the first paragraph, anywhere |
| For Home Assistant | Keep Frigate. Add proof. | the Home Assistant page, the store listings |
| The explanation | Three things, one witness: it sees, it proves, it protects. | the long form |

A surface picks the rung that fits its room. It does not write a sixth.
[`BRAND.md`](../BRAND.md) now carries this table and retires the sentence it
used to call the homepage's.

### Decision 2 — the witness in front, the Canary as the device

`BRAND.md` said the hero noun was the Canary. Three things moved it.

- What ships is software, and it involves no Canary. Leading with a device
  nobody can buy hid the product somebody can install tonight.
- [`TRADEMARK.md`](../../TRADEMARK.md) already says the word is used only in
  combination, because others hold rights in it for security cameras. The
  2026-09 sweep left this open as W15 and named three ways out; this is the
  one that needs no rename. **The clearance question itself is still the
  owner's**, and nothing here settles it.
- A witness is not always a camera: the WAP senses a Wi-Fi field, the Sense
  is radar, and beside Frigate the kernel keeps no video.

So: SecuraCV on the door, *witness* for what it is, *Canary* for the thing
in your hand. "One witness", never "one device".

### Decision 3 — the header is the audience order

Lab · How It Works · Home Assistant · Build · Store · Help. The order people
arrive in, per doc 08: see it, understand it, install it on what you have,
build one, buy one, get help. On the site this is a rank in the page map,
not a list someone types, so the header cannot drift from the decision.

### Decision 4 — one name per thing

| Thing | Its name | Names retired |
|---|---|---|
| The software | SecuraCV; the *witness kernel* when the part matters | "Privacy Witness Kernel" in running copy, "PWK" |
| The device | a Canary | bare "the Canary" as the project's name |
| The company | Errer Labs | "ERRERlabs", "ERRER Labs" |
| A plastic set | enclosure set | "kit" for anything without electronics |
| The page a header link opens | the name on the site's map | a second name in the page's own headline |

Home Assistant's own word is now *app* (it was *add-on*); copy says "app" and,
where a reader may be on an older release, says so once.

### Decision 5 — the experiment is over

The home page carried a dormant A/B test of two heroes. The site has no
analytics by policy, so the test could never be read. It is removed, and the
headline is the hook.

### Decision 6 — nothing a visitor cannot open

- The typeface is served by the site. The claims audit had left "self-host,
  or say so in the policy" as an owner's call; this project's guarantees are
  meant to be *can't*, and a content policy that names no Google host is a
  can't.
- The feedback and build-submission forms live in this repository, the
  public one, and the security policy the site names is this repository's.
- The Engine Room keeps its links to the website's own CI and marks each as
  collaborator-only, with the reason, until the owner decides whether that
  repository becomes public.

## What changed

| Surface | Change |
|---|---|
| Home page | one headline; the install card leads with the two buttons; the demo prints the kernel's real nine event types; readable with scripts off |
| A new page | `/home-assistant`: what the install adds, the three ways in, what "Sealed", "Verified" and "Failed" mean, what it does not do |
| Every page | title starts with the page's map name; a description a search result can show whole; one derived block for canonical, robots, share tags and structured data with breadcrumbs |
| Site map | generated from the page map; a page marked not-for-index leaves both |
| Journeys | Build opens five steps before a sign-up; Choose leads with what exists today; "Keep exploring" leads forward |
| Find Your Path | doors for Home Assistant, a new owner, and proving a record; every mapped page has a door |
| Claims | each item in finding 5, corrected, on the site and in the glossary and FAQ sources |
| The Witness Wall | the emulator's "verified" line no longer survives a live fleet; the hosted copy says why it cannot search |
| Privacy policy | says what pages do fetch, names the waitlist, re-dated |
| The Lab | Help opens the Help Desk from all three bars; the manifest's site links use routes; its site nav is the site's header; every page has a canonical link and share tags, a title that says SecuraCV, and a description a result can show |
| Desktop Flasher | four atlas cards whose blurb described another page; the dead anchor |
| iPhone app, family map | the Hub opens the Home Assistant page |
| Back office | the welcome mail's two links |
| This repository | the two issue forms; `SECURITY.md` covers the website; the kernel explainer's dead links |
| Brand | `BRAND.md` and the glossary's naming table carry decisions 1 and 2 |
| HACS mirror | links the site's Home Assistant page and Help Desk; says what is released |

## What holds it

A direction that lives in a document is the thing this document found
broken. Each decision above has a gate.

- **The site's head and site map are generated** from its page map and each
  page's own title and description, and byte-checked.
- **The site states every address it answers** — routes, redirecting short
  links, published files, each page's anchors — in the contract it already
  published for the files it fetches from here. The carry brings that to
  `canary-local/devices/site_contract.json`.
- **[`scripts/lint_site_links.py`](../../scripts/lint_site_links.py)** holds
  every securacv.com link in this tree to that list: literal URLs, an origin
  constant joined to a path, and the Lab manifest's hrefs. It was written
  against the four dead links in finding 4 and found them.
- **The anchors a Canary prints** as help QR codes are pinned on both sides.
  A device in the field keeps printing them, so they are permanent.
- **The issue forms the site opens** are paths in the same contract; a
  renamed form fails here.
- **The Lab's head is split the same way the site's is**
  (`canary-local/tools/gen_head.py`): the canonical link and share tags are
  stamped, and the hand-written title and description are checked.
- On the site: one headline, the header order, no link into a private
  repository, no font from another host, internal links by route, every
  install-story page linking the page that owns it.

## What only the owner can decide

Listed once, in the order they block something.

1. **Make the website repository public, or keep it private.** Public makes
   the Engine Room's promise true and removes the marks. Private is a
   legitimate choice; the site now behaves correctly either way.
2. **Re-affirm the warrant canary.** It is dated 2026-07-24, states no
   cadence and is unsigned. Only the owner can truthfully re-date it.
3. **The trademark clearance** (W15). Decision 2 lowers the exposure; it does
   not remove it, and the audit's advice not to scale store sales before it
   is settled stands.
4. **Prices.** The store data lists the WAP and Vision kits lower than doc 23
   models them. One of the two is stale.
5. **The store's opening sequence**: the first authorized radio SKU, the
   entity, insurance (docs 27, 29).
6. **One origin for the Lab.** It lives on a GitHub Pages address under a
   different name than the site. A subdomain would let the two share a
   header, a search and a share card.
7. **Store listings**: the App Store for the two Apple apps, HACS's default
   catalog for the integration.
8. **The device's own words.** The boot banner and one console scene say
   "tamper-proof"; the rule everywhere else is *tamper-evident*. The string
   is compiled into six emulator bundles, so the fix is a firmware change
   plus the dist rebuild, and it changes what a device prints.
9. **Labels.** The forms and the sign-up links name issue labels
   (`feedback`, `build-gallery`, `maker-corps`, `build-request`) that this
   repository does not have yet; GitHub drops an unknown label silently.
10. **A name and an address.** No page names a person, an entity's state or
    a mailing address, and every "talk to us" is one Gmail address.
11. **The social queue.** Several queued drafts use the retired claims
    ("24-hour memory", clips that "delete themselves"). They are drafts, so
    nothing was published; they should not be.

## Backlog, in order

1. **One map for the six stages.** The site's journey page should render
   from the Lab's manifest instead of restating it.
2. **Lab bench names.** The page titles now lead with the manifest's name
   for each bench; seven benches still carry the old name in their own
   headline and in links from other benches.
3. **The Lab's footers.** Sixteen pages end "nothing here phones anywhere".
   It is true of the page (its content policy allows only its own origin),
   and it is the phrase the claims audit retired elsewhere because it reads
   as a claim about the product. One wording for both sites.
4. **The device status vocabulary.** The site has four words and a ladder;
   the Lab says "released" for things the registry calls unfinished. One
   vocabulary, carried.
5. **Fold four pages.** The Story into the Showroom, the Maker Corps into
   Build, Feedback into Contact, the Demo Board into News. Each is a page
   that exists because it was built, not because a reader needs two.
6. **`launch_posts.md` and the READMEs.** The launch kit predates the claims
   audit; the front README has no link to the site's install page.
7. **A flashing path with no detour.** "Flash it" on three surfaces opens a
   path picker rather than the flasher.
8. **News with nothing in it.** The page promises everything announced, and
   nothing has been. Demote it from the footer until the first post.

## The test

Ask three people who have never seen the project to open any one surface —
the store page, the README, the home page, the Lab — and then say what it
is. Before this pass they would have given three answers. The direction is
working when they give one, and it is the sentence above.
