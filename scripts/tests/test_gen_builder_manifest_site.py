#!/usr/bin/env python3
"""Pins the website half of docs/hardware/enclosure/gen_builder_manifest.py —
the distilled CAD ledger it carries as scad/cad-dims.json, and the --check
its --site carry gained.

What is pinned and why:
  • the ledger grew ADDITIVELY: every key that existed before `knobs`,
    `seams_mm`, `board_registry` and `board_facts` did is emitted exactly as
    before (the website pins those bytes through a sha256 and its model tests
    read envelope_mm / dims_source), and every new object is keyed in sorted
    order, so the carry stays byte-reproducible;
  • `knobs` appears on exactly the figures a params-bearing manifest draws and
    equals gen_cad_params' resolved, merged values — a reference is already
    the registry's number; the doorbell (no manifest) and the XIAO S3 (no
    params) add nothing; the DevKit's three knobs land on its own figure, not
    the stacked-XIAO's, though both manifests name one case file;
  • `seams_mm` is the ledger's assembled.seams_fig_d verbatim, unrounded
    (decision 7: the AR models place seams in meters; the page rounds);
  • the board registry rides along whole — nine rows with their evidence rung,
    seven facts — so page copy can be pinned to it;
  • a registry correction reaches the ledger through the same generator that
    writes the case; a manifest that disagrees with its case, a params-bearing
    manifest whose figure the ledger does not carry, and one figure drawn from
    two case files are each refused by name;
  • `--site DIR --check` names exactly the stale or missing carries and writes
    nothing; a write run writes only the carries that changed, is a fixed
    point on its second run, and leaves builder_manifest.json untouched when
    it is already current (it used to be rewritten on every --site run).

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import hashlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ENC = REPO / "docs" / "hardware" / "enclosure"
DEVICES = REPO / "devices"
FIGURES = REPO / "canary-local" / "devices" / "figures.json"

if str(ENC) not in sys.path:
    sys.path.insert(0, str(ENC))
import gen_builder_manifest as gbm  # noqa: E402
import gen_cad_params as gcp  # noqa: E402

LIB_NAME = "canary_board_lib.scad"
LIB_REL = "docs/hardware/enclosure/" + LIB_NAME
RELEASED = ["canary_wap_enclosure.scad", "canary_vision_enclosure.scad",
            "canary_vision_doorbell.scad", "canary_sense_enclosure.scad"]
NEW_TOP = {"board_registry", "board_facts"}
NEW_FIG = {"seams_mm", "knobs"}
# The five multi-part devices gen_assembled_dims.py measures, and their seams
# as the ledger states them (figure-depth mm, unrounded).
SEAMS = {
    "device.canary-sense": [19.5],
    "device.canary-vision": [21.38],
    "device.canary-vision-devkit": [16.5],
    "device.canary-vision-doorbell": [4, 28],
    "device.canary-wap": [13.05],
}
DIMS_BASE = 16   # 13 .scad carries + colorways.json + cad-dims.json + builder-data.js


def ledger_figures() -> dict[str, dict]:
    """figures.json's device/part figures with an envelope, by id."""
    data = json.loads(FIGURES.read_text(encoding="utf-8"))
    return {f["id"]: f for f in data["figures"]
            if f.get("role") in ("device", "part") and f.get("envelope_mm")}


def manifests() -> dict[str, dict]:
    return {p.parent.name: json.loads(p.read_text(encoding="utf-8"))
            for p in sorted(DEVICES.glob("*/device.json"))}


class _Tree:
    """A scratch repo: devices/ copied, the released cases and the board
    registry under the same relative path, so a write or a bad manifest can
    be exercised without touching the real sources. The ledger is read from
    the real figures.json unless a test hands the distill its own copy."""

    def __enter__(self) -> Path:
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        shutil.copytree(DEVICES, root / "devices")
        enc = root / "docs" / "hardware" / "enclosure"
        enc.mkdir(parents=True)
        for name in RELEASED + [LIB_NAME]:
            shutil.copyfile(ENC / name, enc / name)
        return root

    def __exit__(self, *exc) -> None:
        self._tmp.cleanup()


def edit(root: Path, slug: str, fn) -> None:
    path = root / "devices" / slug / "device.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    fn(data)
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")


def distill(root: Path, figures_json: Path | None = None) -> dict:
    return gbm.distill_cad_dims(figures_json, root / "devices", root)


class LedgerShape(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dims = gbm.distill_cad_dims()

    def test_top_level_keys_and_order(self):
        self.assertEqual(list(self.dims),
                         ["comment", "spec", "board_registry", "board_facts", "figures"])
        self.assertEqual(self.dims["spec"], "docs/design/FLEET_FIGURES.md")

    def test_every_key_that_existed_is_emitted_as_before(self):
        # The pre-knobs distill, re-derived from the ledger: strip the new keys
        # and what is left must be exactly what the website pins today.
        ledger = ledger_figures()
        figs = self.dims["figures"]
        self.assertEqual(list(figs), sorted(ledger))
        for fid, entry in figs.items():
            fig = ledger[fid]
            old = {k: v for k, v in entry.items() if k not in NEW_FIG}
            want = {"role": fig["role"], "confidence": fig["confidence"],
                    "dims_source": fig["dims_source"],
                    "envelope_mm": {k: fig["envelope_mm"][k] for k in ("w", "h", "d")}}
            if fig.get("sketch_note"):
                want["sketch_note"] = fig["sketch_note"]
            self.assertEqual(old, want, fid)
            self.assertEqual(list(old), list(want), fid)          # key order too
            # the new keys come after every old one, so the old lines' bytes hold
            self.assertEqual([k for k in entry if k in NEW_FIG],
                             [k for k in entry][len(old):], fid)
        # Nothing else changed at the top: the comment is the website's comment.
        self.assertTrue(self.dims["comment"].startswith("GENERATED by gen_builder_manifest.py "
                                                        "--site — do not edit."))

    def test_new_objects_are_key_sorted(self):
        reg = self.dims["board_registry"]
        self.assertEqual(list(reg), sorted(reg))
        for rid, row in reg.items():
            self.assertEqual(list(row), ["evidence", "l", "t", "w"], rid)
        facts = self.dims["board_facts"]
        self.assertEqual(list(facts), sorted(facts))
        for fid, entry in self.dims["figures"].items():
            if "knobs" in entry:
                self.assertEqual(list(entry["knobs"]), sorted(entry["knobs"]), fid)

    def test_seams_equal_the_ledger_unrounded(self):
        ledger = ledger_figures()
        for fid, entry in self.dims["figures"].items():
            seams = (ledger[fid].get("assembled") or {}).get("seams_fig_d")
            if seams:
                self.assertEqual(entry["seams_mm"], seams, fid)
            else:
                self.assertNotIn("seams_mm", entry, fid)
        self.assertEqual({fid: e["seams_mm"] for fid, e in self.dims["figures"].items()
                          if "seams_mm" in e}, SEAMS)
        # unrounded: 13.05 and 21.38 survive as the assembled generator measured them
        self.assertEqual(self.dims["figures"]["device.canary-wap"]["seams_mm"], [13.05])
        self.assertEqual(self.dims["figures"]["device.canary-vision"]["seams_mm"], [21.38])

    def test_knobs_exactly_where_a_params_manifest_draws(self):
        drawn = {m["figure"] for m in manifests().values()
                 if m.get("figure") and (m.get("cad") or {}).get("params")}
        self.assertEqual(drawn, {"device.canary-wap", "device.canary-sense",
                                 "device.canary-vision", "device.canary-vision-devkit"})
        with_knobs = {fid for fid, e in self.dims["figures"].items() if "knobs" in e}
        self.assertEqual(with_knobs, drawn)
        # the doorbell has no manifest; every part figure has none either
        self.assertNotIn("knobs", self.dims["figures"]["device.canary-vision-doorbell"])
        for fid, e in self.dims["figures"].items():
            if e["role"] == "part":
                self.assertNotIn("knobs", e, fid)

    def test_knobs_are_the_resolved_manifest_values(self):
        owned, errors = gcp.load_params()
        self.assertEqual(errors, [])
        figs = self.dims["figures"]
        wap = figs["device.canary-wap"]["knobs"]
        self.assertEqual(wap, {"board_clear": 0.6, "board_h": 1.2, "board_l": 21.0,
                               "board_w": 17.5, "stack_camera": 8.0, "stack_plain": 4.5})
        # references arrive as the registry's numbers, and the per-case decision
        # survives: the Sense clips say brd_w("xiao") = 17.5, the Vision pins the
        # measured 17.8 — same registry, two manifests, two knobs
        self.assertEqual(figs["device.canary-sense"]["knobs"]["xiao_w"], 17.5)
        self.assertEqual(figs["device.canary-vision"]["knobs"]["xiao_w"], 17.8)
        self.assertEqual(figs["device.canary-vision"]["knobs"]["stack_sock_h"], 6.5)
        # one case file, two figures: the DevKit's knobs are its own, not the union
        devkit = figs["device.canary-vision-devkit"]["knobs"]
        self.assertEqual(devkit, {"dk_l": 39.0, "dk_w": 25.4, "stack_h": 9.0})
        vision = figs["device.canary-vision"]["knobs"]
        self.assertEqual(set(vision), {"vm_l", "vm_w", "xiao_l", "xiao_w", "stack_sock_h",
                                       "xiao_below", "vm_front_h", "cam_w", "cam_h", "pcb_t",
                                       "board_clear"})
        for scad_rel, keys in owned.items():
            for name, o in keys.items():
                fig = {"canary_wap_enclosure.scad": "device.canary-wap",
                       "canary_sense_enclosure.scad": "device.canary-sense"}.get(
                           Path(scad_rel).name)
                if fig is None:      # the shared Vision case: by slug
                    fig = ("device.canary-vision-devkit" if o.slugs == ["canary-vision-devkit"]
                           else "device.canary-vision")
                self.assertEqual(figs[fig]["knobs"][name], o.value, f"{fig}.{name}")

    def test_board_registry_rides_along_with_its_evidence(self):
        reg = self.dims["board_registry"]
        self.assertEqual(len(reg), 9)
        self.assertEqual(reg["xiao"], {"evidence": "spec", "l": 21.0, "t": 1.2, "w": 17.5})
        self.assertEqual(reg["grove_v2"], {"evidence": "measured", "l": 40.0, "t": 1.0,
                                           "w": 20.0})
        self.assertEqual(reg["ws147"]["evidence"], "drawing")
        for rid, row in reg.items():
            self.assertIn(row["evidence"], {"measured", "drawing", "spec", "unmeasured"}, rid)
        facts = self.dims["board_facts"]
        self.assertEqual(len(facts), 7)
        self.assertEqual(facts["brd_xiao_w_measured"], 17.8)
        self.assertEqual(facts["brd_stack_sock_measured"], 6.5)
        self.assertEqual(facts["brd_ws169_glass_h"], 41.13)
        # the same numbers the resolver hands the cases — one registry, read once
        reg_src = gcp.parse_board_registry()
        self.assertEqual({r: (row.dims["l"], row.dims["w"], row.dims["t"], row.status)
                          for r, row in reg_src.rows.items()},
                         {r: (row["l"], row["w"], row["t"], row["evidence"])
                          for r, row in reg.items()})

    def test_distill_is_reproducible(self):
        again = gbm.distill_cad_dims()
        self.assertEqual(json.dumps(again, indent=2, ensure_ascii=False),
                         json.dumps(self.dims, indent=2, ensure_ascii=False))


class ScratchTree(unittest.TestCase):
    def test_a_registry_correction_flows_to_the_ledger(self):
        with _Tree() as root:
            lib = root / LIB_REL
            src = lib.read_text(encoding="utf-8")
            old = '["xiao",       21.0,  17.5, 1.2, "spec",'
            self.assertEqual(src.count(old), 1)
            lib.write_text(src.replace(old, '["xiao",       21.4,  17.5, 1.2, "spec",'),
                           encoding="utf-8")
            written, errors = gcp.write(root / "devices", root)
            self.assertEqual(errors, [])
            self.assertEqual(sorted(c.name for _, c in written),
                             ["board_l", "xiao_l", "xiao_l"])     # WAP, Sense, Vision
            dims = distill(root)
            self.assertEqual(dims["board_registry"]["xiao"]["l"], 21.4)
            figs = dims["figures"]
            self.assertEqual(figs["device.canary-wap"]["knobs"]["board_l"], 21.4)
            self.assertEqual(figs["device.canary-sense"]["knobs"]["xiao_l"], 21.4)
            self.assertEqual(figs["device.canary-vision"]["knobs"]["xiao_l"], 21.4)
            self.assertEqual(figs["device.canary-vision-devkit"]["knobs"]["dk_l"], 39.0)

    def test_a_manifest_that_disagrees_with_its_case_is_refused(self):
        with _Tree() as root:
            edit(root, "canary-wap", lambda m: m["cad"]["params"].__setitem__("board_h", 1.6))
            with self.assertRaises(SystemExit) as cm:
                distill(root)
            msg = str(cm.exception.code)
            self.assertIn("board_h = 1.2 in the .scad, but devices/canary-wap cad.params says "
                          "1.6", msg)
            self.assertIn("gen_cad_params.py", msg)

    def test_a_params_manifest_whose_figure_the_ledger_lacks_is_refused(self):
        with _Tree() as root:
            ledger = json.loads(FIGURES.read_text(encoding="utf-8"))
            for fig in ledger["figures"]:
                if fig["id"] == "device.canary-wap":
                    del fig["envelope_mm"]
            fj = root / "figures.json"
            fj.write_text(json.dumps(ledger), encoding="utf-8")
            with self.assertRaises(SystemExit) as cm:
                distill(root, fj)
            self.assertIn("device.canary-wap", str(cm.exception.code))
            self.assertIn("no ledger home", str(cm.exception.code))

    def test_one_figure_drawn_from_two_case_files_is_refused(self):
        with _Tree() as root:
            edit(root, "canary-vision-devkit",
                 lambda m: m.__setitem__("figure", "device.canary-wap"))
            with self.assertRaises(SystemExit) as cm:
                distill(root)
            msg = str(cm.exception.code)
            self.assertIn("device.canary-wap", msg)
            self.assertIn("devices/canary-vision-devkit (canary_vision_enclosure.scad)", msg)
            self.assertIn("devices/canary-wap (canary_wap_enclosure.scad)", msg)

    def test_a_manifest_without_a_figure_or_without_params_contributes_nothing(self):
        with _Tree() as root:
            edit(root, "canary-wap", lambda m: m.pop("figure"))
            edit(root, "canary-sense", lambda m: m["cad"].__setitem__("params", {}))
            figs = distill(root)["figures"]
            self.assertNotIn("knobs", figs["device.canary-wap"])
            self.assertNotIn("knobs", figs["device.canary-sense"])
            self.assertIn("knobs", figs["device.canary-vision"])
            self.assertEqual(figs["device.canary-wap"]["seams_mm"], [13.05])   # still measured


class SiteCarry(unittest.TestCase):
    """A fake website checkout in a temp dir; builder_manifest.json is
    redirected to a temp copy so the committed one is never touched."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.site = Path(self._tmp.name) / "site"
        (self.site / "js").mkdir(parents=True)
        self.manifest = gbm.build_manifest()
        self.fresh = json.dumps(self.manifest, indent=2, ensure_ascii=False) + "\n"
        self._real_manifest = gbm.MANIFEST
        gbm.MANIFEST = Path(self._tmp.name) / "builder_manifest.json"
        gbm.MANIFEST.write_text(self.fresh, encoding="utf-8")

    def tearDown(self):
        gbm.MANIFEST = self._real_manifest
        self._tmp.cleanup()

    def run_main(self, *argv: str) -> tuple[int | None, str, SystemExit | None]:
        with redirect_stdout(io.StringIO()) as out:
            try:
                rc = gbm.main(list(argv))
            except SystemExit as e:
                return None, out.getvalue(), e
        return rc, out.getvalue(), None

    def snapshot(self) -> dict[str, bytes]:
        return {str(p.relative_to(self.site)): p.read_bytes()
                for p in self.site.rglob("*") if p.is_file()}

    def test_carried_set(self):
        carries = gbm.site_carries(self.manifest)
        self.assertEqual(len(carries), DIMS_BASE)
        names = list(carries)
        for model in gbm.CURATED:
            self.assertIn(f"scad/{model['file']}", names)
        self.assertIn("scad/canary_watch_station.scad", names)      # REFERENCE_SCADS
        self.assertIn("scad/canary_board_lib.scad", names)          # a use<> dep
        self.assertEqual(names[-3:], ["scad/colorways.json", "scad/cad-dims.json",
                                      "js/builder-data.js"])
        # the pin in builder-data.js is the sha256 of the very bytes carried
        data = json.loads(carries["js/builder-data.js"].decode("utf-8")
                          .split("export const BUILDER = ", 1)[1].rstrip().rstrip(";"))
        self.assertEqual(data["cad_dims"]["sha256"],
                         hashlib.sha256(carries["scad/cad-dims.json"]).hexdigest())
        self.assertEqual(data["colorways_carry"]["sha256"],
                         hashlib.sha256(carries["scad/colorways.json"]).hexdigest())
        for name, blob in carries.items():
            if name.endswith(".scad"):
                self.assertEqual(blob, (ENC / Path(name).name).read_bytes(), name)
        # the carried ledger is the distill, byte for byte
        self.assertEqual(carries["scad/cad-dims.json"].decode("utf-8"),
                         json.dumps(gbm.distill_cad_dims(), indent=2, ensure_ascii=False) + "\n")

    def test_write_then_check_is_green_and_a_second_write_is_a_fixed_point(self):
        rc, out, _ = self.run_main("--site", str(self.site))
        self.assertEqual(rc, 0)
        self.assertIn(f"{DIMS_BASE} of {DIMS_BASE} website carries written", out)
        first = self.snapshot()
        self.assertEqual(len(first), DIMS_BASE)
        rc, out, _ = self.run_main("--site", str(self.site), "--check")
        self.assertEqual(rc, 0)
        self.assertIn(f"are up to date ({DIMS_BASE} files)", out)
        rc, out, _ = self.run_main("--site", str(self.site))
        self.assertEqual(rc, 0)
        self.assertIn("nothing written", out)
        self.assertEqual(self.snapshot(), first)

    def test_check_names_exactly_the_stale_carry_and_writes_nothing(self):
        self.run_main("--site", str(self.site))
        victim = self.site / "scad" / "canary_core_lib.scad"
        corrupt = victim.read_bytes() + b"\n"
        victim.write_bytes(corrupt)
        before = self.snapshot()
        rc, out, exc = self.run_main("--site", str(self.site), "--check")
        self.assertIsNone(rc)
        self.assertIsNotNone(exc)
        self.assertIn("rerun gen_builder_manifest.py --site", str(exc.code))
        stale = [ln.strip()[2:] for ln in out.splitlines() if ln.strip().startswith("✗")]
        self.assertEqual(stale, ["scad/canary_core_lib.scad (stale)"])
        self.assertIn(f"1 of {DIMS_BASE} website carries are not current", out)
        self.assertEqual(self.snapshot(), before)          # --check wrote nothing
        # a missing carry is named as missing, in write order after the stale one
        (self.site / "js" / "builder-data.js").unlink()
        rc, out, exc = self.run_main("--site", str(self.site), "--check")
        stale = [ln.strip()[2:] for ln in out.splitlines() if ln.strip().startswith("✗")]
        self.assertEqual(stale, ["scad/canary_core_lib.scad (stale)",
                                 "js/builder-data.js (missing)"])
        self.assertFalse((self.site / "js" / "builder-data.js").exists())
        # and a write repairs exactly those two
        rc, out, _ = self.run_main("--site", str(self.site))
        self.assertEqual(rc, 0)
        self.assertIn(f"2 of {DIMS_BASE} website carries written", out)
        self.assertEqual(victim.read_bytes(), (ENC / "canary_core_lib.scad").read_bytes())

    def test_a_site_run_leaves_a_current_manifest_untouched(self):
        old = 1_000_000_000
        os.utime(gbm.MANIFEST, (old, old))
        rc, out, _ = self.run_main("--site", str(self.site))
        self.assertEqual(rc, 0)
        self.assertIn("builder_manifest.json is unchanged — not rewritten", out)
        self.assertEqual(int(gbm.MANIFEST.stat().st_mtime), old)
        self.assertEqual(gbm.MANIFEST.read_text(encoding="utf-8"), self.fresh)
        # a stale manifest IS rewritten, --site or not
        gbm.MANIFEST.write_text("{}\n", encoding="utf-8")
        rc, out, _ = self.run_main("--site", str(self.site))
        self.assertEqual(rc, 0)
        self.assertIn(f"wrote {gbm.MANIFEST}", out)
        self.assertEqual(gbm.MANIFEST.read_text(encoding="utf-8"), self.fresh)
        gbm.MANIFEST.write_text("{}\n", encoding="utf-8")
        rc, out, _ = self.run_main()
        self.assertEqual(rc, 0)
        self.assertEqual(gbm.MANIFEST.read_text(encoding="utf-8"), self.fresh)

    def test_check_alone_still_gates_the_manifest(self):
        rc, out, _ = self.run_main("--check")
        self.assertEqual(rc, 0)
        self.assertIn("builder_manifest.json is up to date", out)
        gbm.MANIFEST.write_text("{}\n", encoding="utf-8")
        rc, out, exc = self.run_main("--check")
        self.assertIsNone(rc)
        self.assertIn("builder_manifest.json is stale", str(exc.code))
        self.assertEqual(gbm.MANIFEST.read_text(encoding="utf-8"), "{}\n")   # not repaired
        # with --site, a stale manifest is reported before any carry is compared
        rc, out, exc = self.run_main("--site", str(self.site), "--check")
        self.assertIn("builder_manifest.json is stale", str(exc.code))
        self.assertEqual(self.snapshot(), {})

    def test_not_a_website_checkout(self):
        bare = Path(self._tmp.name) / "bare"
        bare.mkdir()
        rc, out, exc = self.run_main("--site", str(bare))
        self.assertIsNone(rc)
        self.assertIn("does not look like the website checkout", str(exc.code))
        self.assertEqual(list(bare.iterdir()), [])


class CommittedTree(unittest.TestCase):
    def test_the_committed_manifest_is_current(self):
        with redirect_stdout(io.StringIO()) as out:
            self.assertEqual(gbm.main(["--check"]), 0)
        self.assertIn("builder_manifest.json is up to date", out.getvalue())


if __name__ == "__main__":
    unittest.main()
