#!/usr/bin/env python3
"""Tests for firmware/scripts/check_sentinel_net_sync.sh — the pin needs a pin.

canary-sentinel carries canary-sense's network/witness stack as a copy, and
that script is the only thing that makes the copy safe. Its first version
pinned mqtt_mgr.cpp and witness.cpp function by function from a hand-kept
list, and a review found the list had holes exactly where the header said it
did not: a TLS-gate bypass in mqtt_connect_attempt, a client ID built from the
raw MAC, a 10x socket timeout, a one-byte change to the fingerprint domain or
to an NVS key name, and a disabled OTA-command gate all passed it green. A
pin that reads as covered while catching nothing is worse than no pin.

The script now pins both files WHOLE, minus a short list of named product
regions, and every region may only remove lines of a named shape. These cases
hold it to that, in both directions:

* every one of those six review mutations, applied to the sentinel's copy,
  fails it — and so do the regions it always covered;
* a fix landing in canary-sense's copy fails it until it is carried, including
  one smuggled into a sense-only region (the region refuses the line) and a
  new function (a whole-file pin has no "unlisted" functions);
* sense-only growth of the SHAPE a region names (one more radar dial) and a
  product-payload edit on either side pass — the regions are not so brittle
  that people learn to route around them.

Each case runs the real script against a scratch copy of the two project
trees, so nothing in the checkout is touched.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = Path("firmware/scripts/check_sentinel_net_sync.sh")
SENSE = Path("firmware/projects/canary-sense")
SENT = Path("firmware/projects/canary-sentinel")

MQTT = "src/net/mqtt_mgr.cpp"
WITNESS = "src/witness.cpp"


class SyncPin(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="sentinel-sync-")
        self.root = Path(self._tmp.name)
        (self.root / SCRIPT).parent.mkdir(parents=True)
        shutil.copy2(REPO / SCRIPT, self.root / SCRIPT)
        ignore = shutil.ignore_patterns(".pio", "secrets.h")
        for tree in (SENSE, SENT):
            shutil.copytree(REPO / tree, self.root / tree, ignore=ignore)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def edit(self, tree: Path, rel: str, old: str, new: str) -> None:
        """Replace the one occurrence of `old` — a fixture that no longer
        finds its anchor must fail loudly, not test nothing."""
        path = self.root / tree / rel
        text = path.read_text(encoding="utf-8")
        self.assertEqual(
            text.count(old), 1, f"fixture anchor not unique/present in {tree}/{rel}: {old!r}"
        )
        path.write_text(text.replace(old, new), encoding="utf-8")

    def run_pin(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["bash", str(self.root / SCRIPT)],
            capture_output=True,
            text=True,
            check=False,
        )

    def assert_passes(self) -> None:
        r = self.run_pin()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("in sync with canary-sense", r.stdout)

    def assert_drift(self, needle: str = "Drift detected") -> str:
        r = self.run_pin()
        self.assertEqual(r.returncode, 1, f"the pin passed a drifted copy:\n{r.stdout}{r.stderr}")
        self.assertIn(needle, r.stdout)
        return r.stdout


class CleanTree(SyncPin):
    def test_the_committed_trees_are_in_sync(self) -> None:
        self.assert_passes()


class SentinelDriftFails(SyncPin):
    """A change to the sentinel's copy that canary-sense does not have."""

    # --- the six the review found passing the per-function pin -------------

    def test_tls_gate_bypass_in_connect_fails(self) -> None:
        self.edit(
            SENT,
            MQTT,
            "    case canary::net::mqtt_tls::Prepared::Refused:\n"
            '      log_line("MQTT", tls_msg);\n'
            "      return false;\n",
            "    case canary::net::mqtt_tls::Prepared::Refused:\n"
            '      log_line("MQTT", tls_msg);\n'
            "      break;\n",
        )
        out = self.assert_drift()
        self.assertIn(MQTT, out)

    def test_client_id_from_the_raw_mac_fails(self) -> None:
        self.edit(
            SENT,
            MQTT,
            'String clientId = String("securacv-") + cfg.device_id + "-" + devid_hex;',
            'String clientId = String("securacv-") + cfg.device_id + "-" + WiFi.macAddress();',
        )
        self.assert_drift()

    def test_socket_timeout_constant_fails(self) -> None:
        self.edit(
            SENT,
            MQTT,
            "static constexpr uint16_t MQTT_SOCKET_TIMEOUT_SEC = 5;",
            "static constexpr uint16_t MQTT_SOCKET_TIMEOUT_SEC = 50;",
        )
        self.assert_drift()

    def test_fingerprint_domain_one_byte_fails(self) -> None:
        self.edit(
            SENT,
            WITNESS,
            '"securacv:pubkey:fingerprint";',
            '"securacv:pubkey:fingerprinT";',
        )
        out = self.assert_drift()
        self.assertIn(WITNESS, out)

    def test_nvs_key_name_fails(self) -> None:
        self.edit(SENT, WITNESS, '= "privkey";', '= "privkeY";')
        self.assert_drift()

    def test_ota_command_gate_disabled_fails(self) -> None:
        self.edit(
            SENT,
            MQTT,
            "  if (!is_install && !is_auto) return;\n",
            "  // if (!is_install && !is_auto) return;\n",
        )
        self.assert_drift()

    # --- the rest of the surface the header claims ----------------------------

    def test_install_token_relaxed_fails(self) -> None:
        self.edit(
            SENT,
            MQTT,
            'if (token_at(p, n, "install", 7)) s_pending_install = true;',
            "s_pending_install = true;",
        )
        self.assert_drift()

    def test_broker_transport_object_fails(self) -> None:
        self.edit(
            SENT,
            MQTT,
            "static PubSubClient mqtt(wifiClient);",
            "static PubSubClient mqtt(wifiClient);  // plain only",
        )
        self.assert_drift()

    def test_lwt_payload_fails(self) -> None:
        self.edit(SENT, MQTT, '"\\"status\\":\\"offline\\","', '"\\"status\\":\\"gone\\","')
        self.assert_drift()

    def test_chain_construction_fails(self) -> None:
        self.edit(
            SENT,
            WITNESS,
            "wc_chain_advance(s_head, payload_hash, seq, bucket_uptime_s, new_head);",
            "wc_chain_advance(s_head, payload_hash, 0, bucket_uptime_s, new_head);",
        )
        self.assert_drift()

    def test_an_extra_sentinel_only_function_fails(self) -> None:
        self.edit(
            SENT,
            MQTT,
            "bool mqtt_connected() { return mqtt.connected(); }\n",
            "bool mqtt_connected() { return mqtt.connected(); }\n\n"
            'void mqtt_debug_dump() { mqtt.publish("dbg", "x"); }\n',
        )
        self.assert_drift()

    def test_whole_file_copy_one_byte_fails(self) -> None:
        path = self.root / SENT / "src/net/ota_mgr.cpp"
        path.write_bytes(path.read_bytes() + b" ")
        self.assert_drift()

    def test_setup_network_named_like_sense_fails(self) -> None:
        self.edit(
            SENT,
            "src/net/wifi_mgr.cpp",
            '  pc.product_name = "Canary Sentinel";',
            '  pc.product_name = "Canary Sense";',
        )
        self.assert_drift("must name its own setup network")


class SenseFixMustBeCarried(SyncPin):
    """A change to canary-sense's copy the sentinel has not carried."""

    def test_a_tls_gate_fix_in_sense_fails_until_carried(self) -> None:
        old = (
            '    case canary::net::mqtt_tls::Prepared::Refused:\n      log_line("MQTT", tls_msg);\n'
        )
        new = old + "      mqtt.disconnect();\n"
        self.edit(SENSE, MQTT, old, new)
        self.assert_drift()
        self.edit(SENT, MQTT, old, new)  # carried
        self.assert_passes()

    def test_a_key_handling_fix_in_sense_fails_until_carried(self) -> None:
        old = "  // An all-zero blob is not a key — regenerate rather than sign with it.\n"
        new = old + "  // (fixed)\n"
        self.edit(SENSE, WITNESS, old, new)
        self.assert_drift()

    def test_a_new_sense_function_fails(self) -> None:
        self.edit(
            SENSE,
            MQTT,
            "bool mqtt_connected() { return mqtt.connected(); }\n",
            "bool mqtt_connected() { return mqtt.connected(); }\n\n"
            "bool mqtt_tls_ok() { return s_broker_tls.client() != nullptr; }\n",
        )
        self.assert_drift()

    def test_a_line_smuggled_into_the_resubscribe_region_is_refused(self) -> None:
        # An OTA re-subscribe is not a radar dial: the region must not eat it.
        self.edit(
            SENSE,
            MQTT,
            "  mqtt.subscribe(g_topics.cfg_debounce_cmd, 1);\n",
            "  mqtt.subscribe(g_topics.cfg_debounce_cmd, 1);\n"
            "  mqtt.subscribe(g_topics.update_cmd, 0);\n",
        )
        self.assert_drift("does not excuse")

    def test_a_line_smuggled_into_the_identify_branch_is_refused(self) -> None:
        self.edit(
            SENSE,
            MQTT,
            "      s_pending_identify = true;\n",
            "      s_pending_identify = true;\n      s_pending_install = true;\n",
        )
        self.assert_drift("does not excuse")

    def test_an_install_line_after_the_dial_dispatch_is_not_eaten(self) -> None:
        # The dial run stops at the first line of another shape, so the line
        # (and everything after it) is compared — and differs.
        self.edit(
            SENSE,
            MQTT,
            "  const bool is_install = (strcmp(topic, g_topics.update_cmd) == 0);\n",
            "  if (len > 64) return;\n"
            "  const bool is_install = (strcmp(topic, g_topics.update_cmd) == 0);\n",
        )
        self.assert_drift()

    def test_a_moved_anchor_fails_loudly(self) -> None:
        self.edit(
            SENSE,
            MQTT,
            "  // Identify button: re-subscribe so the wizard's blink request always\n",
            "  // Identify button — re-subscribe so the wizard's blink request always\n",
        )
        self.assert_drift("no longer matches the source exactly once")


class ProductRegionsStayOpen(SyncPin):
    """What the pin deliberately sets aside must stay set aside."""

    def test_one_more_radar_dial_in_sense_passes(self) -> None:
        self.edit(
            SENSE,
            MQTT,
            "static volatile long s_pending_cfg_hmax = -1;\n",
            "static volatile long s_pending_cfg_hmax = -1;\n"
            "static volatile long s_pending_cfg_zone = -1;\n",
        )
        self.edit(
            SENSE,
            MQTT,
            "    s_pending_cfg_hmax = parse_cfg_number(payload, len, 220);\n    return;\n  }\n",
            "    s_pending_cfg_hmax = parse_cfg_number(payload, len, 220);\n"
            "    return;\n"
            "  }\n"
            "  if (strcmp(topic, g_topics.cfg_zone_cmd) == 0) {\n"
            "    s_pending_cfg_zone = parse_cfg_number(payload, len, 8);\n"
            "    return;\n"
            "  }\n",
        )
        self.edit(
            SENSE,
            MQTT,
            "long take_pending_cfg_hmax()     { return take_pending(s_pending_cfg_hmax); }\n",
            "long take_pending_cfg_hmax()     { return take_pending(s_pending_cfg_hmax); }\n"
            "long take_pending_cfg_zone()     { return take_pending(s_pending_cfg_zone); }\n",
        )
        self.edit(
            SENSE,
            MQTT,
            "  mqtt.subscribe(g_topics.cfg_hmax_cmd, 1);\n",
            "  mqtt.subscribe(g_topics.cfg_hmax_cmd, 1);\n"
            "  mqtt.subscribe(g_topics.cfg_zone_cmd, 1);\n",
        )
        self.assert_passes()

    def test_a_sense_heartbeat_payload_edit_passes(self) -> None:
        self.edit(
            SENSE,
            MQTT,
            '"\\"radar_ok\\":%s,"\n           "\\"rssi',
            '"\\"radar_up\\":%s,"\n           "\\"rssi',
        )
        self.assert_passes()

    def test_a_sentinel_state_payload_edit_passes(self) -> None:
        self.edit(SENT, MQTT, '"\\"strong_modalities\\":%u,"', '"\\"strong_channels\\":%u,"')
        self.assert_passes()

    def test_the_sentinel_canonical_call_site_is_its_own(self) -> None:
        self.edit(
            SENT,
            WITNESS,
            "      claim.occupancy, claim.range, claim.modality_bits, bucket_uptime_s,\n"
            "      device_signature::device_id(), canonical, sizeof(canonical));",
            "      claim.occupancy, claim.range, claim.modality_bits,\n"
            "      bucket_uptime_s,\n"
            "      device_signature::device_id(), canonical, sizeof(canonical));",
        )
        self.assert_passes()


if __name__ == "__main__":
    unittest.main()
