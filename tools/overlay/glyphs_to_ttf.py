#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["fonttools==4.60.1", "picosvg==0.23.0", "skia-pathops==0.9.2"]
# [tool.uv]
# exclude-newer = "2026-10-01T00:00:00Z"
# ///
"""The overlay's two symbol fonts, made at commit time (overlay-revamp D7, D8).

  uv run tools/overlay/glyphs_to_ttf.py kenney --pack <Input Prompts dir>
  uv run tools/overlay/glyphs_to_ttf.py lucide --font <lucide.ttf> \\
      --codepoints <codepoints.json>

``kenney`` converts the chosen SVGs of Kenney's Input Prompts pack (CC0)
into one private-use-area font, ``third_party/kenney-input/xbr-input.ttf``,
and writes ``glyphs.txt`` (code point, family, slot, source SVG) and
``VERSION`` beside it. ``lucide`` subsets Lucide's release font to the menu's
icons, ``third_party/lucide/lucide-subset.ttf``, the same as the
``pyftsubset`` command its ``VERSION`` records, then names its licence in
the name table.

The script is not stdlib-only: it needs fonttools (TTF writing, subsetting)
and picosvg (SVG flattening, with skia-pathops for the union). The toolkit has
no pyproject.toml, so they are declared above as PEP 723 inline metadata, pinned
and with ``exclude-newer`` so their own dependencies resolve the same later,
and ``uv run`` installs them. The sources are checked against the digests
below before anything is made, so the fonts cannot drift with a different
download. Nothing at build or run time imports them; the
build embeds the committed TTFs. The output is deterministic: fixed
timestamps, sorted tables, no host paths, so a rerun on the same pack gives
the same bytes (tools/overlay/test_glyphs_to_ttf.py checks it).

Code points: the block b of a family (FAMILY_BLOCK) and slot s give
U+E000 + 0x40 * b + s. A slot is an input_map.h InputSource value
(1 south .. 31 rstick_right) or one of the whole-control slots below. Block
5 is the mouse, block 6 the shared glyphs, so the GameCube, added after
them, is block 7. src/kernel/nv2a_overlay_glyph.c
computes the same numbers; the overlay ctest checks that every glyph it
can ask for is in the font.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys

KENNEY_PACK = "Kenney Input Prompts 1.3"
KENNEY_SOURCE = (
    "https://github.com/Maaack/Kenney-Input-Prompts/tree/"
    "dd37497091c176e91ce81a14e203729ea12d8e4b"
)
KENNEY_SOURCE_NOTE = (
    "a mirror of https://kenney.nl/assets/input-prompts (the pack's own "
    "download refuses scripted fetches); License.txt there names the pack "
    "and its 1.3 release"
)
# Where third_party/kenney-input/LICENSE comes from: the mirror's own
# License.txt, copied unchanged.
KENNEY_LICENSE_NOTE = "LICENSE: the pack's License.txt at that commit, unchanged"
# The mirror's tarball at that commit (GitHub's codeload archive), and the
# SVGs the tables name, in name order: name, NUL, bytes. The pack is read as
# a directory, so the SVG digest is the one always checked.
KENNEY_ARCHIVE_SHA256 = (
    "76f1b16b853467448c1164b4307501172c47f1c3d3d0f94ed4ed702dc64a62b8"
)
KENNEY_SVGS_SHA256 = "ece0779af24cdc8b881594b47d812288e2bc3a67aa6bb95465bc67f6ef37d09d"
LUCIDE_RELEASE = "1.57.0"
LUCIDE_SOURCE = (
    "https://github.com/lucide-icons/lucide/releases/download/"
    "1.57.0/lucide-font-1.57.0.zip"
)
LUCIDE_TTF_SHA256 = "321fa12a84fdb68e0d5aad772db62502f936fc40217d9a122b130b6c5d4dfc86"
LUCIDE_CPS_SHA256 = "6e8d7838b855f82f8e276a3c2b6b8ec63603d8fce41fc1955e4a938bd1fd2e22"
LUCIDE_COPYRIGHT = (
    "Copyright (c) 2026 Lucide Icons and Contributors; "
    "some icons derived from Feather, Copyright (c) 2013-present Cole Bemis"
)
LUCIDE_LICENSE = "ISC License, with Feather's MIT notice; see LICENSE."
LUCIDE_LICENSE_URL = "https://lucide.dev/license"
KENNEY_COPYRIGHT = "Input Prompts (1.3) created/distributed by Kenney (www.kenney.nl)"
KENNEY_LICENSE = (
    "Creative Commons Zero 1.0 Universal: public domain, no attribution "
    "required; see LICENSE."
)
KENNEY_LICENSE_URL = "https://creativecommons.org/publicdomain/zero/1.0/"

PUA = 0xE000
BLOCK = 0x40

# host_pad.h's family order, and each family's block. The GameCube came
# after the mouse and shared blocks were numbered, so it skips them.
FAMILIES = ("xbox", "playstation", "nintendo", "steam", "generic", "gamecube")
FAMILY_BLOCK = {"xbox": 0, "playstation": 1, "nintendo": 2, "steam": 3, "generic": 4}
FAMILY_BLOCK["gamecube"] = 7
MOUSE_BLOCK = 5
SHARED_BLOCK = 6

# input_map.h's InputSource order, 1-based (0 is INPUT_SRC_NONE).
SOURCES = (
    "south",
    "east",
    "west",
    "north",
    "back",
    "guide",
    "start",
    "left_stick_click",
    "right_stick_click",
    "left_shoulder",
    "right_shoulder",
    "dpad_up",
    "dpad_down",
    "dpad_left",
    "dpad_right",
    "misc1",
    "touchpad",
    "paddle1",
    "paddle2",
    "paddle3",
    "paddle4",
    "left_trigger",
    "right_trigger",
    "lstick_up",
    "lstick_down",
    "lstick_left",
    "lstick_right",
    "rstick_up",
    "rstick_down",
    "rstick_left",
    "rstick_right",
)
# Whole controls, for a prompt about the control rather than one direction.
WHOLE = ("dpad", "lstick", "rstick")
SLOTS = SOURCES + WHOLE
MOUSE = ("left", "right", "middle", "wheel_up", "wheel_down", "move", "mouse")
SHARED = ("home",)


def _pad(prefix: str, stick: str, dpad: str, **named: str) -> dict[str, str]:
    """A family's table: the stick and d-pad sets follow one naming scheme
    in every pack folder; the rest is per family."""
    t = {
        "left_stick_click": f"{stick}_l_press",
        "right_stick_click": f"{stick}_r_press",
        "lstick_up": f"{stick}_l_up",
        "lstick_down": f"{stick}_l_down",
        "lstick_left": f"{stick}_l_left",
        "lstick_right": f"{stick}_l_right",
        "rstick_up": f"{stick}_r_up",
        "rstick_down": f"{stick}_r_down",
        "rstick_left": f"{stick}_r_left",
        "rstick_right": f"{stick}_r_right",
        "lstick": f"{stick}_l",
        "rstick": f"{stick}_r",
        # The outline d-pads: the pressed arm solid, the others outlined.
        # The plain ones mark the arm in a second colour, which a one-colour
        # font cannot show.
        "dpad_up": f"{dpad}_up_outline",
        "dpad_down": f"{dpad}_down_outline",
        "dpad_left": f"{dpad}_left_outline",
        "dpad_right": f"{dpad}_right_outline",
        "dpad": dpad,
    }
    t.update({k: f"{prefix}{v}" for k, v in named.items()})
    return {k: v + ".svg" for k, v in t.items()}


# Positional: each slot is the button in that place, so the Switch's south
# is B. The guide button is the shared home glyph on every family (no brand
# marks), so no family lists it.
KENNEY: dict[str, dict[str, str]] = {
    "xbox": _pad(
        "xbox_",
        "xbox_stick",
        "xbox_dpad",
        south="button_a",
        east="button_b",
        west="button_x",
        north="button_y",
        back="button_view",
        start="button_menu",
        misc1="button_share",
        left_shoulder="lb",
        right_shoulder="rb",
        left_trigger="lt",
        right_trigger="rt",
    ),
    "playstation": _pad(
        "playstation",
        "playstation_stick",
        "playstation_dpad",
        south="_button_cross",
        east="_button_circle",
        west="_button_square",
        north="_button_triangle",
        back="5_button_create",
        start="5_button_options",
        misc1="5_button_mute",
        touchpad="5_touchpad",
        left_shoulder="_trigger_l1",
        right_shoulder="_trigger_r1",
        left_trigger="_trigger_l2",
        right_trigger="_trigger_r2",
    ),
    "nintendo": _pad(
        "switch_",
        "switch_stick",
        "switch_dpad",
        south="button_b",
        east="button_a",
        west="button_y",
        north="button_x",
        back="button_minus",
        start="button_plus",
        left_shoulder="button_l",
        right_shoulder="button_r",
        left_trigger="button_zl",
        right_trigger="button_zr",
    ),
    "steam": _pad(
        "steamdeck_",
        "steamdeck_stick",
        "steamdeck_dpad",
        south="button_a",
        east="button_b",
        west="button_x",
        north="button_y",
        back="button_view",
        start="button_options",
        misc1="button_quickaccess",
        touchpad="trackpad",
        # SDL's Deck paddles: 1 = R4, 2 = L4, 3 = R5, 4 = L5.
        paddle1="button_r4",
        paddle2="button_l4",
        paddle3="button_r5",
        paddle4="button_l5",
        left_shoulder="button_l1",
        right_shoulder="button_r1",
        left_trigger="button_l2",
        right_trigger="button_r2",
    ),
    # The lettered fallback: only what differs from the Xbox set, whose
    # letters (A B X Y, LB RB LT RT) are also SDL's positional names. The
    # runtime takes every other generic slot from the Xbox block.
    "generic": {
        "back": "xbox_button_back.svg",
        "start": "xbox_button_start.svg",
        "left_stick_click": "xbox_ls.svg",
        "right_stick_click": "xbox_rs.svg",
    },
    # SDL's positions for the GameCube pad are A X B Y (SDL_gamepad.h): its
    # B sits west, so neither the Switch nor the Xbox letters fit. It has
    # no back button, no left shoulder and no stick clicks; Z is SDL's
    # right shoulder and the C stick the right stick. The runtime takes
    # the slots it lacks from the Nintendo block.
    "gamecube": {
        "south": "gamecube_button_a.svg",
        "east": "gamecube_button_x.svg",
        "west": "gamecube_button_b.svg",
        "north": "gamecube_button_y.svg",
        "start": "gamecube_button_start.svg",
        "right_shoulder": "gamecube_button_z.svg",
        "left_trigger": "gamecube_trigger_l.svg",
        "right_trigger": "gamecube_trigger_r.svg",
        "dpad_up": "gamecube_dpad_up_outline.svg",
        "dpad_down": "gamecube_dpad_down_outline.svg",
        "dpad_left": "gamecube_dpad_left_outline.svg",
        "dpad_right": "gamecube_dpad_right_outline.svg",
        "dpad": "gamecube_dpad.svg",
        "lstick_up": "gamecube_stick_up.svg",
        "lstick_down": "gamecube_stick_down.svg",
        "lstick_left": "gamecube_stick_left.svg",
        "lstick_right": "gamecube_stick_right.svg",
        "rstick_up": "gamecube_stick_c_up.svg",
        "rstick_down": "gamecube_stick_c_down.svg",
        "rstick_left": "gamecube_stick_c_left.svg",
        "rstick_right": "gamecube_stick_c_right.svg",
        "lstick": "gamecube_stick.svg",
        "rstick": "gamecube_stick_c.svg",
    },
}
KENNEY_MOUSE = {
    # The outline mice: the named part solid, the body outlined.
    "left": "mouse_left_outline.svg",
    "right": "mouse_right_outline.svg",
    "middle": "mouse_scroll_outline.svg",
    "wheel_up": "mouse_scroll_up_outline.svg",
    "wheel_down": "mouse_scroll_down_outline.svg",
    "move": "mouse_move.svg",
    "mouse": "mouse_outline.svg",
}
KENNEY_SHARED = {"home": "switch_button_home.svg"}
# The pack folders the tables name SVGs from.
KENNEY_FOLDERS = (
    "Xbox Series",
    "PlayStation Series",
    "Nintendo Switch",
    "Steam Deck",
    "Keyboard & Mouse",
    "Nintendo Gamecube",
)

# The menu's icons (D7), Lucide names; code points stay Lucide's own.
LUCIDE_ICONS = (
    "sliders-horizontal",
    "sparkles",
    "package",
    "gamepad-2",
    "puzzle",
    "activity",
    "keyboard",
    "bug",
    "folder",
    "circle-help",
    "maximize-2",
    "minimize-2",
    "x",
    "check",
    "chevron-left",
    "chevron-right",
    "refresh-ccw",
    "upload",
    "download",
)

UPM = 1024
SVG_BOX = 64  # every pack SVG is 64 x 64
# 2026-01-01 00:00 UTC in the head table's epoch (1904): fixed, so the bytes
# do not depend on the day the script ran.
FIXED_TIME = 3849984000


def kenney_table() -> list[tuple[int, str, str, str]]:
    """(code point, block name, slot name, SVG) for every glyph, in code
    point order. Stdlib only: the tests read it without fonttools."""
    rows = []
    for fam in FAMILIES:
        f = FAMILY_BLOCK[fam]
        t = KENNEY[fam]
        unknown = set(t) - set(SLOTS)
        if unknown:
            raise ValueError(f"{fam}: unknown slots {sorted(unknown)}")
        for s, name in enumerate(SLOTS, start=1):
            if name in t:
                rows.append((PUA + BLOCK * f + s, fam, name, t[name]))
    for s, name in enumerate(MOUSE):
        rows.append((PUA + BLOCK * MOUSE_BLOCK + s, "mouse", name, KENNEY_MOUSE[name]))
    for s, name in enumerate(SHARED):
        rows.append(
            (PUA + BLOCK * SHARED_BLOCK + s, "shared", name, KENNEY_SHARED[name])
        )
    return sorted(rows)


def glyphs_txt() -> str:
    lines = [
        "# xbr-input.ttf: code point, block, slot, source SVG",
        f"# {KENNEY_PACK}; made by tools/overlay/glyphs_to_ttf.py kenney",
    ]
    lines += [f"U+{cp:04X} {b} {s} {svg}" for cp, b, s, svg in kenney_table()]
    return "\n".join(lines) + "\n"


def sha256(path: str) -> str:
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def find_svgs(pack: str) -> dict[str, str]:
    """Basename to path for the vector SVGs of the pack folders the tables
    use. Other folders reuse basenames (Switch 2 has its own switch_*), so
    only these are read; a basename found twice in them is an error."""
    found: dict[str, str] = {}
    for folder in KENNEY_FOLDERS:
        d = os.path.join(pack, folder, "Vector")
        if not os.path.isdir(d):
            raise ValueError(f"no {folder}/Vector in {pack}")
        for n in sorted(os.listdir(d)):
            if not n.endswith(".svg"):
                continue
            if n in found:
                raise ValueError(f"{n}: in two pack folders")
            found[n] = os.path.join(d, n)
    return found


def svg_glyph(path: str):
    """One SVG as a TrueType glyph: picosvg flattens it (transforms, arcs,
    strokes and invisible shapes gone), skia-pathops unions its shapes, and
    the result is flipped to y up and scaled to the em."""
    import pathops
    from fontTools.pens.cu2quPen import Cu2QuPen
    from fontTools.pens.transformPen import TransformPen
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    from fontTools.svgLib.path import parse_path
    from picosvg.svg import SVG

    pico = SVG.parse(path).topicosvg()
    acc = pathops.Path()
    for shape in pico.shapes():
        # A second colour would be lost in a one-colour font, so the list
        # must name SVGs that have none.
        if (
            shape.fill.upper() not in ("#FFFFFF", "#FFF", "WHITE")
            or shape.opacity != 1.0
        ):
            raise ValueError(f"{path}: a shape filled {shape.fill} ({shape.opacity})")
        p = pathops.Path()
        parse_path(shape.d, p.getPen())
        p.fillType = (
            pathops.FillType.EVEN_ODD
            if shape.fill_rule == "evenodd"
            else pathops.FillType.WINDING
        )
        acc = pathops.op(acc, p, pathops.PathOp.UNION, fix_winding=True)
    pen = TTGlyphPen(None)
    k = UPM / SVG_BOX
    acc.draw(
        TransformPen(
            Cu2QuPen(pen, max_err=1.0, reverse_direction=True), (k, 0, 0, -k, 0, UPM)
        )
    )
    return pen.glyph()


def build_font(rows, family_name: str, version: str):
    """A font of rows (code point, glyph name, glyph) with an empty
    .notdef, ascent UPM and descent 0, so a pixel height is the SVG box."""
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen

    order = [".notdef"] + [name for _, name, _ in rows]
    glyphs = {".notdef": TTGlyphPen(None).glyph()}
    glyphs.update({name: g for _, name, g in rows})
    fb = FontBuilder(UPM, isTTF=True)
    fb.font.recalcTimestamp = False
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap({cp: name for cp, name, _ in rows})
    fb.setupGlyf(glyphs)
    metrics = {}
    for name in order:
        g = glyphs[name]
        metrics[name] = (UPM, getattr(g, "xMin", 0) if g.numberOfContours else 0)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=UPM, descent=0)
    fb.setupNameTable(
        {"familyName": family_name, "styleName": "Regular", "version": version}
    )
    fb.setupOS2(sTypoAscender=UPM, sTypoDescender=0, usWinAscent=UPM, usWinDescent=0)
    fb.setupPost()
    fb.setupHead(
        unitsPerEm=UPM, created=FIXED_TIME, modified=FIXED_TIME, fontRevision=1.0
    )
    return fb.font


def set_legal(font, copyright: str, licence: str, url: str) -> None:
    """Name IDs 0, 13 and 14 (copyright, licence, licence URL), so the
    terms travel inside the file, and fsType 0: both licences allow any
    embedding, and a nonzero fsType would tell tools otherwise."""
    name = font["name"]
    for nid, text in ((0, copyright), (13, licence), (14, url)):
        name.removeNames(nameID=nid)
        name.setName(text, nid, 3, 1, 0x409)
    name.names.sort()
    font["OS/2"].fsType = 0


def digest_svgs(svgs: dict[str, str], names) -> str:
    """The sha256 of the named SVGs in name order: name, NUL, bytes, NUL."""
    h = hashlib.sha256()
    for n in sorted(set(names)):
        with open(svgs[n], "rb") as f:
            h.update(n.encode() + b"\0" + f.read() + b"\0")
    return h.hexdigest()


def check(what: str, got: str, want: str) -> bool:
    if got == want:
        return True
    print(f"{what}: sha256 {got}, expected {want}", file=sys.stderr)
    return False


def kenney_version_text() -> str:
    lines = [
        f"{KENNEY_PACK}, Creative Commons Zero 1.0 (LICENSE)",
        f"source: {KENNEY_SOURCE}",
        f"  ({KENNEY_SOURCE_NOTE})",
        f"source archive sha256 {KENNEY_ARCHIVE_SHA256}",
        f"SVGs used, sha256 {KENNEY_SVGS_SHA256} (glyphs_to_ttf.digest_svgs)",
        KENNEY_LICENSE_NOTE,
    ]
    lines += [
        "made by: uv run tools/overlay/glyphs_to_ttf.py kenney --pack <pack dir>",
        f"glyphs: {len(kenney_table())} (glyphs.txt)",
        "",
    ]
    return "\n".join(lines)


def cmd_kenney(a) -> int:
    svgs = find_svgs(a.pack)
    table = kenney_table()
    missing = sorted({svg for *_, svg in table} - set(svgs))
    if missing:
        print(f"missing from the pack: {missing}", file=sys.stderr)
        return 1
    if a.pack_sha256 and not check(
        "the pack archive", a.pack_sha256, KENNEY_ARCHIVE_SHA256
    ):
        return 1
    if not check(
        "the pack's SVGs",
        digest_svgs(svgs, [svg for *_, svg in table]),
        KENNEY_SVGS_SHA256,
    ):
        return 1
    rows = [(cp, f"{b}.{s}", svg_glyph(svgs[svg])) for cp, b, s, svg in table]
    font = build_font(rows, "xbr-input", "Version 1.0; " + KENNEY_PACK)
    set_legal(font, KENNEY_COPYRIGHT, KENNEY_LICENSE, KENNEY_LICENSE_URL)
    os.makedirs(a.out, exist_ok=True)
    font.save(os.path.join(a.out, "xbr-input.ttf"))
    with open(
        os.path.join(a.out, "glyphs.txt"), "w", encoding="utf-8", newline="\n"
    ) as f:
        f.write(glyphs_txt())
    with open(os.path.join(a.out, "VERSION"), "w", encoding="utf-8", newline="\n") as f:
        f.write(kenney_version_text())
    lic = os.path.join(a.pack, "License.txt")
    if os.path.exists(lic):
        shutil.copyfile(lic, os.path.join(a.out, "LICENSE"))
    return 0


def lucide_codepoints(path: str) -> list[int]:
    with open(path, encoding="utf-8") as f:
        cps = json.load(f)
    missing = [n for n in LUCIDE_ICONS if n not in cps]
    if missing:
        raise ValueError(f"not in {path}: {missing}")
    return [cps[n] for n in LUCIDE_ICONS]


def pyftsubset_command(cps: list[int]) -> str:
    u = ",".join(f"U+{c:04X}" for c in sorted(cps))
    return (
        f"pyftsubset lucide.ttf --unicodes={u} --layout-features='' "
        "--output-file=lucide-subset.ttf"
    )


def lucide_version_text(cps: list[int], src_sha: str) -> str:
    lines = [
        f"Lucide {LUCIDE_RELEASE}, ISC licence and Feather's MIT notice (LICENSE)",
        f"source: {LUCIDE_SOURCE}",
        f"lucide-font/lucide.ttf sha256 {src_sha}",
        (
            "made by: uv run tools/overlay/glyphs_to_ttf.py lucide --font <lucide.ttf> "
            "--codepoints <codepoints.json>, which is:"
        ),
        f"  {pyftsubset_command(cps)}",
        "  then name IDs 0, 13 and 14 (copyright, licence, URL) and fsType 0",
        "icons:",
    ]
    lines += [f"  U+{c:04X} {n}" for n, c in zip(LUCIDE_ICONS, cps)]
    return "\n".join(lines) + "\n"


def subset_lucide(src: str, cps: list[int], out: str) -> None:
    from fontTools import subset

    opts = subset.Options()
    opts.layout_features = []
    font = subset.load_font(src, opts)
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=cps)
    sub.subset(font)
    set_legal(font, LUCIDE_COPYRIGHT, LUCIDE_LICENSE, LUCIDE_LICENSE_URL)
    subset.save_font(font, out, opts)


def cmd_lucide(a) -> int:
    if not check("the Lucide font", sha256(a.font), LUCIDE_TTF_SHA256) or not check(
        "the Lucide code points", sha256(a.codepoints), LUCIDE_CPS_SHA256
    ):
        return 1
    cps = lucide_codepoints(a.codepoints)
    os.makedirs(a.out, exist_ok=True)
    subset_lucide(a.font, cps, os.path.join(a.out, "lucide-subset.ttf"))
    with open(os.path.join(a.out, "VERSION"), "w", encoding="utf-8", newline="\n") as f:
        f.write(lucide_version_text(cps, LUCIDE_TTF_SHA256))
    return 0


def main(argv=None) -> int:
    root = os.path.normpath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
    )
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sp = p.add_subparsers(dest="cmd", required=True)
    k = sp.add_parser("kenney", help="the input glyph font from the Input Prompts pack")
    k.add_argument(
        "--pack", required=True, help="the unpacked pack (its Vector folders)"
    )
    k.add_argument(
        "--pack-sha256", help="the pack archive's sha256, checked when given"
    )
    k.add_argument("--out", default=os.path.join(root, "third_party", "kenney-input"))
    k.set_defaults(fn=cmd_kenney)
    lu = sp.add_parser("lucide", help="the icon subset from Lucide's release font")
    lu.add_argument("--font", required=True, help="lucide-font/lucide.ttf")
    lu.add_argument("--codepoints", required=True, help="lucide-font/codepoints.json")
    lu.add_argument("--out", default=os.path.join(root, "third_party", "lucide"))
    lu.set_defaults(fn=cmd_lucide)
    a = p.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
