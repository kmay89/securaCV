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
  "Nice — check your phone"            captive sheet pops automatically
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
eye has continuity while the words change.

## No dead ends (the recovery matrix)

| What goes wrong | What the user sees | Recovery |
|---|---|---|
| Wrong password | Portal: inline "Wrong password", field cleared + refocused, sheet shakes. Glass: amber "Wrong password — try again on your phone" | resubmit; AP never dropped |
| Network out of range / gone | "Network not found" (distinct from wrong password — WAP lesson) | pick another network |
| Router slow / edge of range | 30 s budget before "Couldn't connect" (15 s reads as a false "wrong password" at range edge) | retry |
| Captive sheet never pops | after 9 s the glass quietly adds "no page? open 192.168.4.1" | manual URL |
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
| Pure helpers (QR/JSON escaping, password alphabet, captive DNS, probe policy) | `include/canary/net/provision_core.h` (host-tested) — a byte-identical copy of the canonical `firmware/common/network/provision_core.h`, pinned by `firmware/scripts/check_provision_core_sync.sh` until the display migrates to the shared portal |
| State machine + AP + portal | `src/net/provision.cpp` (`FEATURE_ONBOARDING`) |
| Glass scenes | `src/ui/onboard_ui.cpp` — own LVGL screen, auto-deleted at handoff |
| NVS persistence | `canary::cfg::set_wifi_credentials()` (success only) |
| Boot hook | `main.cpp`: placeholder creds → `provision_run()` before the watchdog arms |
| Captive mechanics provenance | canary-wap wizard, `LESSONS_LEARNED` §captive-portal |

## Bench acceptance (runbook §F8)

iPhone + Android camera-scan → auto-join → sheet pops → wrong-password round
trip (specific reason, no dead end) → correct join → glass bloom → phone ✓ →
fleet referral lands the broker → fleet renders. Power-cycle mid-setup
restarts clean. Panel-dead build provisions over serial.
