#!/usr/bin/env python3
"""scripts/ha_onboard.py — finish SecuraCV onboarding through the core API.

The Supervisor can install add-ons, but everything a user would otherwise
click inside Home Assistant itself — waiting for core to come back after a
restart, creating the SecuraCV config entry, wiring the daily-digest
automation to a real phone — goes through the core API. This helper does that
work so scripts/install.sh stays a narrator, not an API client.

Two subcommands:

  wait-core [--timeout N]
      Poll GET {base}/core/api/ until it answers HTTP 200 or the timeout
      passes. Exit 0 when core answers, 1 when it does not.

  finish [--config-dir /config]
      Idempotently finish onboarding, degrading per sub-step instead of
      raising: (1) confirm the MQTT config entry exists (print the exact
      clicks if not); (2) create the SecuraCV config entry by driving its
      config flow with {"setup_mode": "auto"}; (3) create the daily-digest
      automation from the installed blueprint, targeting the first
      mobile_app notify service (alphabetically); (4) summarize what was
      done and what was skipped. Exit nonzero only if NOTHING succeeded.

Runtime: Python 3 standard library only — nothing to pip-install on the box.
Base URL comes from $SUPERVISOR_URL (default http://supervisor); the bearer
token from $SUPERVISOR_TOKEN. Every line is prefixed "[SecuraCV]" so the
installer's transcript reads as one voice.
"""
from __future__ import annotations

import argparse
import json
import os
import time
import urllib.error
import urllib.request

PREFIX = "[SecuraCV]"
BASE_URL = os.environ.get("SUPERVISOR_URL", "http://supervisor").rstrip("/")
TOKEN = os.environ.get("SUPERVISOR_TOKEN", "")

AUTOMATION_ID = "securacv_daily_digest"
AUTOMATION_PATH = f"/core/api/config/automation/config/{AUTOMATION_ID}"
BLUEPRINT_RELPATH = "blueprints/automation/securacv/securacv_daily_digest.yaml"

# Flow-abort reasons that mean "already done", not "failed".
ABORT_OK = ("already_configured", "single_instance_allowed")


def say(msg: str) -> None:
    print(f"{PREFIX} {msg}", flush=True)


class CoreUnreachable(RuntimeError):
    """The core API could not be reached at all (connection-level failure)."""


def request(method: str, path: str, body: dict | None = None, timeout: float = 15.0):
    """One core-API call. Returns (http_status, parsed_json_or_None).

    HTTP error statuses are returned, not raised — a 404 on the automation
    endpoint is an answer ("not there yet"), not a failure. Only a
    connection-level failure raises, as CoreUnreachable.
    """
    url = f"{BASE_URL}{path}"
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", f"Bearer {TOKEN}")
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read().decode("utf-8", errors="replace")
            status = resp.status
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", errors="replace")
        status = e.code
    except urllib.error.URLError as e:
        raise CoreUnreachable(f"{method} {path}: {e.reason}") from None
    if not raw:
        return status, None
    try:
        return status, json.loads(raw)
    except json.JSONDecodeError:
        return status, None


# ---------------------------------------------------------------------------
# wait-core
# ---------------------------------------------------------------------------


def cmd_wait_core(args: argparse.Namespace) -> int:
    if not TOKEN:
        say("SUPERVISOR_TOKEN is not set — cannot ask the core API anything from here.")
        return 1
    timeout = args.timeout
    # Short timeouts (tests, quick re-checks) poll fast; long ones politely.
    interval = 2.0 if timeout > 30 else 0.5
    say(f"Waiting for Home Assistant core to answer at {BASE_URL}/core/api/ (up to {timeout}s)…")
    deadline = time.monotonic() + timeout
    dots = 0
    while True:
        try:
            status, _ = request("GET", "/core/api/", timeout=min(10.0, max(1.0, timeout)))
        except CoreUnreachable:
            status = 0
        if status == 200:
            if dots:
                print(flush=True)
            say("Core is answering — onboarding can continue.")
            return 0
        if time.monotonic() >= deadline:
            if dots:
                print(flush=True)
            say(f"Core did not answer within {timeout}s (last status: {status or 'unreachable'}).")
            say("It may simply still be starting — re-run this, or finish onboarding in the UI.")
            return 1
        print(".", end="", flush=True)
        dots += 1
        time.sleep(interval)


# ---------------------------------------------------------------------------
# finish
# ---------------------------------------------------------------------------


def get_entry_domains() -> set[str] | None:
    """Domains that already have a config entry, or None if the list is unreadable."""
    try:
        status, data = request("GET", "/core/api/config/config_entries/entry")
    except CoreUnreachable as e:
        say(f"Could not list config entries ({e}).")
        return None
    if status != 200 or not isinstance(data, list):
        say(f"Could not list config entries (HTTP {status}).")
        return None
    return {e.get("domain", "") for e in data if isinstance(e, dict)}


def flow_finished(resp) -> bool:
    """True when a config-flow response means "the entry exists now"."""
    if not isinstance(resp, dict):
        return False
    if resp.get("type") == "create_entry":
        return True
    return resp.get("type") == "abort" and resp.get("reason", "") in ABORT_OK


def check_mqtt(domains: set[str] | None) -> str:
    """Sub-step 1: is Home Assistant connected to the broker?"""
    if domains is None:
        return "failed"
    if "mqtt" in domains:
        say("MQTT integration: connected — Home Assistant hears the broker.")
        return "ok"
    say("MQTT integration: NOT set up yet. Without it, no broker traffic becomes entities.")
    say("To connect it, click: Settings -> Devices & Services -> Add integration -> MQTT.")
    say("Continuing — the rest of onboarding does not depend on it.")
    return "manual"


def ensure_securacv_entry(domains: set[str] | None) -> str:
    """Sub-step 2: create the SecuraCV config entry by driving its flow."""
    if domains is not None and "securacv" in domains:
        say("SecuraCV integration: entry already exists — skipping.")
        return "skipped"
    try:
        status, flow = request(
            "POST",
            "/core/api/config/config_entries/flow",
            {"handler": "securacv", "show_advanced_options": False},
        )
    except CoreUnreachable as e:
        say(f"SecuraCV integration: could not start its setup flow ({e}).")
        return "failed"
    if flow_finished(flow):
        say("SecuraCV integration: entry created.")
        return "ok"
    if not isinstance(flow, dict) or status not in (200, 201):
        say(f"SecuraCV integration: Home Assistant refused the setup flow (HTTP {status}).")
        say("If Home Assistant just restarted, the integration may not be loaded yet — re-run this, or add it by hand: Settings -> Devices & Services -> Add integration -> SecuraCV.")
        return "failed"
    if flow.get("type") == "form" and flow.get("step_id") == "user":
        flow_id = flow.get("flow_id", "")
        try:
            _status2, result = request(
                "POST",
                f"/core/api/config/config_entries/flow/{flow_id}",
                {"setup_mode": "auto"},
            )
        except CoreUnreachable as e:
            say(f"SecuraCV integration: setup flow broke off ({e}).")
            return "failed"
        if flow_finished(result):
            say("SecuraCV integration: entry created (automatic mode — it detected what is installed).")
            return "ok"
        if isinstance(result, dict) and result.get("type") == "form":
            step = result.get("step_id", "?")
            say(f"SecuraCV integration: its flow asked one more question (step '{step}').")
            say(f"One manual step remains: Settings -> Devices & Services -> Add integration -> SecuraCV, and answer the '{step}' step.")
            return "manual"
        say("SecuraCV integration: unexpected flow answer — finish it in Settings -> Devices & Services.")
        return "failed"
    step = flow.get("step_id", flow.get("type", "?"))
    say(f"SecuraCV integration: its flow opened at step '{step}', which this helper does not answer.")
    say("One manual step remains: Settings -> Devices & Services -> Add integration -> SecuraCV.")
    return "manual"


def pick_notify_service() -> str | None:
    """The first mobile_app notify service, alphabetically, or None."""
    try:
        status, data = request("GET", "/core/api/services")
    except CoreUnreachable as e:
        say(f"Could not list services ({e}).")
        return None
    if status != 200 or not isinstance(data, list):
        say(f"Could not list services (HTTP {status}).")
        return None
    names: list[str] = []
    for domain in data:
        if not isinstance(domain, dict) or domain.get("domain") != "notify":
            continue
        services = domain.get("services")
        if isinstance(services, dict):
            names.extend(services.keys())
        elif isinstance(services, list):
            names.extend(s for s in services if isinstance(s, str))
    mobile = sorted(n for n in names if n.startswith("mobile_app"))
    return mobile[0] if mobile else None


def ensure_digest_automation(config_dir: str) -> str:
    """Sub-step 3: the daily-digest automation, from the installed blueprint."""
    blueprint = os.path.join(config_dir, BLUEPRINT_RELPATH)
    if not os.path.isfile(blueprint):
        say(f"Daily digest: blueprint not found at {blueprint} — skipping (the installer places it; re-run once it is there).")
        return "manual"

    service = pick_notify_service()
    if service is None:
        say("Daily digest: no mobile_app notification targets exist yet, so there is no phone to send it to — skipping.")
        say("Install the Home Assistant companion app on a phone, then re-run; the automation will be created then.")
        return "skipped"
    say(f"Daily digest: will notify notify.{service} (the first mobile_app target, alphabetically).")

    try:
        status, _ = request("GET", AUTOMATION_PATH)
    except CoreUnreachable as e:
        say(f"Daily digest: could not check for the automation ({e}).")
        return "failed"
    if status == 200:
        say("Daily digest: automation already exists — skipping (edit or delete it freely; it is yours).")
        return "skipped"
    if status != 404:
        say(f"Daily digest: unexpected answer checking for the automation (HTTP {status}) — skipping.")
        return "failed"

    body = {
        "alias": "SecuraCV daily digest",
        "description": "Created by the SecuraCV installer. Safe to edit or delete.",
        "use_blueprint": {
            "path": "securacv/securacv_daily_digest.yaml",
            "input": {"notify_service": f"notify.{service}"},
        },
    }
    try:
        status, _ = request("POST", AUTOMATION_PATH, body)
    except CoreUnreachable as e:
        say(f"Daily digest: could not create the automation ({e}).")
        return "failed"
    if 200 <= status < 300:
        say("Daily digest: automation created — a morning summary of witness activity, sent to your phone.")
        return "ok"
    say(f"Daily digest: Home Assistant refused the automation (HTTP {status}).")
    say("Create it by hand: Settings -> Automations -> Create -> from the 'SecuraCV Daily Digest' blueprint.")
    return "failed"


def cmd_finish(args: argparse.Namespace) -> int:
    if not TOKEN:
        say("SUPERVISOR_TOKEN is not set — cannot reach the core API from here.")
        say("Finish in the UI instead: Settings -> Devices & Services -> Add integration -> SecuraCV.")
        return 1

    say("Finishing onboarding through the Home Assistant API (each sub-step degrades on its own; nothing here raises).")
    results: dict[str, str] = {}

    domains = get_entry_domains()
    results["mqtt entry"] = check_mqtt(domains)
    results["securacv entry"] = ensure_securacv_entry(domains)
    results["daily digest"] = ensure_digest_automation(args.config_dir)

    pretty = {
        "ok": "done",
        "skipped": "already done / not applicable",
        "manual": "needs a click from you (see above)",
        "failed": "failed (see above)",
    }
    say("Summary: " + "; ".join(f"{name}: {pretty[state]}" for name, state in results.items()))

    # Success is gated on the one thing onboarding exists for: the SecuraCV
    # config entry. A skipped digest (no phone yet) or a manual MQTT click
    # must not let "finished" mask a missing entry.
    if results["securacv entry"] in ("ok", "skipped"):
        return 0
    say("The SecuraCV entry is not set up — the lines above say what to click; re-running is safe.")
    return 1


# ---------------------------------------------------------------------------
# entry point
# ---------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = ap.add_subparsers(dest="command", required=True)

    wait = sub.add_parser("wait-core", help="poll the core API until it answers")
    wait.add_argument("--timeout", type=int, default=300, help="seconds to wait (default 300)")
    wait.set_defaults(func=cmd_wait_core)

    finish = sub.add_parser("finish", help="idempotently finish onboarding")
    finish.add_argument(
        "--config-dir",
        default="/config",
        help="Home Assistant config dir, for the blueprint-presence check (default /config)",
    )
    finish.set_defaults(func=cmd_finish)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
