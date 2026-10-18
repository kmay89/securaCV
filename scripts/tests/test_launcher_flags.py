"""Every flag a launcher passes to a kernel binary is one the binary accepts.

The add-on's s6 services exec `PUBLISHER_CMD_ARRAY` / `FRIGATE_CMD_ARRAY`
(rendered by privacy_witness_kernel/run.sh) and the sidecar's entrypoint
starts `event_mqtt_bridge "${pub_args[@]}"` / `frigate_bridge
"${bridge_args[@]}"`. A flag clap does not know is a startup error, the
service exits non-zero, and — under s6 with a finish script, or as the
sidecar's foreground daemon — the whole container halts. That already
happened once in review (the add-on passed `--announce-*` flags to a bridge
that did not have them yet), so this test reads the clap tables straight out
of the two binaries' sources and the arrays straight out of the two scripts,
and holds one to the other. It also pins the launcher posture contracts C1
and C5 name (announce bind/pairing/host, the state prefix and device id
handed to frigate_bridge) and the one-hour verify cadence both launchers
share (D7: 3600 s).
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

BINARIES = {
    "event_mqtt_bridge": ROOT / "src/bin/event_mqtt_bridge.rs",
    "frigate_bridge": ROOT / "src/bin/frigate_bridge.rs",
}

# binary -> [(script, array name)]
LAUNCHERS = {
    "event_mqtt_bridge": [
        (ROOT / "privacy_witness_kernel/run.sh", "PUBLISHER_CMD_ARRAY"),
        (ROOT / "docker/sidecar/entrypoint.sh", "pub_args"),
    ],
    "frigate_bridge": [
        (ROOT / "privacy_witness_kernel/run.sh", "FRIGATE_CMD_ARRAY"),
        (ROOT / "docker/sidecar/entrypoint.sh", "bridge_args"),
    ],
}

# `#[arg(...)]` then (doc comments, visibility) then the field name. The
# attribute may span lines (`default_value_t = 3600` on its own line).
ARG_RE = re.compile(
    r"#\[arg\((?P<attrs>.*?)\)\]\s*(?:///[^\n]*\n\s*)*(?:pub(?:\([a-z]+\))?\s+)?"
    r"(?P<field>[a-z_][a-z0-9_]*)\s*:",
    re.S,
)
LONG_OVERRIDE_RE = re.compile(r"\blong\s*=\s*\"([a-z][a-z0-9-]*)\"")
FLAG_RE = re.compile(r"(?<![\w-])--[a-z][a-z0-9-]*")


def clap_long_flags(source: str) -> set[str]:
    """The `--long` flags a clap derive struct accepts, with their dashes."""
    flags: set[str] = set()
    for match in ARG_RE.finditer(source):
        attrs = match.group("attrs")
        if not re.search(r"\blong\b", attrs):
            continue
        override = LONG_OVERRIDE_RE.search(attrs)
        name = override.group(1) if override else match.group("field").replace("_", "-")
        flags.add(f"--{name}")
    return flags


def _code_lines(text: str) -> str:
    return "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))


def array_flags(script: Path, name: str) -> list[str]:
    """Every `--flag` token assigned into the shell array `name`.

    Both forms: the block `NAME=(` ... `)` (also `local NAME=(`) and the
    one-line appends `NAME+=(...)`. Comment lines inside a block are dropped
    first, so prose about a flag is never read as the flag.
    """
    text = _code_lines(script.read_text(encoding="utf-8"))
    flags: list[str] = []
    for block in re.finditer(rf"(?m)^\s*(?:local\s+)?{re.escape(name)}=\(\s*(.*?)^\s*\)\s*$", text, re.S):
        flags += FLAG_RE.findall(block.group(1))
    for append in re.finditer(rf"{re.escape(name)}\+=\((.*?)\)", text):
        flags += FLAG_RE.findall(append.group(1))
    return flags


def test_the_parser_sees_the_flags_everyone_knows():
    publisher = clap_long_flags(BINARIES["event_mqtt_bridge"].read_text(encoding="utf-8"))
    bridge = clap_long_flags(BINARIES["frigate_bridge"].read_text(encoding="utf-8"))
    for flag in ("--mqtt-broker-addr", "--mqtt-topic-prefix", "--ha-device-id", "--mqtt-username"):
        assert flag in publisher and flag in bridge, flag
    assert {"--daemon", "--announce-api-bind", "--announce-pairing", "--announce-api-host", "--no-announce"} <= publisher
    assert {"--frigate-topic-prefix", "--enable-reviews", "--cameras", "--retention-check-interval-secs"} <= bridge
    assert "--mqtt-password" in publisher and "--mqtt-password" in bridge, "env-only in the launchers, but a real flag"


def test_every_launcher_flag_is_accepted_by_its_binary():
    for binary, launchers in LAUNCHERS.items():
        accepted = clap_long_flags(BINARIES[binary].read_text(encoding="utf-8"))
        for script, array in launchers:
            passed = array_flags(script, array)
            assert passed, f"{script.name}: no flags found in {array} (the array moved?)"
            unknown = sorted(set(passed) - accepted)
            assert not unknown, (
                f"{script.relative_to(ROOT)} passes {unknown} to {binary}, which does not accept "
                f"them (clap refuses, the service exits, the container halts). Accepted: "
                f"{sorted(accepted)}"
            )


def test_the_launchers_never_put_the_broker_password_on_argv():
    for binary, launchers in LAUNCHERS.items():
        for script, array in launchers:
            assert "--mqtt-password" not in array_flags(script, array), (
                f"{script.name} {array}: the password reaches {binary} through MQTT_PASSWORD, "
                "never argv (readable from /proc/<pid>/cmdline)"
            )


def test_the_launcher_posture_matches_the_contracts():
    """C1 launchers (announce bind/pairing/host), C5 (the state prefix and
    device id frigate_bridge publishes frigate_link under) and D7 (3600 s)."""
    run_sh = _code_lines((ROOT / "privacy_witness_kernel/run.sh").read_text(encoding="utf-8"))
    entrypoint = _code_lines((ROOT / "docker/sidecar/entrypoint.sh").read_text(encoding="utf-8"))

    publisher_addon = " ".join(array_flags(ROOT / "privacy_witness_kernel/run.sh", "PUBLISHER_CMD_ARRAY"))
    assert "--announce-api-bind" in publisher_addon and "--announce-pairing" in publisher_addon
    assert "--announce-api-host" in publisher_addon
    assert re.search(r"--announce-api-bind all\b", run_sh)
    assert re.search(r"--announce-pairing token_file\b", run_sh)
    assert re.search(r'--announce-api-host "\$ADDON_HOST"', run_sh)

    publisher_sidecar = " ".join(array_flags(ROOT / "docker/sidecar/entrypoint.sh", "pub_args"))
    assert "--announce-api-bind" in publisher_sidecar and "--announce-pairing" in publisher_sidecar
    assert re.search(r'--announce-api-bind "\$API_BIND_MODE"', entrypoint)
    assert re.search(r"--announce-pairing required\b", entrypoint)
    assert re.search(r'--announce-api-host "\$SECURACV_ANNOUNCE_HOST"', entrypoint)
    assert 'SECURACV_ANNOUNCE_HOST:-' in entrypoint, "the host rides only when the operator named one"

    for script, array in LAUNCHERS["frigate_bridge"]:
        flags = array_flags(script, array)
        assert "--mqtt-topic-prefix" in flags and "--ha-device-id" in flags, (
            f"{script.name} {array}: frigate_link must sit under the publisher's prefix and id"
        )

    # D7 / (j): the add-on's hour and the sidecar's 3600 s are one cadence.
    assert re.search(r"VERIFY_INTERVAL_HOURS=1\b", run_sh)
    assert "--verify-interval-secs \"$((VERIFY_INTERVAL_HOURS * 3600))\"" in run_sh
    assert '--verify-interval-secs "${VERIFY_INTERVAL_SECS:-3600}"' in entrypoint
    config_yaml = (ROOT / "privacy_witness_kernel/config.yaml").read_text(encoding="utf-8")
    assert re.search(r"(?m)^\s*verify_interval_hours:\s*1\s*$", config_yaml)


def test_the_arrays_name_their_binaries():
    run_sh = (ROOT / "privacy_witness_kernel/run.sh").read_text(encoding="utf-8")
    assert re.search(r'PUBLISHER_CMD_ARRAY=\(\s*"/usr/local/bin/event_mqtt_bridge"', run_sh)
    assert re.search(r'FRIGATE_CMD_ARRAY=\(\s*"/usr/local/bin/frigate_bridge"', run_sh)
    entrypoint = (ROOT / "docker/sidecar/entrypoint.sh").read_text(encoding="utf-8")
    assert 'event_mqtt_bridge "${pub_args[@]}"' in entrypoint
    assert 'frigate_bridge "${bridge_args[@]}"' in entrypoint
    for service in ("event_mqtt_bridge", "frigate_bridge"):
        run = (ROOT / f"privacy_witness_kernel/rootfs/etc/services.d/{service}/run").read_text(encoding="utf-8")
        array = "PUBLISHER_CMD_ARRAY" if service == "event_mqtt_bridge" else "FRIGATE_CMD_ARRAY"
        assert f'"${{{array}[@]}}"' in run, f"{service}/run must exec the rendered array"
