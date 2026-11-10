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

import importlib.util
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
    # A list of strings, in the Supervisor's list syntax (a YAML list holding
    # the element type). `list(str)?` is a select that takes only "str".
    assert schema["frigate"]["cameras"] == ["str?"]
    assert schema["frigate"]["labels"] == ["str?"]
    assert set(opts) == set(schema), "every option has a schema entry and vice versa"


# The Supervisor's option check, mirrored for the types this schema uses:
# supervisor/apps/options.py (AppOptions, home-assistant/supervisor at
# bde7fb7b), which write_options runs on the stored options before every
# start (the app does not start when it refuses them) and POST
# /addons/self/options runs on every save the wizard sends. A YAML list is a
# list of its one element type; a dict is checked key by key (unknown keys
# dropped, a missing key refused unless its type ends in "?"); `list(a|b)` is
# a SELECT: str(value) must be one of a, b. So `list(str)?` refused the
# defaults' `cameras: []` ("[]" is not "str") and the app never started.
_SUP_ELEMENT = re.compile(
    r"^(?:"
    r"|bool"
    r"|email"
    r"|url"
    r"|port"
    r"|device(?:\((?P<filter>subsystem=[a-z]+)\))?"
    r"|str(?:\((?P<s_min>\d+)?,(?P<s_max>\d+)?\))?"
    r"|password(?:\((?P<p_min>\d+)?,(?P<p_max>\d+)?\))?"
    r"|int(?:\((?P<i_min>-?\d+)?,(?P<i_max>-?\d+)?\))?"
    r"|float(?:\((?P<f_min>-?\d*\.?\d+)?,(?P<f_max>-?\d*\.?\d+)?\))?"
    r"|match\((?P<match>.*)\)"
    r"|list\((?P<list>.+)\)"
    r")\??$"
)


class _Refused(Exception):
    """The Supervisor would raise vol.Invalid here."""


def _sup_range(match, low, high, number):
    lo, hi = match.group(low), match.group(high)
    if (lo and number < float(lo)) or (hi and number > float(hi)):
        raise _Refused(f"{number} is outside ({lo}, {hi})")


def _supervisor_check(typ, value, key):
    """Return the value as the Supervisor stores it, or raise _Refused."""
    if isinstance(typ, list):
        if not isinstance(value, list):
            raise _Refused(f"Invalid list for option '{key}'")
        return [_supervisor_check(typ[0], element, key) for element in value]
    if isinstance(typ, dict):
        if not isinstance(value, dict):
            raise _Refused(f"Invalid dict for option '{key}'")
        out = {k: _supervisor_check(typ[k], v, k) for k, v in value.items() if k in typ}
        for missing in set(typ) - set(out):
            inner = typ[missing][0] if isinstance(typ[missing], list) and typ[missing] else typ[missing]
            if not (isinstance(inner, str) and inner.endswith("?")):
                raise _Refused(f"Missing option '{missing}' in {key}")
        return out
    if value is None:
        raise _Refused(f"Missing required option '{key}'")
    match = _SUP_ELEMENT.match(typ)
    if not match:
        raise _Refused(f"Unknown type '{typ}'")
    if typ.startswith(("str", "password")):
        if not isinstance(value, str):
            raise _Refused(f"'{key}' expects a string, got {value!r}")
        prefix = "s" if typ.startswith("str") else "p"
        _sup_range(match, f"{prefix}_min", f"{prefix}_max", len(value))
        return value
    if typ.startswith("int"):
        number = int(value)
        _sup_range(match, "i_min", "i_max", number)
        return number
    if typ.startswith("float"):
        number = float(value)
        _sup_range(match, "f_min", "f_max", number)
        return number
    if typ.startswith("bool"):
        if isinstance(value, str):
            if value.lower() not in ("1", "true", "yes", "on", "enable",
                                     "0", "false", "no", "off", "disable"):
                raise _Refused(f"'{key}' expects a boolean, got {value!r}")
            return value.lower() in ("1", "true", "yes", "on", "enable")
        return bool(value)
    if typ.startswith("url"):
        from urllib.parse import urlparse
        parsed = urlparse(value)
        if not parsed.scheme or not parsed.netloc:
            raise _Refused(f"'{key}' expects a URL, got {value!r}")
        return value
    if typ.startswith("match"):
        if not re.match(match.group("match"), str(value)):
            raise _Refused(f"'{key}' does not match {match.group('match')}")
        return str(value)
    if typ.startswith("list"):
        choices = match.group("list").split("|")
        if str(value) not in choices:
            raise _Refused(f"value must be one of {choices} for option '{key}', got {value!r}")
        return str(value)
    raise AssertionError(f"type '{typ}' is not mirrored here; mirror it before using it")


def test_the_supervisor_check_mirror_refuses_a_select_standing_in_for_a_list():
    """The mirror is only worth its fidelity: a select takes one of its
    values, never a list (what 0.9.0's `list(str)?` met in its own defaults),
    and the list syntax takes the list."""
    with pytest.raises(_Refused, match=r"one of \['str'\]"):
        _supervisor_check("list(str)?", [], "cameras")
    with pytest.raises(_Refused, match=r"one of \['str'\]"):
        _supervisor_check("list(str)?", ["person", "car"], "labels")
    assert _supervisor_check("list(standalone|frigate)", "frigate", "mode") == "frigate"
    assert _supervisor_check(["str?"], ["person", "car"], "labels") == ["person", "car"]
    assert _supervisor_check({"labels": ["str?"]}, {}, "frigate") == {}
    with pytest.raises(_Refused, match="Invalid list"):
        _supervisor_check(["str?"], "person", "labels")
    with pytest.raises(_Refused, match="Missing option 'name'"):
        _supervisor_check({"name": "str", "zone_id": "str?"}, {}, "cameras")


def _walk_schema(schema, options, path=""):
    """Yield (dotted path, schema entry, default) for every option leaf."""
    for key, typ in schema.items():
        here = f"{path}.{key}" if path else key
        value = options.get(key) if isinstance(options, dict) else None
        if isinstance(typ, dict):
            yield from _walk_schema(typ, value if isinstance(value, dict) else {}, here)
        else:
            yield here, typ, value


def test_every_list_option_is_declared_as_a_list():
    """A default that is a list must have the list syntax, so a select can
    never again stand in for one (0.9.0's frigate.cameras and labels)."""
    for path, typ, default in _walk_schema(CONFIG["schema"], CONFIG["options"]):
        if isinstance(default, list):
            assert isinstance(typ, list), f"{path}: a list default declared as {typ!r}"
        if isinstance(typ, str) and typ.startswith("list("):
            assert not isinstance(default, list), f"{path}: {typ!r} is a select"


def test_the_default_options_pass_the_supervisor_check():
    """write_options checks the stored options (config.yaml's defaults under
    whatever the person saved) before every start; the defaults alone must
    pass, or the app never starts."""
    stored = _supervisor_check(CONFIG["schema"], CONFIG["options"], "root")
    assert stored["frigate"]["cameras"] == []
    assert stored["frigate"]["labels"] == ["person", "car", "dog", "cat"]
    assert stored["cameras"] == []


def _load_serve_wizard():
    spec = importlib.util.spec_from_file_location("serve_wizard_store", ADDON / "serve_wizard.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize("payload", [
    {"mode": "frigate", "frigate_detected": True, "digest_enabled": False,
     "cameras": [{"name": "Front Door"}, {"name": "Driveway"}]},
    {"mode": "frigate", "frigate_detected": True, "digest_enabled": False, "cameras": []},
    {"mode": "standalone", "digest_enabled": False,
     "cameras": [{"name": "Front Door", "url": "rtsp://10.0.0.2/front"},
                 {"name": "\U0001F3E0", "url": "rtsp://10.0.0.3/home"}]},
], ids=["frigate, two cameras", "frigate, every camera", "standalone, two cameras"])
def test_the_wizard_saves_pass_the_supervisor_check(payload, monkeypatch, tmp_path):
    """What the wizard POSTs to /addons/self/options (over the options the
    Supervisor answers, here the defaults) must pass the same check, or the
    save is refused and the setup never completes."""
    wizard = _load_serve_wizard()
    sent: dict = {}
    monkeypatch.setattr(wizard, "DEVICE_KEY_FILE", tmp_path / "device_key")
    monkeypatch.setattr(wizard, "LEGACY_DEVICE_KEY_FILE", tmp_path / "legacy" / "device_key")
    monkeypatch.setattr(wizard, "WIZARD_PREFS_FILE", tmp_path / "wizard_prefs.json")
    monkeypatch.setattr(wizard, "get_addon_options", lambda: json.loads(json.dumps(CONFIG["options"])))
    monkeypatch.setattr(wizard, "set_addon_options", lambda options: sent.update(options=options))
    monkeypatch.setattr(wizard, "restart_addon", lambda: None)
    handler = wizard.WizardHandler.__new__(wizard.WizardHandler)
    result = handler._handle_save(payload)
    assert result["ok"] is True, result
    stored = _supervisor_check(CONFIG["schema"], {**CONFIG["options"], **sent["options"]}, "root")
    assert stored["mode"] == payload["mode"]
    if payload["mode"] == "frigate":
        assert stored["frigate"]["cameras"] == result["cameras"]
        assert stored["frigate"]["labels"] == ["person", "car", "dog", "cat"]
    else:
        assert [c["zone_id"] for c in stored["cameras"]] == ["zone:front_door", "zone:_"]


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
# seed file (crypto::resolve_device_seed_persisting_env created
# `<db>.ed25519.seed` at every first start, until the service set
# DEVICE_KEY_SEED_PERSIST=0) had no rule. A new file in the sources is a new
# row here AND a new rule in apparmor.txt.
#
#   witness.db(-wal/-shm)        storage.rs (SQLite WAL)            witnessd, witness_api, frigate_bridge
#   witness.db.health.json(.tmp) storage_health.rs store_state      witnessd, witness_api
#   witness.ed25519.seed         crypto/mod.rs device_key_path_for_db  witnessd + witness_api + frigate_bridge (read, mismatch check); the setup removes it
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
        "/data/device_key": "rw",
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


def _setup_profile_block():
    """The top-level profile's own rules: from its header to the first child."""
    start = APPARMOR.index(f"profile {CONFIG['slug']} ")
    end = APPARMOR.index("\n  profile ", start)
    return APPARMOR[start:end]


def test_apparmor_the_device_key_lives_in_data_and_the_setup_can_verify_it():
    """The seed file is the app's own (/data/device_key: the wizard writes
    it, the setup reads it); the setup's one-time move out of /config holds
    the seed against the sealed log with log_verify --lineage, which runs
    inherited under the top-level profile and opens the database as every
    kernel process does. No child profile but the panel names the key file,
    and nothing names the legacy path by itself (the panel's
    /config/.securacv/** covers the copy the move removes)."""
    setup = _file_rules(_setup_profile_block())
    assert setup["/data/**"] == "rw"
    assert "r" in setup["/config/witness.db"] and "k" in setup["/config/witness.db"]
    assert "r" in setup["/config/witness.db-*"] and "k" in setup["/config/witness.db-*"]
    assert setup["/config/.securacv/**"] == "rw"  # the move deletes the legacy copy
    assert _file_rules(_profile_block("panel"))["/data/device_key"] == "rw"
    for name in ("witness_api", "witnessd", "frigate_bridge", "event_mqtt_bridge"):
        assert "/data/device_key" not in _file_rules(_profile_block(name)), name
    assert "/config/.securacv/device_key" not in APPARMOR
    assert 'DEVICE_KEY_FILE="/data/device_key"' in RUN_SH
    assert 'LEGACY_DEVICE_KEY_FILE="/config/.securacv/device_key"' in RUN_SH
    wizard = (ADDON / "serve_wizard.py").read_text()
    assert 'DEVICE_KEY_FILE = Path("/data/device_key")' in wizard
    assert 'LEGACY_DEVICE_KEY_FILE = Path("/config/.securacv/device_key")' in wizard


def test_apparmor_no_kernel_process_writes_the_seed_file_and_the_setup_removes_it():
    """The seed never lands under /config again. witnessd used to persist
    the environment seed beside the database on a first standalone start;
    its service now sets DEVICE_KEY_SEED_PERSIST=0, so it, like witness_api
    and frigate_bridge, only reads the file (the seed-mismatch check for an
    install that still has one). The setup alone holds `w`, to remove the
    copy an earlier release left once it is shown to hold the key in use
    (run_lib.sh remove_kernel_seed_copy, called from run.sh)."""
    for profile in ("witnessd", "witness_api", "frigate_bridge"):
        assert _file_rules(_profile_block(profile))["/config/witness.ed25519.seed"] == "r", profile
    for profile in ("event_mqtt_bridge", "panel"):
        assert "/config/witness.ed25519.seed" not in _file_rules(_profile_block(profile)), profile
    assert _file_rules(_setup_profile_block())["/config/witness.ed25519.seed"] == "rw"
    service = (SERVICES_DIR / "witnessd" / "run").read_text()
    assert "export DEVICE_KEY_SEED_PERSIST=0\n" in service
    assert service.index("export DEVICE_KEY_SEED_PERSIST=0") < service.index("exec /usr/local/bin/witnessd")
    witnessd = (REPO / "src" / "bin" / "witnessd.rs").read_text()
    assert "DEVICE_KEY_SEED_PERSIST_ENV" in witnessd and "device_seed_persist_policy" in witnessd
    assert 'DEVICE_KEY_SEED_PERSIST_ENV: &str = "DEVICE_KEY_SEED_PERSIST"' in (
        REPO / "src" / "crypto" / "mod.rs"
    ).read_text()
    assert 'KERNEL_SEED_FILE="/config/witness.ed25519.seed"' in RUN_SH
    assert RUN_SH.count('remove_kernel_seed_copy "$KERNEL_SEED_FILE" "$DEVICE_KEY_SEED"') == 1


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
    # Pinned by digest as well as tag: the tag says which base, the digest
    # says which bytes, so a re-tagged upstream image cannot change the
    # add-on's runtime under a release.
    assert re.search(
        r"^FROM ghcr\.io/home-assistant/amd64-base:3\.18@sha256:[0-9a-f]{64} AS runtime-amd64$",
        DOCKERFILE,
        re.M,
    )
    assert re.search(
        r"^FROM ghcr\.io/home-assistant/aarch64-base:3\.18@sha256:[0-9a-f]{64} AS runtime-arm64$",
        DOCKERFILE,
        re.M,
    )
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


def test_run_sh_moves_the_device_key_out_of_config_before_reading_it():
    """The one-time move runs before the first-run gate and before the seed
    resolves, names the sealed log for the verification, and its verdicts
    are matched as fixed words: the seed itself is never logged. A copy that
    cannot be written stops the start (the app must not mint a new identity
    over a key it could not carry over)."""
    move = 'migrate_device_key_file "$LEGACY_DEVICE_KEY_FILE" "$DEVICE_KEY_FILE" "$DB_PATH"'
    assert RUN_SH.count(move) == 1
    assert RUN_SH.index(move) < RUN_SH.index('elif [ -s "$DEVICE_KEY_FILE" ]; then')
    assert RUN_SH.index(move) < RUN_SH.index('resolve_device_key_seed "$DEVICE_KEY_SEED" "$DEVICE_KEY_FILE"')
    for verdict in ('"moved verified")', '"moved no-log")', '"kept unverified")', '"kept differs")'):
        assert verdict in RUN_SH, verdict
    code = "\n".join(line for line in RUN_SH.splitlines() if not line.lstrip().startswith("#"))
    assert not re.search(r"bashio::log\.\w+ .*DEVICE_KEY_SEED", code)
    assert not re.search(r"bashio::log\.\w+ .*key_move", code)
    assert "exit 1" in RUN_SH[RUN_SH.index(move):RUN_SH.index("First-run detection")]
    # A seed the key file supplied is recorded in the option (the Configuration
    # tab is where the docs send people to copy it from): after the seed
    # resolves, only when the option was empty, through the panel's Supervisor
    # client with the seed in that child's environment and never on argv.
    record = 'DEVICE_KEY_SEED="$DEVICE_KEY_SEED" /usr/bin/python3 /usr/local/bin/serve_wizard.py record-device-key-seed'
    assert RUN_SH.count(record) == 1
    assert RUN_SH.index('resolve_device_key_seed "$DEVICE_KEY_SEED" "$DEVICE_KEY_FILE"') < RUN_SH.index(record)
    assert RUN_SH.index(record) < RUN_SH.index('DEVICE_ID=$(ha_device_id')
    assert 'OPTION_DEVICE_KEY_SEED="$DEVICE_KEY_SEED"' in RUN_SH
    assert RUN_SH.index('OPTION_DEVICE_KEY_SEED="$DEVICE_KEY_SEED"') < RUN_SH.index('resolve_device_key_seed "$DEVICE_KEY_SEED"')
    assert '[ -z "$OPTION_DEVICE_KEY_SEED" ] || [ "$OPTION_DEVICE_KEY_SEED" = "devkey:mvp" ]' in RUN_SH
    assert not re.search(r"serve_wizard\.py record-device-key-seed[^\n]*\$DEVICE_KEY_SEED", RUN_SH)
    # The token the integration reads did not move with it.
    const = (REPO / "custom_components" / "securacv" / "const.py").read_text()
    assert 'DEFAULT_TOKEN_FILE = "/config/api_token"' in const
    assert 'TOKEN_FILE="/config/api_token"' in RUN_SH


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
        # The device key: where it is, what a backup holds, what uninstall does.
        "`/data/device_key` inside the app", "the device key with it",
        "moves it, checks it against the sealed log",
    ):
        assert needle in docs, needle
    assert "appears automatically via MQTT discovery" not in docs
    # Nothing user-facing still sends people to the /config copy for a backup.
    for text in (docs, re.sub(r"\s+", " ", (ADDON / "wizard" / "index.html").read_text()),
                 re.sub(r"\s+", " ", (ADDON / "translations" / "en.yaml").read_text())):
        assert "stored at `/config/.securacv/device_key`" not in text
        assert "included in Home Assistant backups" not in text


def test_changelog_has_an_entry_for_this_version():
    changelog = (ADDON / "CHANGELOG.md").read_text()
    assert f"## {CONFIG['version']}" in changelog
