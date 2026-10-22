"""docs/hardware/enclosure/gen_revisions.py — the part-revision ledger's
arithmetic, pinned: the fingerprint on a solid whose moments are known, the
tolerance that separates render noise from a design move, the region
finder on a bump it must see and a comment it must not, the comment
stripper, the -D round trip, and the recipe list recorded off render.sh
(bash and the stand-in only — nothing renders)."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ENC = REPO / "docs/hardware/enclosure"
sys.path.insert(0, str(ENC))
import gen_revisions as gr  # noqa: E402


def flipped(tris):
    """The same triangles wound inward: a void, when unioned into a solid."""
    return [(a, c, b) for a, b, c in tris]


def box_tris(lo, hi):
    """Twelve outward-wound triangles of an axis-aligned box."""
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    v = {
        "a": (x0, y0, z0), "b": (x1, y0, z0), "c": (x1, y1, z0), "d": (x0, y1, z0),
        "e": (x0, y0, z1), "f": (x1, y0, z1), "g": (x1, y1, z1), "h": (x0, y1, z1),
    }
    quads = [("a", "d", "c", "b"),   # bottom (z0), normal -z
             ("e", "f", "g", "h"),   # top, +z
             ("a", "b", "f", "e"),   # front (y0), -y
             ("b", "c", "g", "f"),   # right (x1), +x
             ("c", "d", "h", "g"),   # back (y1), +y
             ("d", "a", "e", "h")]   # left (x0), -x
    tris = []
    for p, q, r, s in quads:
        tris.append((v[p], v[q], v[r]))
        tris.append((v[p], v[r], v[s]))
    return tris


class Fingerprint(unittest.TestCase):
    def test_unit_cube_has_the_moments_a_cube_has(self):
        fp = gr.fingerprint(box_tris((0, 0, 0), (1, 1, 1)))
        self.assertEqual(fp["triangles"], 12)
        self.assertAlmostEqual(fp["volume_mm3"], 1.0, places=6)
        self.assertAlmostEqual(fp["area_mm2"], 6.0, places=6)
        self.assertEqual(fp["bbox_mm"], [1.0, 1.0, 1.0])
        self.assertEqual(fp["lo_mm"], [0.0, 0.0, 0.0])
        for c in fp["centroid_mm"]:
            self.assertAlmostEqual(c, 0.5, places=4)
        for m in fp["moments_mm2"]:
            self.assertAlmostEqual(m, 1 / 12, places=4)
        for p in fp["products_mm2"]:
            self.assertAlmostEqual(p, 0.0, places=4)
        for m in fp["third_mm3"]:
            self.assertAlmostEqual(m, 0.0, places=3)

    def test_moments_see_a_hole_that_moved(self):
        # the same volume, area, box and triangle count — a hole shifted
        # sideways is what the centroid and the second moments are for
        a = gr.fingerprint(box_tris((0, 0, 0), (10, 10, 10)) + flipped(box_tris((2, 2, 2), (3, 3, 3))))
        b = gr.fingerprint(box_tris((0, 0, 0), (10, 10, 10)) + flipped(box_tris((6, 2, 2), (7, 3, 3))))
        self.assertEqual(a["triangles"], b["triangles"])
        self.assertAlmostEqual(a["volume_mm3"], b["volume_mm3"], places=3)
        why = gr.geom_equal(a, b)
        self.assertIsNotNone(why)
        self.assertTrue(why.startswith(("centroid", "moments")), why)

    def test_a_hole_pair_moved_to_the_other_diagonal_reads_as_moved(self):
        # the review's example: holes at (-x,-y)/(x,y) moved to (-x,y)/(x,-y)
        # keep volume, area, box, centroid and every diagonal moment; only the
        # products of inertia (xy changes sign) tell the two apart
        def plate(holes):
            tris = box_tris((-20, -20, 0), (20, 20, 6))
            for hx, hy in holes:
                tris += flipped(box_tris((hx - 2, hy - 2, 1), (hx + 2, hy + 2, 5)))
            return gr.fingerprint(tris)
        a = plate([(-10, -10), (10, 10)])
        b = plate([(-10, 10), (10, -10)])
        self.assertEqual(a["triangles"], b["triangles"])
        self.assertEqual(a["moments_mm2"], b["moments_mm2"])
        self.assertEqual(a["centroid_mm"], b["centroid_mm"])
        why = gr.geom_equal(a, b)
        self.assertIsNotNone(why)
        self.assertTrue(why.startswith("products[xy]"), why)

    def test_a_feature_and_its_mirror_image_read_as_different(self):
        # one boss at +z on one end vs the same boss mirrored: a centroid move
        # catches most mirrors; the third moments catch the skew that survives
        base = box_tris((0, 0, 0), (40, 10, 4))
        a = gr.fingerprint(base + box_tris((2, 2, 4), (6, 8, 10)) + box_tris((30, 2, 4), (38, 8, 6)))
        b = gr.fingerprint(base + box_tris((34, 2, 4), (38, 8, 10)) + box_tris((2, 2, 4), (10, 8, 6)))
        self.assertIsNotNone(gr.geom_equal(a, b))
        self.assertNotEqual(a["third_mm3"][0], b["third_mm3"][0])

    def test_a_fingerprint_recorded_before_a_key_existed_still_compares(self):
        fp = gr.fingerprint(box_tris((0, 0, 0), (40, 30, 20)))
        old = {k: v for k, v in fp.items() if k not in ("products_mm2", "third_mm3")}
        self.assertIsNone(gr.geom_equal(old, fp))
        self.assertIsNone(gr.geom_equal(fp, old))

    def test_a_triangulation_fold_does_not_move_the_area(self):
        # OpenSCAD sometimes exports a planar face as a FOLD: an overlapping
        # coplanar triangle wound backwards (the 7" stand gauge: 14869.3 vs
        # 14711.4 mm² of plain area from the same sources on two runs). The
        # fold must cancel out of the area as it does out of the volume.
        p1, p2, p3, p4 = (0.0, 0.0, 0.0), (4.0, 0.0, 0.0), (0.0, 4.0, 0.0), (1.0, 2.0, 0.0)
        clean = [(p1, p2, p3)]
        fold = [(p1, p2, p3), (p1, p3, p4), (p1, p4, p3)]   # an overlap and its reverse
        self.assertAlmostEqual(gr.surface_area(clean), 8.0)
        self.assertAlmostEqual(gr.surface_area(fold), 8.0)
        # a real neighbor face at an angle is not merged into the patch
        wall = [(p1, p2, (0.0, 0.0, 3.0))]
        self.assertAlmostEqual(gr.surface_area(clean + wall), 8.0 + 6.0)
        # a closed box measures its six faces
        self.assertAlmostEqual(gr.surface_area(box_tris((0, 0, 0), (2, 3, 4))), 2 * (6 + 8 + 12))

    def test_render_noise_is_not_a_move(self):
        fp = gr.fingerprint(box_tris((0, 0, 0), (40, 30, 20)))
        noisy = dict(fp, volume_mm3=fp["volume_mm3"] + 0.01, area_mm2=fp["area_mm2"] + 0.005,
                     centroid_mm=[c + 0.001 for c in fp["centroid_mm"]])
        self.assertIsNone(gr.geom_equal(fp, noisy))
        moved = dict(fp, volume_mm3=fp["volume_mm3"] + 1.0)
        self.assertEqual(gr.geom_equal(fp, moved), f"volume_mm3 {fp['volume_mm3']} -> {moved['volume_mm3']}")

    def test_read_stl_refuses_what_is_not_a_binary_stl(self):
        with self.assertRaises(ValueError):
            gr.read_stl(b"solid x\nfacet normal 0 0 1\n" + b" " * 100)
        with self.assertRaises(ValueError):
            gr.read_stl(b"\0" * 84 + b"\0" * 10)


class Regions(unittest.TestCase):
    def test_a_bump_is_one_added_region_where_the_bump_is(self):
        old = box_tris((0, 0, 0), (30, 20, 10))
        new = old + box_tris((10, 5, 10), (16, 11, 14))   # a 6×6×4 bump on the top
        regs = gr.regions_between(old, new, cell=1.0)
        self.assertEqual([r["kind"] for r in regs], ["added"])
        b = regs[0]["box"]
        self.assertLessEqual(b[0], 10.5)
        self.assertGreaterEqual(b[3], 15.5)
        self.assertLessEqual(b[1], 5.5)
        self.assertGreaterEqual(b[4], 10.5)
        self.assertGreaterEqual(b[5], 13.5)
        # the reverse is the same box, removed
        back = gr.regions_between(new, old, cell=1.0)
        self.assertEqual([r["kind"] for r in back], ["removed"])
        self.assertEqual(back[0]["box"], b)

    def test_the_same_mesh_has_no_regions(self):
        tris = box_tris((0, 0, 0), (30, 20, 10))
        self.assertEqual(gr.regions_between(tris, tris, cell=1.0), [])

    def test_a_feature_that_moved_reads_as_moved_or_as_its_two_faces(self):
        base = box_tris((0, 0, 0), (40, 20, 10))
        old = base + box_tris((10, 5, 10), (14, 9, 14))
        new = base + box_tris((12, 5, 10), (16, 9, 14))      # the same post, 2 mm over
        regs = gr.regions_between(old, new, cell=1.0)
        # a stride of two cells: the face that left and the face that arrived
        # are either one "moved" box or an added/removed pair — both bracket
        # the post, and nothing else on the part is tinted
        self.assertTrue(1 <= len(regs) <= 2, regs)
        self.assertTrue({r["kind"] for r in regs} <= {"moved", "added", "removed"})
        for r in regs:
            b = r["box"]
            for lo_i, lo_min, hi_i, hi_max in ((0, 9, 3, 17), (1, 4, 4, 10), (2, 9, 5, 15)):
                self.assertGreaterEqual(b[lo_i], lo_min)
                self.assertLessEqual(b[hi_i], hi_max)
        # a 0.5 mm nudge of a 4 mm post is two thin slabs of material on its
        # opposite faces — the one face that lost material and the one that
        # gained it — and nothing anywhere else on the part
        near = base + box_tris((10.5, 5, 10), (14.5, 9, 14))
        regs = gr.regions_between(old, near, cell=1.0)
        self.assertTrue(1 <= len(regs) <= 2, regs)
        for r in regs:
            b = r["box"]
            self.assertGreaterEqual(b[0], 9)
            self.assertLessEqual(b[3], 16)

    def test_a_ring_of_holes_through_a_thin_plate_is_seen(self):
        # the case a surface-distance diff missed: holes drilled through a 2 mm
        # face, their walls within a cell of the old face planes
        plate = box_tris((0, 0, 0), (40, 40, 2))
        drilled = list(plate)
        holes = []
        for x in (10, 20, 30):
            holes += flipped(box_tris((x - 0.8, 18.2, 0), (x + 0.8, 19.8, 2)))   # inward-wound: a void
        regs = gr.regions_between(plate, drilled + holes, cell=0.4)
        self.assertTrue(regs, "the holes are a removed region")
        self.assertEqual({r["kind"] for r in regs}, {"removed"})
        for r in regs:
            self.assertGreaterEqual(r["box"][1], 17.5)
            self.assertLessEqual(r["box"][4], 20.5)

    def test_regions_are_capped_by_merging(self):
        base = box_tris((0, 0, 0), (200, 20, 10))
        bumps = []
        for i in range(gr.MAX_REGIONS + 6):
            x = 2 + i * 6
            bumps += box_tris((x, 5, 10), (x + 3, 8, 13))
        regs = gr.regions_between(base, base + bumps, cell=1.0)
        self.assertLessEqual(len(regs), gr.MAX_REGIONS)
        self.assertGreater(len(regs), 1)


class Sources(unittest.TestCase):
    def test_strip_comments_keeps_strings_and_code(self):
        src = 'a = 1; // one\n/* [Group] */ b = "x // not a comment"; /* multi\nline */ c = 2;'
        out = gr.strip_comments(src)
        self.assertIn('b = "x // not a comment"', out)
        self.assertIn("c = 2;", out)
        self.assertNotIn("one", out)
        self.assertNotIn("Group", out)
        self.assertNotIn("multi", out)

    def test_digest_ignores_comments_and_whitespace_but_not_a_literal(self):
        files = {"case.scad": "use <lib.scad>\nwall = 2.0; // the wall\nmodule m() {}\n",
                 "lib.scad": "function f() = 1;\n"}
        d0 = gr.src_digest("case.scad", {"part": "lid"}, files.get)
        reworded = dict(files, **{"case.scad": "use <lib.scad>\nwall =   2.0;   // a wall, reworded\nmodule m() {}\n"})
        self.assertEqual(gr.src_digest("case.scad", {"part": "lid"}, reworded.get), d0)
        moved = dict(files, **{"case.scad": files["case.scad"].replace("2.0", "2.4")})
        self.assertNotEqual(gr.src_digest("case.scad", {"part": "lid"}, moved.get), d0)
        lib = dict(files, lib={"lib.scad": "function f() = 2;\n"}["lib.scad"])
        lib = dict(files, **{"lib.scad": "function f() = 2;\n"})
        self.assertNotEqual(gr.src_digest("case.scad", {"part": "lid"}, lib.get), d0, "a library edit moves it")
        self.assertNotEqual(gr.src_digest("case.scad", {"part": "base"}, files.get), d0, "the selector is part of the identity")
        with self.assertRaises(FileNotFoundError):
            gr.src_digest("case.scad", {}, {"case.scad": "use <gone.scad>\n"}.get)

    def test_define_round_trip(self):
        for tok, val in (('"drum"', "drum"), ("15", 15), ("0.8", 0.8), ("true", True), ("false", False)):
            self.assertEqual(gr._parse_define(tok), val)
            self.assertEqual(gr.define_token(val), tok)

    def test_version_tag_reads_the_header_only(self):
        self.assertEqual(gr.version_of("//\n//  Canary — X  ⚠️ IN DEVELOPMENT (v0.3-dev)\n//"), "v0.3-dev")
        self.assertIsNone(gr.version_of("// no tag\n" * 3 + "// (v9.9) too late\n"))


class Recipes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.recipes = gr.all_recipes()

    def test_render_sh_is_the_recipe_list(self):
        keys = [r["key"] for r in self.recipes]
        self.assertEqual(len(keys), len(set(keys)))
        homes = {r["home"] for r in self.recipes}
        self.assertEqual(homes, {"released", "dev", "preview"})
        released = [r for r in self.recipes if r["home"] == "released"]
        self.assertGreaterEqual(len(released), 25)
        for r in released:
            self.assertTrue((ENC / r["key"]).is_file(), f"{r['key']} is a committed STL")
            self.assertTrue((ENC / r["scad"]).is_file(), r["scad"])
        for r in self.recipes:
            self.assertIn("part", r["defines"] | {"part": None} if r["scad"] in ("canary_dock.scad", "canary_hammond_chassis.scad") else r["defines"],
                          f"{r['key']}: every part recipe selects a part")

    def test_previews_are_gen_enclosures_presets(self):
        presets = gr.preview_presets()
        self.assertIn("canary_watch_station.scad", presets)
        previews = {r["key"]: r for r in self.recipes if r["home"] == "preview"}
        self.assertEqual(len(previews), sum(len(v) for v in presets.values()))
        for scad, parts in presets.items():
            for part in parts:
                key = f"preview/{Path(scad).stem}_{part}.stl"
                self.assertIn(key, previews)
                self.assertEqual(previews[key]["defines"]["part"], part)
                self.assertEqual(previews[key]["defines"]["$fs"], 0.8)
                self.assertTrue((REPO / gr.PREVIEW_REL / f"{Path(scad).stem}_{part}.stl").is_file())

    def test_every_recipe_digests_from_the_tree(self):
        for r in self.recipes:
            d = gr.src_digest(r["scad"], r["defines"], gr.read_tree)
            self.assertRegex(d, r"^[0-9a-f]{8}$")


if __name__ == "__main__":
    unittest.main()
