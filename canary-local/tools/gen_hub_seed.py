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


def die(msg: str) -> None:
    sys.exit(f"gen_hub_seed.py: {msg}")


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
    ]

    steps = [
        {
            "id": "add-repositories",
            "title": "Register the add-on repositories",
            "what": "Tell Home Assistant where the Frigate and securaCV add-ons come from.",
            "why": (
                "Home Assistant only installs add-ons from repositories it knows about. Both of "
                "ours are third-party, so nothing else in this plan can work until they're "
                "registered. The Supervisor API does this unattended; the `ha` command line "
                "cannot, which is why the by-hand path makes you click through the store UI."
            ),
            "for_what": "Makes the next two installs possible.",
            "repositories": [r["url"] for r in repositories],
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
            "start": True,
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
                "The Frigate add-on reads config.yml from its OWN add-on config directory — NOT "
                "/config/frigate.yml. Editing the wrong file is the classic 'I changed it and "
                "nothing happened'."
            ),
            "never_overwrite": True,
            "then_start": frigate["slug"],
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
            "what": f"Install and start the `{kernel['slug']}` add-on.",
            "why": (
                "This is the part that makes the system a witness rather than a camera system. It "
                "subscribes to Frigate's detections, strips identity, and writes each one into a "
                "signed, hash-chained log that can be verified later without trusting us. It "
                "auto-discovers the broker installed above, so there's no MQTT to type."
            ),
            "for_what": "Turns detections into tamper-evident, privacy-preserving claims.",
            "addon": kernel["slug"],
            "start": True,
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
