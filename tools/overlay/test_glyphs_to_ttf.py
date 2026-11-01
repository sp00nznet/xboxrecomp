"""Checks the glyph fonts in third_party against the script that made them.

The stdlib checks always run: glyphs.txt, VERSION and LICENSE must agree
with the script's tables, so a table edit without a regenerated font fails.
The byte-for-byte reproduce tests need fonttools and picosvg (run them with
`uv run --with fonttools --with picosvg --with skia-pathops pytest
tools/overlay`) and the sources, which are not in the repo:

  XBR_KENNEY_PACK   the unpacked Input Prompts pack (its Vector folders)
  XBR_LUCIDE_TTF    lucide-font/lucide.ttf of the release VERSION names
  XBR_LUCIDE_CPS    lucide-font/codepoints.json of the same release

Each skips with its reason when what it needs is missing.
"""

import hashlib
import importlib.util
import os
import tempfile
import unittest

from . import glyphs_to_ttf as g

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
KENNEY_DIR = os.path.join(ROOT, "third_party", "kenney-input")
LUCIDE_DIR = os.path.join(ROOT, "third_party", "lucide")

HAVE_FONTTOOLS = all(
    importlib.util.find_spec(m) is not None for m in ("fontTools", "picosvg", "pathops")
)
NO_FONTTOOLS = (
    "fonttools, picosvg or skia-pathops missing: "
    "uv run --with fonttools --with picosvg --with skia-pathops"
)


def _read(path: str) -> str:
    with open(path, encoding="utf-8") as f:
        return f.read()


def _sha(path: str) -> str:
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


class TableTest(unittest.TestCase):
    def test_glyphs_txt_matches_the_tables(self):
        self.assertEqual(_read(os.path.join(KENNEY_DIR, "glyphs.txt")), g.glyphs_txt())

    def test_code_points_are_unique_and_in_their_block(self):
        rows = g.kenney_table()
        cps = [r[0] for r in rows]
        self.assertEqual(len(cps), len(set(cps)))
        for cp, block, _, _ in rows:
            b = (cp - g.PUA) // g.BLOCK
            want = {"mouse": g.MOUSE_BLOCK, "shared": g.SHARED_BLOCK}.get(block)
            self.assertEqual(b, g.FAMILY_BLOCK[block] if want is None else want)
        self.assertEqual(cps, sorted(cps))

    def test_c_side_constants(self):
        # nv2a_overlay_glyph.c hard-codes these; a change here is a change there.
        self.assertEqual(g.PUA, 0xE000)
        self.assertEqual(g.BLOCK, 0x40)
        self.assertEqual(
            g.FAMILIES,
            ("xbox", "playstation", "nintendo", "steam", "generic", "gamecube"),
        )
        self.assertEqual([g.FAMILY_BLOCK[f] for f in g.FAMILIES], [0, 1, 2, 3, 4, 7])
        self.assertEqual(g.SLOTS.index("south") + 1, 1)
        self.assertEqual(g.SLOTS.index("dpad") + 1, 32)
        self.assertEqual(g.SLOTS.index("rstick") + 1, 34)
        self.assertEqual(
            g.MOUSE[:6], ("left", "right", "middle", "wheel_up", "wheel_down", "move")
        )
        self.assertEqual(g.PUA + g.BLOCK * g.SHARED_BLOCK, 0xE180)

    def test_switch_south_is_b(self):
        # Nintendo's south face button is labelled B, not A.
        self.assertEqual(g.KENNEY["nintendo"]["south"], "switch_button_b.svg")

    def test_gamecube_faces_are_sdls(self):
        # SDL places the GameCube's A X B Y south, east, west, north.
        t = g.KENNEY["gamecube"]
        self.assertEqual(
            [t[k] for k in ("south", "east", "west", "north")],
            [f"gamecube_button_{c}.svg" for c in "axby"],
        )

    def test_kenney_version(self):
        v = _read(os.path.join(KENNEY_DIR, "VERSION"))
        self.assertTrue(v.startswith(g.KENNEY_PACK + ","))
        self.assertIn(f"glyphs: {len(g.kenney_table())} ", v)
        lic = _read(os.path.join(KENNEY_DIR, "LICENSE"))
        self.assertIn("Creative Commons Zero", lic)

    def test_lucide_version_lists_every_icon(self):
        v = _read(os.path.join(LUCIDE_DIR, "VERSION"))
        self.assertTrue(v.startswith(f"Lucide {g.LUCIDE_RELEASE},"))
        icons = [line.split()[1] for line in v.split("icons:\n", 1)[1].splitlines()]
        self.assertEqual(tuple(icons), g.LUCIDE_ICONS)
        lic = _read(os.path.join(LUCIDE_DIR, "LICENSE"))
        self.assertIn("ISC", lic)
        self.assertIn("Feather", lic)


@unittest.skipUnless(HAVE_FONTTOOLS, NO_FONTTOOLS)
class ReproduceTest(unittest.TestCase):
    def test_kenney_font_reproduces(self):
        pack = os.environ.get("XBR_KENNEY_PACK")
        if not pack:
            self.skipTest("XBR_KENNEY_PACK not set (the unpacked Input Prompts pack)")
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(g.main(["kenney", "--pack", pack, "--out", out]), 0)
            self.assertEqual(
                _sha(os.path.join(out, "xbr-input.ttf")),
                _sha(os.path.join(KENNEY_DIR, "xbr-input.ttf")),
            )

    def test_lucide_subset_reproduces(self):
        ttf = os.environ.get("XBR_LUCIDE_TTF")
        cps = os.environ.get("XBR_LUCIDE_CPS")
        if not ttf or not cps:
            self.skipTest(
                "XBR_LUCIDE_TTF and XBR_LUCIDE_CPS not set (Lucide's release font)"
            )
        with tempfile.TemporaryDirectory() as out:
            self.assertEqual(
                g.main(["lucide", "--font", ttf, "--codepoints", cps, "--out", out]), 0
            )
            for n in ("lucide-subset.ttf", "VERSION"):
                self.assertEqual(
                    _sha(os.path.join(out, n)), _sha(os.path.join(LUCIDE_DIR, n)), n
                )

    def test_fonts_carry_their_licence(self):
        from fontTools.ttLib import TTFont

        for path, url in (
            (os.path.join(KENNEY_DIR, "xbr-input.ttf"), g.KENNEY_LICENSE_URL),
            (os.path.join(LUCIDE_DIR, "lucide-subset.ttf"), g.LUCIDE_LICENSE_URL),
        ):
            font = TTFont(path)
            self.assertEqual(font["OS/2"].fsType, 0, path)
            name = font["name"]
            for nid in (0, 13):
                self.assertTrue(name.getDebugName(nid), (path, nid))
            self.assertEqual(name.getDebugName(14), url, path)

    def test_a_different_source_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            ttf = os.path.join(d, "lucide.ttf")
            with open(ttf, "wb") as f:
                f.write(b"not the release font")
            out = os.path.join(d, "out")
            self.assertEqual(
                g.main(["lucide", "--font", ttf, "--codepoints", ttf, "--out", out]),
                1,
            )
            self.assertFalse(os.path.exists(out))

    def test_build_is_deterministic(self):
        # Two builds of the same SVG give the same bytes, with no source pack.
        svg = (
            '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">'
            '<circle cx="32" cy="32" r="20" fill="#fff"/>'
            '<rect x="8" y="8" width="10" height="10" fill="#fff"/></svg>'
        )
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "a.svg")
            with open(p, "w", encoding="utf-8") as f:
                f.write(svg)
            outs = []
            for i in range(2):
                font = g.build_font([(0xE001, "a", g.svg_glyph(p))], "t", "Version 1.0")
                path = os.path.join(d, f"{i}.ttf")
                font.save(path)
                outs.append(_sha(path))
            self.assertEqual(outs[0], outs[1])

    def test_a_second_colour_is_refused(self):
        svg = (
            '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">'
            '<rect x="8" y="8" width="10" height="10" fill="#f00"/></svg>'
        )
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "a.svg")
            with open(p, "w", encoding="utf-8") as f:
                f.write(svg)
            with self.assertRaises(ValueError):
                g.svg_glyph(p)


if __name__ == "__main__":
    unittest.main()
