"""The app's store presentation and supervision, held to the spec.

config.yaml (the keys the Supervisor validates and the decisions behind them),
translations/en.yaml (every option named), icon/logo (the sizes the store
shows), apparmor.txt (one profile named after the slug, children for the
processes, writes limited to the paths DOCS.md lists), the Dockerfile (an
official pinned base selected by TARGETARCH, no build.yaml, the pruned
integration carry), the s6 service tree (run + finish per service, the
configured gate), run.sh (the C6 payload and the C1 announce flags), and the
carried integration's version equal to the app's.

PyYAML is a TEST dependency only (the image has none); the pwk-wizard-tests
job installs it, and the tests are skipped locally without it.
"""

import json
import os
import re
import struct
from pathlib import Path

import pytest

yaml = pytest.importorskip("yaml") if not os.environ.get("CI") else __import__("yaml")

ADDON = Path(__file__).resolve().parent.parent
REPO = ADDON.parent
CONFIG = yaml.safe_load((ADDON / "config.yaml").read_text())
TRANSLATIONS = yaml.safe_load((ADDON / "translations" / "en.yaml").read_text())
MANIFEST = json.loads((REPO / "custom_components" / "securacv" / "manifest.json").read_text())
DOCKERFILE = (ADDON / "Dockerfile").read_text()
APPARMOR = (ADDON / "apparmor.txt").read_text()
RUN_SH = (ADDON / "run.sh").read_text()
SERVICES_DIR = ADDON / "rootfs" / "etc" / "services.d"
SERVICES = ("panel", "witness_api", "witnessd", "frigate_bridge", "event_mqtt_bridge", "discovery")


# ---------------------------------------------------------------------------
# config.yaml
# ---------------------------------------------------------------------------


def test_store_identity_and_floor():
    assert CONFIG["name"] == "SecuraCV"
    assert CONFIG["slug"] == "privacy_witness_kernel"  # upgrade continuity
    assert CONFIG["homeassistant"] == "2026.5.0"
    assert CONFIG["arch"] == ["aarch64", "amd64"]
    assert CONFIG["init"] is False  # s6-overlay is PID 1
    assert CONFIG["image"] == "ghcr.io/kmay89/{arch}-addon-privacy_witness_kernel"


def test_no_hassio_role_and_no_ssl_map():
    assert "hassio_role" not in CONFIG
    assert CONFIG["homeassistant_api"] is True
    assert CONFIG["hassio_api"] is True
    for entry in CONFIG["map"]:
        assert isinstance(entry, dict), "legacy string map entries are retired"
        assert entry["type"] != "ssl"


def test_map_is_typed_with_the_paths_every_contract_relies_on():
    by_type = {m["type"]: m for m in CONFIG["map"]}
    assert set(by_type) == {"homeassistant_config", "all_app_configs", "share"}
    assert by_type["homeassistant_config"] == {"type": "homeassistant_config", "read_only": False,
                                               "path": "/config"}
    assert by_type["all_app_configs"] == {"type": "all_app_configs", "read_only": False,
                                          "path": "/addon_configs"}
    assert by_type["share"]["read_only"] is False


def test_watchdog_matches_the_supervisor_shape_and_probes_the_panel():
    watchdog = CONFIG["watchdog"]
    # apps/validate.py's RE_WATCHDOG (review-home.md)
    assert re.fullmatch(r"(?:https?|\[PROTO:\w+\]|tcp):\/\/\[HOST\]:(\[PORT:\d+\]|\d+).*", watchdog)
    assert watchdog == f"http://[HOST]:[PORT:{CONFIG['ingress_port']}]/api/health"


def test_options_and_schema_carry_the_new_switches():
    opts = CONFIG["options"]
    schema = CONFIG["schema"]
    assert opts["install_integration"] is True
    assert schema["install_integration"] == "bool?"
    assert opts["verify_interval_hours"] == 1
    assert schema["verify_interval_hours"] == "int(0,168)?"
    assert opts["retention_days"] == 7
    assert opts["frigate"]["enable_reviews"] is False
    assert schema["frigate"]["cameras"] == "list(str)?"
    assert set(opts) == set(schema), "every option has a schema entry and vice versa"


def test_discovery_services_and_ingress_unchanged():
    assert CONFIG["discovery"] == ["securacv"]
    assert CONFIG["services"] == ["mqtt:want"]
    assert CONFIG["ingress"] is True and CONFIG["ingress_port"] == 8788
    assert CONFIG["ports"] == {"8799/tcp": None}
    assert CONFIG["apparmor"] is True


def test_carried_integration_version_equals_the_app_version():
    assert MANIFEST["version"] == CONFIG["version"]


# ---------------------------------------------------------------------------
# translations/en.yaml
# ---------------------------------------------------------------------------


def test_translations_name_every_option_and_the_port():
    configuration = TRANSLATIONS["configuration"]
    assert set(configuration) == set(CONFIG["options"])
    for key, entry in configuration.items():
        assert entry.get("name"), key
        assert entry.get("description"), key
    assert set(TRANSLATIONS["network"]) == set(CONFIG["ports"])


def test_translations_explain_install_integration_honestly():
    text = TRANSLATIONS["configuration"]["install_integration"]["description"]
    assert "never" in text and "restart" in text.lower()
    assert "HACS" in text


# ---------------------------------------------------------------------------
# icon.png / logo.png
# ---------------------------------------------------------------------------


def _png_size(path: Path):
    head = path.read_bytes()[:24]
    assert head[:8] == b"\x89PNG\r\n\x1a\n", path
    width, height = struct.unpack(">II", head[16:24])
    return width, height


def test_store_art_exists_at_the_sizes_the_store_shows():
    assert _png_size(ADDON / "icon.png") == (128, 128)
    width, height = _png_size(ADDON / "logo.png")
    assert width == 250 and 60 <= height <= 100


# ---------------------------------------------------------------------------
# apparmor.txt
# ---------------------------------------------------------------------------


def test_apparmor_profile_is_named_after_the_slug_with_child_profiles():
    top = re.findall(r"^profile (\S+)", APPARMOR, re.M)
    assert top == [CONFIG["slug"]], "exactly one column-0 profile, named after the slug"
    children = set(re.findall(r"^[ \t]+profile (\S+)", APPARMOR, re.M))
    assert children == {"witness_api", "witnessd", "frigate_bridge", "event_mqtt_bridge", "panel"}
    for child in ("witness_api", "witnessd", "frigate_bridge", "event_mqtt_bridge"):
        assert f"/usr/local/bin/{child} cx -> {child}," in APPARMOR
    assert "/usr/bin/python3* cx -> panel," in APPARMOR
    assert "#include <tunables/global>" in APPARMOR
    assert "signal (receive) peer=*_privacy_witness_kernel," in APPARMOR


def test_apparmor_never_grants_blanket_file_access_and_limits_writes():
    assert re.search(r"^\s*file,\s*$", APPARMOR, re.M) is None
    writes = re.findall(r"^\s*(/\S+)\s+[a-z]*w[a-z]*,\s*$", APPARMOR, re.M)
    allowed_prefixes = (
        "/data", "/run", "/tmp", "/dev/", "/share",
        "/root/.cache/",  # GStreamer's plugin registry cache (witnessd)
        "/etc/services.d/", "/etc/cont-init.d/", "/etc/cont-finish.d/",
        "/config/witness.db", "/config/witness_config.json",
        "/config/witness.ed25519.seed",
        "/config/api_token", "/config/.api_token.",
        "/config/paired_tokens.json", "/config/.paired_tokens.json.",
        "/config/pairing_code", "/config/.pairing_code.",
        "/config/fleet_peers.json",
        "/config/.securacv/",
        "/config/blueprints/",
        "/config/custom_components/",
        "/config/frigate.yml",
        "/addon_configs/ccab4aaf_frigate/",
    )
    for path in writes:
        assert path.startswith(allowed_prefixes), f"unexpected write path {path}"
    # /config itself is read-only; the panel never writes the whole tree.
    assert re.search(r"^\s*/config/\*\* rw", APPARMOR, re.M) is None
    assert re.search(r"^\s*/config/ rw", APPARMOR, re.M) is None
    assert re.search(r"^\s*/addon_configs/\*\* rw", APPARMOR, re.M) is None


def _profile_block(name):
    start = APPARMOR.index(f"profile {name} ")
    end = APPARMOR.index("\n  }\n", start)
    return APPARMOR[start:end]


def _file_rules(block):
    """{path: permission letters} for every path rule in a profile block."""
    return {
        path: perms
        for path, perms in re.findall(r"^\s*(/\S+)\s+([a-z]+),\s*$", block, re.M)
    }


def test_apparmor_only_the_panel_writes_the_integration_tree():
    assert "/config/custom_components/securacv/** rw," in _profile_block("panel")
    for name in ("witness_api", "witnessd", "frigate_bridge", "event_mqtt_bridge"):
        assert "custom_components" not in _profile_block(name), name


# Every file a confined process opens under /config, by the literal the Rust
# source (or the setup) spells, with the least permission that keeps it
# running — a standalone install crash-looped once because witnessd's own
# seed file (crypto::resolve_device_seed_persisting_env creates
# `<db>.ed25519.seed` at every first start) had no rule. A new file in the
# sources is a new row here AND a new rule in apparmor.txt.
#
#   witness.db(-wal/-shm)        storage.rs (SQLite WAL)            witnessd, witness_api, frigate_bridge
#   witness.db.health.json(.tmp) storage_health.rs store_state      witnessd, witness_api
#   witness.ed25519.seed         crypto/mod.rs device_key_path_for_db  witnessd (creates), witness_api + frigate_bridge (read)
#   api_token + .api_token.*.tmp api/mod.rs write_restricted         witnessd, witness_api (write); event_mqtt_bridge, panel (read)
#   paired_tokens.json (+tmp)    api/pairing.rs PAIRED_TOKEN_FILE_NAME  witnessd, witness_api
#   pairing_code (+tmp)          api/pairing.rs PAIRING_CODE_FILE_NAME  witnessd, witness_api
#   viewer_tokens.json           api/mod.rs VIEWER_TOKEN_FILE_NAME   witnessd, witness_api (read)
#   fleet_peers.json (+ .tmp)    fleet_peers.rs write_atomic         event_mqtt_bridge (write); witnessd, witness_api (read)
#   witness_config.json          run.sh CONFIG_FILE                  every kernel process (read)
REQUIRED_FILE_RULES = {
    "witnessd": {
        "/config/witness_config.json": "r",
        "/config/witness.db": "rwk",
        "/config/witness.db-*": "rwk",
        "/config/witness.db.health.json": "rw",
        "/config/witness.db.health.json.tmp": "rw",
        "/config/witness.ed25519.seed": "rw",
        "/config/api_token": "rw",
        "/config/.api_token.*.tmp": "rw",
        "/config/viewer_tokens.json": "r",
        "/config/paired_tokens.json": "rw",
        "/config/.paired_tokens.json.*.tmp": "rw",
        "/config/pairing_code": "rw",
        "/config/.pairing_code.*.tmp": "rw",
        "/config/fleet_peers.json": "r",
        "/share/**": "rwk",
    },
    "witness_api": {
        "/config/witness_config.json": "r",
        "/config/witness.db": "rwk",
        "/config/witness.db-*": "rwk",
        "/config/witness.db.health.json": "rw",
        "/config/witness.db.health.json.tmp": "rw",
        "/config/witness.ed25519.seed": "r",
        "/config/api_token": "rw",
        "/config/.api_token.*.tmp": "rw",
        "/config/viewer_tokens.json": "r",
        "/config/paired_tokens.json": "rw",
        "/config/.paired_tokens.json.*.tmp": "rw",
        "/config/pairing_code": "rw",
        "/config/.pairing_code.*.tmp": "rw",
        "/config/fleet_peers.json": "r",
        "/share/**": "rwk",
    },
    "frigate_bridge": {
        "/config/witness_config.json": "r",
        "/config/witness.db": "rwk",
        "/config/witness.db-*": "rwk",
        "/config/witness.ed25519.seed": "r",
    },
    "event_mqtt_bridge": {
        "/config/api_token": "r",
        "/config/fleet_peers.json": "rw",
        "/config/fleet_peers.json.tmp": "rw",
    },
    "panel": {
        "/config/api_token": "r",
        "/config/.securacv/**": "rw",
        "/config/frigate.yml": "rw",
        "/config/blueprints/automation/securacv/**": "r",
        "/config/custom_components/securacv/**": "rw",
        "/addon_configs/ccab4aaf_frigate/config.yml": "rw",
        "/addon_configs/ccab4aaf_frigate/config.yml.new": "rw",
    },
}


@pytest.mark.parametrize("profile", sorted(REQUIRED_FILE_RULES))
def test_apparmor_names_every_file_each_process_opens(profile):
    rules = _file_rules(_profile_block(profile))
    for path, needed in REQUIRED_FILE_RULES[profile].items():
        assert path in rules, f"{profile}: no rule for {path} (the process opens it)"
        missing = set(needed) - set(rules[path])
        assert not missing, f"{profile}: {path} needs {needed!r}, has {rules[path]!r}"


def test_apparmor_the_seed_file_is_created_by_witnessd_alone():
    """witnessd persists the environment seed on a first standalone start;
    the other kernel processes only read it (resolve_device_seed never
    writes a seed it was given)."""
    assert "w" in _file_rules(_profile_block("witnessd"))["/config/witness.ed25519.seed"]
    for profile in ("witness_api", "frigate_bridge"):
        assert _file_rules(_profile_block(profile))["/config/witness.ed25519.seed"] == "r", profile
    for profile in ("event_mqtt_bridge", "panel"):
        assert "/config/witness.ed25519.seed" not in _file_rules(_profile_block(profile)), profile


def test_apparmor_rules_name_the_literals_the_sources_spell():
    """The file names are the Rust constants', so a renamed file is caught."""
    pairing = (REPO / "src" / "api" / "pairing.rs").read_text()
    assert 'PAIRING_CODE_FILE_NAME: &str = "pairing_code"' in pairing
    assert 'PAIRED_TOKEN_FILE_NAME: &str = "paired_tokens.json"' in pairing
    assert 'VIEWER_TOKEN_FILE_NAME: &str = "viewer_tokens.json"' in (REPO / "src" / "api" / "mod.rs").read_text()
    assert 'with_extension("ed25519.seed")' in (REPO / "src" / "crypto" / "mod.rs").read_text()
    assert '"{}.health.json"' in (REPO / "src" / "storage_health.rs").read_text()
    assert 'with_extension("json.tmp")' in (REPO / "src" / "storage_health.rs").read_text()
    assert 'tmp.push(".tmp")' in (REPO / "src" / "fleet_peers.rs").read_text()
    assert 'format!(".{file_name}.{:016x}.tmp"' in (REPO / "src" / "break_glass" / "backend.rs").read_text()
    assert 'DB_PATH="/config/witness.db"' in RUN_SH and 'TOKEN_FILE="/config/api_token"' in RUN_SH
    assert 'FLEET_PEERS_FILE="/config/fleet_peers.json"' in RUN_SH


# ---------------------------------------------------------------------------
# Dockerfile
# ---------------------------------------------------------------------------


def test_dockerfile_uses_the_official_pinned_base_selected_by_targetarch():
    assert "ARG TARGETARCH" in DOCKERFILE
    assert "FROM ghcr.io/home-assistant/amd64-base:3.18 AS runtime-amd64" in DOCKERFILE
    assert "FROM ghcr.io/home-assistant/aarch64-base:3.18 AS runtime-arm64" in DOCKERFILE
    assert "FROM runtime-${TARGETARCH}" in DOCKERFILE
    assert "hassio-addons/base" not in DOCKERFILE
    assert "BUILD_FROM" not in DOCKERFILE
    assert not (ADDON / "build.yaml").exists()
    # s6-overlay is the entrypoint: no CMD that would replace the service tree.
    assert re.search(r"^CMD\b", DOCKERFILE, re.M) is None
    assert "COPY privacy_witness_kernel/rootfs/ /" in DOCKERFILE
    assert "COPY privacy_witness_kernel/run.sh /etc/cont-init.d/" in DOCKERFILE


def test_dockerfile_carries_a_pruned_integration_and_the_blueprints():
    assert "COPY custom_components/securacv /carry/integration/securacv" in DOCKERFILE
    assert "COPY docs/blueprints/ /carry/blueprints/" in DOCKERFILE
    assert "rm -rf /carry/integration/securacv/tests" in DOCKERFILE
    assert "rm -f /carry/integration/securacv/www/*.test.js" in DOCKERFILE
    assert "COPY --from=carry /carry/integration /usr/share/securacv/integration" in DOCKERFILE
    assert "COPY --from=carry /carry/blueprints /usr/share/securacv/blueprints" in DOCKERFILE
    # The build refuses a carried version that differs from the app's.
    assert 'carried integration version $carried != app version $app' in DOCKERFILE
    assert "COPY privacy_witness_kernel/integration_carry.py /usr/local/bin/" in DOCKERFILE


# ---------------------------------------------------------------------------
# The s6 service tree
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("service", SERVICES)
def test_every_service_has_an_executable_run_and_finish(service):
    run = SERVICES_DIR / service / "run"
    finish = SERVICES_DIR / service / "finish"
    for script in (run, finish):
        assert script.is_file(), script
        assert os.access(script, os.X_OK), f"{script} is not executable"
    assert run.read_text().startswith("#!/usr/bin/with-contenv bashio\n")
    assert finish.read_text().startswith("#!/usr/bin/env bashio\n")
    # The official finish shape: halt the tree on a crash, not on a clean exit.
    text = finish.read_text()
    assert '-ne 0 ]] && [[ "${APP_EXIT_CODE}" -ne 256' in text
    assert "exec /run/s6/basedir/bin/halt" in text


def test_no_service_dir_is_undeclared():
    assert sorted(p.name for p in SERVICES_DIR.iterdir()) == sorted(SERVICES)


@pytest.mark.parametrize("service", [s for s in SERVICES if s != "panel"])
def test_kernel_services_gate_on_the_configured_marker(service):
    text = (SERVICES_DIR / service / "run").read_text()
    assert "/run/securacv/configured" in text
    assert "s6-svc -O ." in text
    assert 'source "$SETUP_FILE"' in text


def test_panel_always_runs_the_wizard_server():
    text = (SERVICES_DIR / "panel" / "run").read_text()
    assert "exec /usr/bin/python3 /usr/local/bin/serve_wizard.py" in text
    assert "/run/securacv/configured" not in text


def test_mode_gates_match_the_processes():
    assert '[ "$MODE" != "frigate" ]' in (SERVICES_DIR / "witness_api" / "run").read_text()
    assert '[ "$MODE" = "frigate" ]' in (SERVICES_DIR / "witnessd" / "run").read_text()
    assert '[ "$MODE" != "frigate" ]' in (SERVICES_DIR / "frigate_bridge" / "run").read_text()
    assert '"$MQTT_PUBLISH_ENABLED" != "true"' in (SERVICES_DIR / "event_mqtt_bridge" / "run").read_text()


def test_passwords_travel_in_the_environment_of_one_child_only():
    frigate = (SERVICES_DIR / "frigate_bridge" / "run").read_text()
    publisher = (SERVICES_DIR / "event_mqtt_bridge" / "run").read_text()
    assert 'exec env MQTT_PASSWORD="$FRIGATE_MQTT_PASSWORD" "${FRIGATE_CMD_ARRAY[@]}"' in frigate
    assert 'exec env MQTT_PASSWORD="$PUBLISHER_MQTT_PASSWORD" "${PUBLISHER_CMD_ARRAY[@]}"' in publisher
    assert "export MQTT_PASSWORD" not in RUN_SH


def test_discovery_service_posts_the_c6_payload_once_the_kernel_answers():
    text = (SERVICES_DIR / "discovery" / "run").read_text()
    assert 'compose_discovery_payload "$ADDON_HOST" 8799 "$TOKEN_FILE" "$PUBLISH_PREFIX" "$DEVICE_ID"' in text
    assert "wait_for_kernel_health" in text
    assert "http://supervisor/discovery" in text
    assert "s6-svc -O ." in text


# ---------------------------------------------------------------------------
# run.sh: the setup step
# ---------------------------------------------------------------------------


def test_run_sh_computes_the_device_id_before_anything_announces_it():
    assert 'DEVICE_ID=$(ha_device_id "$DEVICE_KEY_SEED")' in RUN_SH
    assert RUN_SH.index('DEVICE_ID=$(ha_device_id') < RUN_SH.index("PUBLISHER_CMD_ARRAY=(")


def test_run_sh_passes_the_c1_announce_flags_to_the_publisher():
    for flag in ("--announce-api-bind all", "--announce-pairing token_file",
                 '--announce-api-host "$ADDON_HOST"', '--ha-device-id "$DEVICE_ID"'):
        assert flag in RUN_SH, flag


def test_run_sh_installs_blueprints_and_updates_only_the_apps_own_integration():
    assert 'install_blueprints "$BLUEPRINT_SRC" "$BLUEPRINT_DEST"' in RUN_SH
    assert 'BLUEPRINT_DEST="/config/blueprints/automation/securacv"' in RUN_SH
    assert "integration_carry.py on-start --notify" in RUN_SH
    assert "integration_carry.py install" not in RUN_SH  # the first install is a click
    assert "homeassistant/restart" not in RUN_SH
    assert "/core/restart" not in RUN_SH


def test_run_sh_leaves_the_marker_and_the_setup_file_for_the_services():
    assert 'touch "$CONFIGURED_MARKER"' in RUN_SH
    assert 'CONFIGURED_MARKER="$RUN_DIR/configured"' in RUN_SH
    assert 'RUN_DIR="/run/securacv"' in RUN_SH
    assert "declare -p PUBLISHER_CMD_ARRAY PUBLISHER_MQTT_PASSWORD" in RUN_SH
    assert "declare -p FRIGATE_CMD_ARRAY FRIGATE_MQTT_PASSWORD" in RUN_SH
    assert 'chmod 600 "$SETUP_FILE"' in RUN_SH
    # No process is started from the setup step any more.
    assert re.search(r"^\s*exec /usr/local/bin/", RUN_SH, re.M) is None
    assert re.search(r"&\s*$", "\n".join(
        line for line in RUN_SH.splitlines() if not line.lstrip().startswith("#")), re.M) is None


def test_verify_interval_default_is_hourly_everywhere():
    assert "local VERIFY_INTERVAL_HOURS=1" in RUN_SH
    assert CONFIG["options"]["verify_interval_hours"] == 1


# ---------------------------------------------------------------------------
# Store docs
# ---------------------------------------------------------------------------


def test_docs_cover_the_journeys_the_carry_disclosure_revocation_and_migration():
    # Markdown wraps sentences; compare on collapsed whitespace.
    docs = re.sub(r"\s+", " ", (ADDON / "DOCS.md").read_text())
    for needle in (
        "Home Assistant OS", "HACS first", "Docker sidecar",
        "Nothing is copied until you click", "never restarts Home Assistant by itself",
        "HACS or you installed is never touched", "Never a downgrade",
        "Revoking and uninstalling", "sensor.pwk_last_event", "sensor.securacv_last_event",
        "binary_sensor.securacv_chain_valid", "event.securacv_<zone>",
        "Settings → Dashboards → Add dashboard → SecuraCV",
    ):
        assert needle in docs, needle
    assert "appears automatically via MQTT discovery" not in docs


def test_changelog_has_an_entry_for_this_version():
    changelog = (ADDON / "CHANGELOG.md").read_text()
    assert f"## {CONFIG['version']}" in changelog
