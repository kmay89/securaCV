# Canary Display — First-Boot Onboarding

> Status: **display-side SHIPPED** (`FEATURE_ONBOARDING`, compile/CI-verified;
> bench validation is runbook §F8). This closes the last "follow-up to finish
> the magic" in [`display_discovery_and_resilience.md`](./display_discovery_and_resilience.md):
> the fleet can hand a display its *broker*, but only once the display is on
> the LAN — WiFi itself needed a human, and until now that human needed a
> compiled `secrets.h`.

## Zero-touch first: flasher-seeded Wi-Fi wins

The portal below is the *fallback*, not the front door. Both flashers bake
Wi-Fi (and optionally the broker) straight into the image's NVS at install
time — namespace `securacv`, keys `wifi_ssid`/`wifi_pass` as strings — and the
boot loader honors them **before** onboarding is consulted: `provision_run()`
only starts when the stored SSID is a placeholder. A seeded key that exists is
taken at face value even when empty (an open network's password *is* empty;
substituting the compiled placeholder was the bug that made seeded open
networks fail). So the expected first boot for a flasher-installed display is:
no QR, no portal — it just joins, and the on-glass setup never appears.

## The promise (when nothing was seeded)

Plug the display in. It says hello, shows a QR code, and ninety seconds later
it's watching your canaries — **without the user typing an IP, installing an
app, or touching the glass once.** Setup is the product's first impression;
it should feel like the device is doing the work.

## The flow, end to end

```
  GLASS                                PHONE
  ─────                                ─────
  "Hello. Let's get you connected."
        (2.6 s welcome beat — the
         radio pre-scans behind it)
  [QR]  "Scan me"                      camera sees WIFI: QR →
        SecuraCV-A7K2 · p7Rm2Kqf         "Join SecuraCV-A7K2?" → tap
  "Nice - check your phone"            captive sheet pops automatically
        (breathing halo)               dark portal, network list already
                                         loaded (pre-scan), signal bars
                                       tap network → password → Join
  "Joining <HomeNet>…"                 button becomes spinner,
        (sweep arc)                      polls live status
  "You're in." (green bloom)           ✓ draws in: "You're all set"
        cross-fade to the normal UI      "the display takes it from here"
  "looking for your canaries…"
        → fleet finds the broker (mDNS gossip) → fleet fades in
```

On glass too narrow for `SecuraCV-A7K2 • p7Rm2Kqf` on one row (the round
watch, and the 172/180 px nightstand and nightlight) the name and the key take
a row each, the way round glass always has. The name and the key stay on the
glass for as long as the Join scene is up — QR or no QR, hint or no hint: when
the QR does not scan (or never rendered) that text is the only way in, and the
stuck-phone hint ("forget it on your phone") comes up exactly when the phone
needs the key again. On that glass the hint gets a row of its own: under the
key on the nightstand and nightlight, the title's band on the round watch
(the title yields while the hint stands: with the QR up, the card speaks for
itself; without one, the hint replaces "On your phone").
None of those rows (the name, the key, the hint) is ever cut to an ellipsis
(F45).

Nor is any scene's title or body on small glass (F65). Each line is fitted
to the width at its own latitude — the disc's chord on the round watch, the
panel less its side pads on rectangular glass and no wider than the halo's
inner chord there — and tries its words in the Character's own face, then a
shorter form in fewer of the same words, then the default Character's face.
Two lines have a shorter form: "Nice - check your phone" becomes "Check your
phone", and the Fail reason "No address from the router" becomes "No
address" (`join_failure_label_narrow()`; the fix under it still names the
router). "No address" shows on every small glass except the round watch at
the default type; "Check your phone" shows on the same glass except, also,
the AMOLED at the default type. Three different reasons put them there:

- on the 172/180x320 portrait glass no face holds the whole line in the
  panel's 156/164 px;
- on the 240x280 touch169 (both types) and the AMOLED, the panel would hold
  it — the old rows did, 224 and 434 px wide — but the halo's inner chord
  does not (a title gets 160 px on the touch169, 154 under Heirloom, and 314
  on the AMOLED, 308): that is F66's cost, the lines now sit inside the
  ring;
- on the round watch under Heirloom, the ladder reaches the shorter form in
  Heirloom's face before the whole line in the default face, though the
  whole line would fit there (221 px of "No address from the router" on a
  222 px line).

The last two are decisions, not limits of the glass, and are filed as such.
Everything else reads whole, stepping down to the default Character's type
under Heirloom where it must ("On your phone" on the round watch's 142 px
title row; on the portrait glass and the touch169, the bodies, "Check your
phone" and the other four Fail titles). Wide glass (the dash line) sets its
scene titles and bodies content-sized, as before; only its network name is
fitted.

The one line whose words are not ours is the network name in "Joining
<HomeNet>": it is whole in the line's face or the default Character's, and
a name too wide for either keeps its head and its tail around "..." (the
32-byte "Basement-Mesh-Extender-Office-5G" reads "Basement-M...-Office-5G"
on the 172 px nightstand). The end of a network name is what tells a
household's networks apart ("-5G", "_2.4", "-EXT"), and the 2.4 GHz band is
the first thing a not-found failure asks about, so the tail stays; the cut
falls between characters (the old 28-byte clip could split one). The dash
fits the name the same way, on its panel's row.

The same holds for the hints after the Join scene. When the phone sits on the
setup network without opening the page, the glass adds "no page? open
192.168.4.1"; after a failed join it names the fix under the reason
("passwords are case-sensitive", "it only sees 2.4 GHz wifi - not 5"). Those
scenes leave the credentials rows empty, so the hint has both of them: whole
on one row where it fits, over the two rows where it does not ("no page?" over
"open 192.168.4.1" on the round watch). A shorter form of the same fix
(`join_failure_hint_narrow()`) is there for a row that would hold neither; no
display's glass needs it today, and the host test fails if one ever does.
None of these hints is cut either (F50).

The last step is the payoff of the whole discovery program: the moment WiFi
exists, the **fleet referral** (broker gossip, discovery doc §5.1) configures
MQTT with zero further input. Onboarding ends at a *working* display, not at
a joined network.

## Choreography (the motion contract)

Setup is an active-attention moment, so it earns slightly more motion than
the ambient UI — still enumerated, still rationed:

| Motion | Timing | Job |
|---|---|---|
| Scene fade-in | 260 ms ease-out | one transition per state change, content as a unit |
| Waiting breath | 2.4 s cycle, 30↔70 % opacity | "I'm alive and waiting for you" — halo only |
| Connect sweep | 1.2 s/rev, 70° arc | "working on it" — replaces the breath, never joins it |
| Success bloom | 500 ms, edge→ok-green | the one earned flourish |
| Handoff cross-fade | 420 ms | onboarding screen → live fleet UI, then every setup object is freed |
| Portal: sheet slide-up | 240 ms | password entry appears |
| Portal: error shake | 160 ms ×2 | wrong password — felt, not read |
| Portal: ✓ draw-in | 500 + 350 ms | completion, drawn not popped |

Nothing else moves. The halo ring persists across every glass scene so the
eye has continuity while the words change. On the round watch it rides the
rim, and every row sits inside it (fitted to the disc's chord at the ring's
inner edge, so a full-width row's box reaches that edge, its corners within
a pixel of the stroke; rectangular glass keeps 2 px). On
rectangular small glass it sits in the band between the Join scene's title
and its credentials rows, concentric with the QR card (the way the dash's
halo is with its card), so no row under it — the credentials, the hint, a
split coach line — runs through its arc; the old 236 px ring crossed the
hint row on the 172/180x320 portrait glass and the 240x280 touch169 (F66).
That band gives 196 px on the portrait glass (192 under Heirloom), 176 on
the touch169 (172) and 328 on the AMOLED (322). On the narrow portrait glass
the ring is wider than the panel and runs off its sides, as it always did.
The centered lines are fitted inside it, which costs the touch169 and the
AMOLED the shorter forms above. On the touch169 the 128 px QR card's rounded
corners would have met that ring (the old one cleared them), so its canvas
gives up a few pixels there — 106 px, 104 under Heirloom, still 3 px a
module, the join code's pitch unchanged — and the card's corners stay
inside the ring like the rows (`small_join()`).

The bird sits where the scene places it: over the title, inside the halo
(on the 240x280 touch169 under Heirloom, 1 px lower than elsewhere to stay
inside), and in the QR card's empty seat when the Join scene has no code to
show. It used to ride the panel's center behind "Hello." and then walk
further off the glass at each scene: the mark read its seat from LVGL's
layout before the layout had run (F64). The Success scene's one hop rises
from that seat; on the touch169 the top of the hop reaches the halo's arc.

## No dead ends (the recovery matrix)

| What goes wrong | What the user sees | Recovery |
|---|---|---|
| Wrong password | Portal: inline "Wrong password", field cleared + refocused, sheet shakes. Glass: amber "Wrong password — try again on your phone" | resubmit; AP never dropped |
| Network out of range / gone | "Network not found" (distinct from wrong password — WAP lesson) | pick another network |
| Router slow / edge of range | 30 s budget before "Couldn't connect" (15 s reads as a false "wrong password" at range edge) | retry |
| Captive sheet never pops | 4 s after the phone joins, the glass quietly adds "no page? open 192.168.4.1" | manual URL |
| Phone leaves the AP mid-setup | glass returns to the QR scene | rescan |
| User walks away | join scene breathes indefinitely; glass dims after 8 min (touch or a joining phone re-wakes) | resume any time |
| Power cycle mid-setup | credentials persist **on success only** — an interrupted setup restarts clean | start over, ~90 s |
| Panel dead (bench fault) | wizard runs serial-guided: AP + portal still work, SSID/password print on serial | provision headless |
| NVS write fails | session continues on the joined network; wizard reruns next boot (honest, logged) | re-run |

The phone's success page and the glass's "You're in." are **two independent
success channels**: the AP lingers after success (max 25 s, or 1.6 s after
the phone's status poll actually observed the verdict) so the phone wins the
STA channel-change race — but even if it doesn't, the user looks up and the
glass says so. (Tearing the AP down instantly makes every *successful* join
look failed on the phone — hard-won WAP lesson.)

## Security posture

- **AP identity**: SSID `SecuraCV-XXXX` from the salted, MAC-free device
  pseudonym; password **random per session**, 8 chars of the unambiguous
  alphabet (no `0/O/o`, `1/I/l`), ~46 bits. It is displayed on the glass and
  embedded in the join QR — ephemeral beats memorable, and nothing derivable
  from published identifiers gates the AP. Max **1 client**, WPA2, channel 1.
- **Captive DNS** answers A-queries only and returns NODATA for AAAA/HTTPS
  (the stock catch-all responder stalls Android Chrome) — pure,
  host-tested builder in `provision_core.h`.
- **Hostile SSIDs** (anyone can broadcast one): scan results reach the portal
  as escaped JSON and land via `textContent` — inert by construction; the
  escaping is host-tested.
- **Credentials**: travel one hop over the WPA2 AP, persist to NVS **on
  verified success only**, never appear in logs. Footer says it plainly:
  *no account · no cloud · your password goes only to this device.*

## Implementation map

| Piece | Where |
|---|---|
| Pure helpers (QR/JSON escaping, password alphabet, captive DNS, probe policy) | `firmware/common/network/provision_core.h` (host-tested) — included directly from common/; the display's former byte-identical copy and its sync gate are gone (the Arduino sketch stages it flat via `setup.sh regen`) |
| State machine + AP + portal | `src/net/provision.cpp` (`FEATURE_ONBOARDING`) |
| Glass scenes | `src/ui/onboard_ui.cpp` — own LVGL screen, auto-deleted at handoff |
| Join-scene geometry | `include/canary/ui/onboard_layout.h` — title, QR card and caption lines stacked from the panel and the fonts' line heights, never crossing (host-tested on every display env's panel by `tests_host/test_onboard_layout.cpp`; `canary-local/tests/onboard_probe.mjs` checks the card is clean on each emulated flavor) |
| Scene words and the halo | the same header's `scene_copy()` (every scene's title and body, and their shorter forms), `scene_line_w()` and `name_line()` (F65), `small_join()` (`halo_ring()` and the card inside it) and `scene_bird_top()` (F66). `tests_host/test_onboard_layout.cpp` holds the rules on every small-glass env (the round watch, the 172/180x320 portrait glass, the touch169, the AMOLED) with both ladders: it fits every title and body of every scene and requires each one whole (the network name: whole, or its head and tail around "..."), pins where the two shorter forms show, and holds every row of every scene, the QR card and the bird's seats clear of the halo's stroke. On wide glass it measures only the network name. `tests_host/test_onboard_scenes.cpp` holds `onboard_ui.cpp` to those rules: it compiles the real module against `tests_host/fake_lvgl/` (LVGL 8's position rules), drives every scene the way `provision.cpp` does on the same envs and ladders, and reads what each label says at what width in which font, where the halo, the card and the bird are drawn. The probe fails on an ellipsis in each flavor's PhoneJoined scene and its wrong-key and absent-network Fail scenes |
| The bird's seat | `src/ui/canary_mark.cpp` records the host's own offset from its anchor (`lv_obj_get_style_x/y`), so the bird is drawn where `lv_obj_align` put it, before or after a layout pass (F64; `tests_host/test_canary_mark_seat.cpp`) |
| Join-scene text | the same header's `join_lines()` — the credentials joined where they fit their row, else split (name, then key, and a standing hint on the note row: nothing displaces the name or the key), shorter forms before a smaller face, never an ellipsis (F45). The host test measures every glass and ladder with LVGL's glyph metrics (`tests_host/montserrat_metrics.h`, from `firmware/scripts/gen_montserrat_metrics.py`) over the widest name and key the unit can mint, and requires both on the glass with and without the stuck-phone hint; the probe fails on an ellipsis in each flavor's Join scene and reads the firmware's own labels (the emulator's `emu_screen_labels`) for the name and key it printed, before and after the hint. After the Join scene, `hint_lines()` gives the coach line both of the empty credentials rows: whole on one where it fits, else split at a clause over two, else the shorter form (F50). The host test runs every failure's fix and the "no page?" hint through it on every env and ladder and requires the whole form; the probe reads the whole "no page?" hint and the wrong-key and absent-network fixes off each flavor's glass |
| NVS persistence | `canary::cfg::set_wifi_credentials()` (success only) |
| Boot hook | `main.cpp`: placeholder creds → `provision_run()` before the watchdog arms |
| Captive mechanics provenance | canary-wap wizard, `LESSONS_LEARNED` §captive-portal |

## Bench acceptance (runbook §F8)

iPhone + Android camera-scan → auto-join → sheet pops → wrong-password round
trip (specific reason, no dead end) → correct join → glass bloom → phone ✓ →
fleet referral lands the broker → fleet renders. Power-cycle mid-setup
restarts clean. Panel-dead build provisions over serial.
