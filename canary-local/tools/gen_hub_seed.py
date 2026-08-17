#!/usr/bin/env python3
"""canary-local/tools/gen_hub_seed.py — the hub's first-boot provisioning plan.

Emits canary-local/devices/hub_seed.json: the ORDERED list of steps that turn a
freshly-flashed Home Assistant OS into a working securaCV hub — the broker, the
witness kernel, Frigate, and the configs, in the order they have to happen.

Why a plan as *data* rather than a script: three different things need to agree
about this sequence — the on-device installer that executes it, the docs that
explain it, and the flasher UI that narrates it. Written three times they drift;
generated once from the repo's own sources, they can't. Same drift-gated posture
as gen_hub_image.py (its sibling: that one says WHAT image to write, this one
says what to do once it boots).

Every step carries `why` and `for_what` in plain language, because a user
watching an installer configure their home should be able to answer "what is
this doing to my house, and why?" at every line. That is a product requirement
here, not a nicety — see docs/full_stack_setup.md.

Derived, never hand-typed:
  1. canary-local/devices/hub_image.json ... the add-on set + their real
     Supervisor slugs and store repositories (itself generated from the add-on
     config), so a slug fixed there can't stay wrong here.
  2. homeassistant/frigate/config.yaml ..... the curated Frigate config this
     plan installs, and the destination the Frigate add-on actually reads.

Run:  python3 canary-local/tools/gen_hub_seed.py
CI:   the same command + `git diff --exit-code canary-local/devices/hub_seed.json`.
"""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUT_JSON = REPO / "canary-local/devices/hub_seed.json"
HUB_IMAGE_JSON = REPO / "canary-local/devices/hub_image.json"
FRIGATE_CONFIG = REPO / "homeassistant/frigate/config.yaml"

# The Frigate add-on does NOT read /config/frigate.yml — it reads config.yml in
# its own add-on config directory. Getting this wrong is the classic "I edited
# the config and nothing changed" failure, so the destination is stated once,
# here, and everything else reads it from the emitted plan.
FRIGATE_CONFIG_DEST = "/addon_configs/{slug}/config.yml"

# The Supervisor REST API can register an add-on store repository; the `ha` CLI
# cannot (which is why the manual path tells people to click through the UI).
# Stating it here is what lets the installer do it unattended.
SUPERVISOR_REPO_API = "POST http://supervisor/store/repositories"

# Optional feature: Pi-hole, network-wide ad/tracker DNS blocking. Not part of
# the securaCV mission (witnessing), which is exactly why it is OPT-IN — the
# default plan installs nothing a witness hub doesn't need. Poeschl's add-on
# repository is the maintained community packaging of the real Pi-hole for
# Home Assistant OS. Honest status: the slug follows the repository's own
# layout and the first real `--with pihole` run is what validates it end to
# end (same bar as everything else here).
PIHOLE_REPO = "https://github.com/Poeschl/Hassio-Addons"
PIHOLE_SLUG = "pihole"

# Optional feature: a screen plugged into the hub itself. The default hub is
# headless — that stays true and stays the default. But a Pi with an HDMI
# touchscreen attached (e.g. a 7" 1024x600 IPS panel with USB touch) can show
# the household dashboard right on the box, and HAOSKiosk is the community
# add-on built for exactly that: it starts an X server + Luakit browser ON the
# HAOS host and points it at Home Assistant, with touch input mapped to the
# panel. Local by construction — the browser talks to the hub it runs on.
# Honest status: same bar as Pi-hole above — the slug follows the repository's
# own layout and the first real `--with display` run on hardware is what
# validates it end to end. The add-on refuses to start until the operator
# types their own Home Assistant login into its configuration (a credential
# this plan must never mint or carry), so the step deliberately installs
# WITHOUT starting and hands the last move to the user.
KIOSK_REPO = "https://github.com/puterboy/HAOS-kiosk"
KIOSK_SLUG = "haoskiosk"


def die(msg: str) -> None:
    sys.exit(f"gen_hub_seed.py: {msg}")


def repo_hash(url: str) -> str:
    """Supervisor's 8-char repository hash: sha1(lowercased URL, no trailing '/')[:8].

    Verified against the two real slugs the hub uses — the frigate-hass-addons
    repo hashes to `ccab4aaf` and this project's repo to `d0491a67`, matching the
    `ccab4aaf_frigate` / `d0491a67_privacy_witness_kernel` add-ons a real
    Supervisor exposes. A trailing slash changes the hash, so it is stripped.
    """
    return hashlib.sha1(url.rstrip("/").lower().encode("utf-8")).hexdigest()[:8]


def supervisor_slug(addon_slug: str, repositories: list[dict]) -> str:
    """The slug the Supervisor REST API actually answers to for `addon_slug`.

    This is the single fact that makes install-by-API work rather than 404. An
    add-on's *friendly* slug (what it calls itself in its own config.yaml, e.g.
    `privacy_witness_kernel`) is NOT what `/store/addons/<slug>/install` wants —
    a third-party add-on is addressed as `<repo_hash>_<friendly_slug>`. The
    asymmetry is real in our catalog: Frigate already arrives prefixed
    (`ccab4aaf_frigate`) while securaCV arrives bare (`privacy_witness_kernel`),
    so without this the executor would install Frigate but fail on securaCV.

    Official add-ons (`core_`/`local_`) are global and keep their slug. For a
    third-party add-on we find the registered repository that provides it and
    prefix its hash — unless the slug already carries that prefix (Frigate), in
    which case it's returned unchanged.
    """
    if addon_slug.startswith(("core_", "local_")):
        return addon_slug
    for r in repositories:
        if addon_slug in r.get("provides", []):
            h = repo_hash(r["url"])
            return addon_slug if addon_slug.startswith(h + "_") else f"{h}_{addon_slug}"
    # Not official and not provided by any repo in this plan: don't invent a
    # hash. Leaving it bare makes the executor fail loudly (add-on not found)
    # rather than silently targeting the wrong thing.
    return addon_slug


def load_catalog() -> dict:
    if not HUB_IMAGE_JSON.exists():
        die(f"missing {HUB_IMAGE_JSON.relative_to(REPO)} — run gen_hub_image.py first")
    return json.loads(HUB_IMAGE_JSON.read_text(encoding="utf-8"))


def addon_by_source(catalog: dict, source: str) -> list[dict]:
    return [a for a in catalog.get("payload", {}).get("add_ons", []) if a.get("source") == source]


def find_addon(catalog: dict, slug_contains: str) -> dict:
    for a in catalog.get("payload", {}).get("add_ons", []):
        if slug_contains in a.get("slug", ""):
            return a
    die(f"no add-on matching {slug_contains!r} in hub_image.json — did the catalog change shape?")
    return {}  # unreachable; keeps type-checkers happy


def main() -> None:
    catalog = load_catalog()
    if not FRIGATE_CONFIG.exists():
        die(f"missing {FRIGATE_CONFIG.relative_to(REPO)} — the curated Frigate config is the source")

    mosquitto = find_addon(catalog, "mosquitto")
    frigate = find_addon(catalog, "frigate")
    securacv = addon_by_source(catalog, "securacv")
    if not securacv:
        die("no securacv add-on in hub_image.json")
    kernel = securacv[0]

    # Repositories that must be registered BEFORE the add-ons in them can be
    # installed. This is the step the manual path can't automate — the `ha` CLI
    # has no command for it — so it's called out explicitly with its API.
    repositories = [
        {
            "url": frigate["repository"],
            "provides": [frigate["slug"]],
            "why": (
                "Frigate is a third-party add-on: Home Assistant can't see it until its store "
                "repository is registered. Installing Frigate before this step is what fails with "
                "'add-on not found'."
            ),
            "api": SUPERVISOR_REPO_API,
            "cli_can_do_it": False,
        },
        {
            "url": "https://github.com/kmay89/securaCV",
            "provides": [kernel["slug"]],
            "why": "The securaCV add-on lives in this project's own repository, added the same way.",
            "api": SUPERVISOR_REPO_API,
            "cli_can_do_it": False,
        },
        {
            "url": PIHOLE_REPO,
            "provides": [PIHOLE_SLUG],
            "why": (
                "Community packaging of Pi-hole for Home Assistant OS. Only registered when the "
                "optional `pihole` feature is enabled — see that step."
            ),
            "api": SUPERVISOR_REPO_API,
            "cli_can_do_it": False,
            "feature": "pihole",
        },
        {
            "url": KIOSK_REPO,
            "provides": [KIOSK_SLUG],
            "why": (
                "HAOSKiosk's own add-on repository — the community add-on that shows a dashboard "
                "on a screen plugged into the hub. Only registered when the optional `display` "
                "feature is enabled — see that step."
            ),
            "api": SUPERVISOR_REPO_API,
            "cli_can_do_it": False,
            "feature": "display",
        },
    ]

    # The API-addressable slug for each add-on (see supervisor_slug's docstring).
    # Computed once, from the repositories above, so the executor, the docs, and
    # the flasher UI all install the SAME thing. The securaCV/Frigate slug
    # asymmetry (one bare, one already hash-prefixed) lived unnoticed until an
    # installer had to POST it — carrying the resolved slug in the plan is what
    # keeps that fixed everywhere at once.
    mosquitto_sup = supervisor_slug(mosquitto["slug"], repositories)
    frigate_sup = supervisor_slug(frigate["slug"], repositories)
    kernel_sup = supervisor_slug(kernel["slug"], repositories)
    pihole_sup = supervisor_slug(PIHOLE_SLUG, repositories)
    kiosk_sup = supervisor_slug(KIOSK_SLUG, repositories)
    for r in repositories:
        r["supervisor_slug"] = [supervisor_slug(s, repositories) for s in r["provides"]]

    steps = [
        {
            "id": "add-repositories",
            "title": "Register the app repositories",
            "what": "Tell Home Assistant where the Frigate and securaCV apps come from.",
            "why": (
                "Home Assistant only installs apps (older versions call them add-ons) from "
                "repositories it knows about. Both of "
                "ours are third-party, so nothing else in this plan can work until they're "
                "registered. The Supervisor API does this unattended; the `ha` command line "
                "cannot, which is why the by-hand path makes you click through the store UI."
            ),
            "for_what": "Makes the next two installs possible.",
            # Feature-tagged repositories register inside their own optional
            # step, so an un-enabled feature leaves zero footprint on the hub.
            "repositories": [r["url"] for r in repositories if not r.get("feature")],
            "reversible": True,
        },
        {
            "id": "install-broker",
            "title": "Install the Mosquitto broker",
            "what": f"Install and start the `{mosquitto['slug']}` add-on.",
            "why": (
                "MQTT is the shared language of the hub: cameras' detections and each Canary's "
                "events all arrive as MQTT messages, and Home Assistant discovers new sensors the "
                "same way. The broker is the post office they all hand messages to — nothing "
                "downstream can talk without it, which is why it goes first."
            ),
            "for_what": "The message bus every other piece publishes to and reads from.",
            "addon": mosquitto["slug"],
            "supervisor_slug": mosquitto_sup,
            "start": True,
            "reversible": True,
        },
        {
            "id": "mqtt-login",
            "title": "Make the login your Canaries sign in with",
            "what": (
                f"Add a `canary` account to the `{mosquitto['slug']}` add-on's own "
                "logins, with a password generated on the hub."
            ),
            "why": (
                "The broker refuses anonymous connections — every client must present a "
                "username and password. Home Assistant is exempt in practice: it reaches "
                "Mosquitto over the internal container network as the reserved "
                "`homeassistant` account, which is why the connect step below needs no "
                "credential. A Canary has no such exemption. It is an ordinary external "
                "client on your LAN, so without an account of its own a fully provisioned "
                "hub reports success and then rejects the first device that ever tries to "
                "publish — and on a screen the size of a matchbox that failure is "
                "indistinguishable from a wrong Wi-Fi password.\n\n"
                "A broker-local login rather than a Home Assistant user, deliberately: a "
                "camera is not a person. A Home Assistant account could also call the Home "
                "Assistant API; this one can only speak MQTT, which is the whole of what a "
                "Canary needs. The password is minted here on the hub and never lives in "
                "this repository, because a password committed here would be a published "
                "credential on every hub anyone ever flashed. The run prints it once; "
                "afterwards it is readable from the add-on's own configuration."
            ),
            "for_what": "The username and password you type into each Canary when you flash it.",
            "mqtt_login": {
                "addon": mosquitto["slug"],
                "supervisor_slug": mosquitto_sup,
                "username": "canary",
            },
            "reversible": True,
        },
        {
            "id": "connect-mqtt",
            "title": "Connect Home Assistant to the broker",
            "what": (
                "Create Home Assistant's MQTT integration entry pointing at the "
                f"`{mosquitto['slug']}` add-on."
            ),
            "why": (
                "Installing the broker is not the same as USING it. A running Mosquitto with no "
                "MQTT integration is a post office nobody has an address for: Frigate publishes "
                "detections, every Canary publishes events, and Home Assistant subscribes to "
                "none of it — so no entities appear and the hub looks empty and broken. On a "
                "hub with a keyboard you'd click through a dialog to fix that. Headless, nobody "
                "ever sees the dialog, which is why it has to be part of the plan."
            ),
            "for_what": "Turns broker traffic into Home Assistant entities.",
            "core_config_entry": {
                "handler": "mqtt",
                "data": {
                    # The add-on's in-cluster hostname, not localhost: Core runs
                    # in a different container from the add-on, so 127.0.0.1
                    # would point Core at itself and time out.
                    "broker": "core-mosquitto",
                    "port": 1883,
                    # Mosquitto's add-on defaults to authenticating against Home
                    # Assistant's own users, and Core reaches it over the
                    # internal network, so no separate credential is minted here.
                    # Deliberate: inventing a password would either commit a
                    # secret to this repo or leave one the operator can't find.
                    "discovery": True,
                },
            },
            "start": False,
            "reversible": True,
        },
        {
            "id": "install-frigate",
            "title": "Install Frigate (the camera eyes)",
            "what": f"Install the `{frigate['slug']}` add-on.",
            "why": (
                "Frigate is what actually looks at camera video and decides 'that's a person'. "
                "securaCV deliberately does NOT re-implement detection — it subscribes to "
                "Frigate's events over MQTT and turns them into signed, identity-stripped claims. "
                "So Frigate is the eyes; securaCV is the witness that writes down what was seen."
            ),
            "for_what": "Turns camera video into detection events on the broker.",
            "addon": frigate["slug"],
            "supervisor_slug": frigate_sup,
            # Deliberately NOT started here: Frigate needs its config (next step)
            # before it has anything to do, and starting it configless just makes
            # it crash-loop and look broken.
            "start": False,
            "reversible": True,
        },
        {
            "id": "write-frigate-config",
            "title": "Write Frigate's configuration",
            "what": (
                f"Copy the curated config to `{FRIGATE_CONFIG_DEST.format(slug=frigate['slug'])}`, "
                "then start Frigate."
            ),
            "why": (
                "A stock Frigate knows nothing about your hub. This config points it at the "
                "Mosquitto broker (so securaCV hears its detections), tracks the same objects the "
                "witness kernel labels, and — importantly — leaves recordings AND snapshots OFF: "
                "securaCV is 'claims, not recordings', so nothing stores raw imagery unless you "
                "deliberately turn it on. It also uses a distinct MQTT client id, so a second "
                "detector (say a Jetson) can join without the two knocking each other offline."
            ),
            "for_what": "Makes Frigate talk to your hub instead of sitting idle.",
            "source": str(FRIGATE_CONFIG.relative_to(REPO)),
            "dest": FRIGATE_CONFIG_DEST.format(slug=frigate["slug"]),
            "dest_note": (
                "The Frigate app reads config.yml from its OWN app config directory — NOT "
                "/config/frigate.yml. Editing the wrong file is the classic 'I changed it and "
                "nothing happened'."
            ),
            "never_overwrite": True,
            "then_start": frigate["slug"],
            "then_start_supervisor_slug": frigate_sup,
            "user_must_finish": (
                "Add your camera's RTSP URL and set `enabled: true` on it — the shipped example "
                "camera is disabled so the config is valid before you've added anything. Frigate "
                "ignores a disabled camera silently, so this is the one edit nobody can make for "
                "you."
            ),
            "reversible": True,
        },
        {
            "id": "install-securacv",
            "title": "Install the securaCV witness kernel",
            "what": f"Install the `{kernel['slug']}` add-on, set it to Frigate mode, and start it.",
            "why": (
                "This is the part that makes the system a witness rather than a camera system. It "
                "subscribes to Frigate's detections, strips identity, and writes each one into a "
                "signed, hash-chained log that can be verified later without trusting us. We set it "
                "to `frigate` mode BEFORE starting: its factory default is `standalone`, which only "
                "serves a setup wizard, so without this the 'unattended' install would quietly stop "
                "short of ever producing a claim. In frigate mode it auto-discovers the broker "
                "installed above (no MQTT to type) and generates + persists your device signing key "
                "automatically — open the app's panel afterwards to back that key up, because "
                "losing it means you can't verify your own log."
            ),
            "for_what": "Turns detections into tamper-evident, privacy-preserving claims.",
            "addon": kernel["slug"],
            "supervisor_slug": kernel_sup,
            # Set the add-on's options before starting. `mode: frigate` is what
            # flips run.sh out of "serve the wizard only" and into consuming
            # Frigate's MQTT events; the broker and topic_prefix ("frigate",
            # matching homeassistant/frigate/config.yaml) auto-discover from
            # their defaults, so mode is the one option that must be set.
            "options": {"mode": "frigate"},
            "start": True,
            "reversible": True,
        },
        {
            "id": "install-pihole",
            "title": "Install Pi-hole (optional: whole-network ad blocking)",
            "what": (
                f"Register `{PIHOLE_REPO}`, then install and start the `{PIHOLE_SLUG}` add-on."
            ),
            "why": (
                "Pi-hole is a small DNS server: your devices ask it 'where is this domain?', it "
                "answers, and it refuses known ad/tracker domains along the way. To tell you WHICH "
                "device asked, it logs the asking client's IP (and hostname, where the network "
                "supplies one) with the domain and the time — that is the feature, and it is also "
                "a record of your household's lookups, so: it stays on your hub, nothing is "
                "uploaded, and retention is yours to shorten or switch off in Pi-hole's settings "
                "(blocking still works with query logging off). It never sees page contents, only "
                "the names looked up. It's open source and has "
                "been run by millions of people for years — you're not trusting us, you're "
                "using the same tool everyone else uses. The reason it's the recommended pairing "
                "here: securaCV's whole promise is devices that DON'T talk out, and Pi-hole is "
                "how you check that promise instead of taking our word — one page shows every "
                "domain every device on your network tries to reach, so a counterfeit or "
                "compromised Canary (or any gadget) that starts phoning home shows up in plain "
                "sight. Skipping it changes nothing else; it binds DNS (port 53) on the hub and "
                "does nothing at all until your router points at it."
            ),
            "for_what": (
                "A local, readable answer to 'what is my network talking to?' — the check on "
                "securaCV's own quiet promise — plus ads and trackers blocked as a side effect."
            ),
            "feature": "pihole",
            "repositories": [PIHOLE_REPO],
            "addon": PIHOLE_SLUG,
            "supervisor_slug": pihole_sup,
            "start": True,
            "user_must_finish": (
                "Two things only you can do: set a Pi-hole admin password from the add-on's page, "
                "and point your router's DNS server at the hub's IP address (in the router's DHCP "
                "settings) so your devices actually ask Pi-hole. Until the router change, Pi-hole "
                "sits idle and nothing on your network behaves differently."
            ),
            "reversible": True,
        },
        {
            "id": "install-display",
            "title": "Install the hub display (optional: a screen on the hub itself)",
            "what": (
                f"Register `{KIOSK_REPO}`, then install the `{KIOSK_SLUG}` add-on — it shows "
                "your dashboard on a screen plugged into the hub."
            ),
            "why": (
                "The hub never needs a screen — headless is the default and stays fully "
                "supported. But if you've plugged one in (any HDMI monitor, or a touchscreen "
                "like a 7\" 1024x600 IPS panel with USB touch), this add-on puts it to work: "
                "it runs a small browser on the hub itself, signed in to your Home Assistant, "
                "showing the household dashboard full-screen — glance at the hallway, see the "
                "whole house. Touch works as touch; a wall-mounted hub becomes a control "
                "panel. Local by construction: the browser runs on the hub and talks to the "
                "hub, so turning your screen on adds no cloud, no account, and no new way "
                "for anything to leave your house."
            ),
            "for_what": (
                "Your whole-house dashboard, live on the screen attached to the hub — no "
                "phone or laptop needed to glance at it."
            ),
            "feature": "display",
            "repositories": [KIOSK_REPO],
            "addon": KIOSK_SLUG,
            "supervisor_slug": kiosk_sup,
            # Deliberately NOT started: the add-on refuses to run until it has a
            # Home Assistant login, and that credential is yours — this plan
            # never mints or carries one (a password in this repo would be a
            # published credential on every hub anyone ever flashed, and even a
            # hub-minted HA login would be an account with API rights nobody
            # asked for). Starting it configless would just crash-loop and look
            # broken, the same reason Frigate isn't started before its config.
            "start": False,
            "user_must_finish": (
                "One thing only you can do: give the browser on the hub a login. In Home "
                "Assistant open Settings → Apps → HAOS Kiosk Display → Configuration, enter "
                "your Home Assistant username and password, then press Start. The screen "
                "lights up with your dashboard. No screen attached? The add-on simply won't "
                "start — nothing else on the hub cares. The same Configuration tab also has "
                "zoom, rotation and screen-timeout settings for your particular panel."
            ),
            "reversible": True,
        },
    ]

    out = {
        "$generated_by": "canary-local/tools/gen_hub_seed.py — do not edit by hand",
        "$doc": (
            "The securaCV hub's first-boot provisioning plan: the ordered steps that turn stock "
            "Home Assistant OS into a working hub. Generated so the on-device installer, the docs, "
            "and the flasher UI can't disagree about the sequence. Every step states why it exists "
            "and what it buys, because a user watching software configure their home should be "
            "able to answer 'what is this doing, and why?' at every line."
        ),
        "schema_version": 1,
        "order_matters": (
            "Repositories before installs (nothing installs from an unregistered repo); broker "
            "before the things that publish to it; Frigate's config before Frigate starts, or it "
            "crash-loops with nothing to do."
        ),
        "slug_note": (
            "Each add-on step carries both `addon` (the friendly slug, for display) and "
            "`supervisor_slug` (what the Supervisor REST API answers to). They differ for "
            "third-party add-ons: a repo add-on is addressed as `<repo_hash>_<slug>`, so securaCV's "
            "`privacy_witness_kernel` becomes `d0491a67_privacy_witness_kernel`. Install by the "
            "supervisor_slug or the API returns 'add-on not found'."
        ),
        "optional_features": {
            "pihole": {
                "what": (
                    "Pi-hole DNS on the hub: a local log of which device asked for which domain "
                    "(client IP + domain + time, never page contents) — the way to verify "
                    "nothing, including a Canary, is quietly talking out. Ad/tracker blocking is "
                    "the side effect. The log stays on the hub; retention is configurable and "
                    "query logging can be switched off without losing blocking."
                ),
                "enable": "sh provision.sh --with pihole   (or host_provision.sh --with pihole)",
                "recommended": True,
                "off_by_default_because": (
                    "A DNS server is a whole-network change someone should choose on purpose — "
                    "recommended, never imposed. The plan without it is complete."
                ),
            },
            "display": {
                "what": (
                    "A dashboard on a screen plugged into the hub: the HAOSKiosk add-on runs a "
                    "small browser on the hub itself, full-screen on the attached HDMI display, "
                    "with touch mapped to the panel. Made for a hub with a screen — like a 7\" "
                    "1024x600 IPS HDMI touchscreen — and harmless without one (the add-on just "
                    "won't start). Everything stays on the hub; nothing new talks out."
                ),
                "enable": "sh provision.sh --with display   (or host_provision.sh --with display)",
                "recommended": False,
                "off_by_default_because": (
                    "Most hubs are headless — a closet box needs no browser running on it, and "
                    "the add-on can't start without a screen anyway. Choosing it is what says "
                    "'this hub has eyes on it.' The plan without it is complete."
                ),
            },
        },
        "repositories": repositories,
        "steps": steps,
        "user_supplied": [
            {
                "id": "camera",
                "what": "Your camera's RTSP URL, and `enabled: true` on that camera.",
                "why": (
                    "Camera addresses and credentials are yours; we can't guess them, and we won't "
                    "scan your network to find out. This is the one step that is genuinely manual."
                ),
            }
        ],
        "meta": {
            "design": "docs/design/raspberry_pi_hub_flashing.md",
            "guide": "docs/full_stack_setup.md",
            "sources": [
                "canary-local/devices/hub_image.json",
                "homeassistant/frigate/config.yaml",
            ],
        },
    }

    OUT_JSON.write_text(json.dumps(out, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    print(
        f"wrote {OUT_JSON.relative_to(REPO)} — {len(steps)} steps, "
        f"{len(repositories)} repositories, frigate slug {frigate['slug']}"
    )


if __name__ == "__main__":
    main()
