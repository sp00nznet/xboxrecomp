"""An indirect call whose site has a small recorded target set is guarded.

The runtime records, per `call` site, which targets the title reached
(icall_sites.dump); the merge tool keeps them per site. For a site whose
whole recorded set is small and translated, the lifter compares the target
against each and calls it directly, keeping the dispatch-table path as the
fallback -- so an unseen target is slow, never wrong. A site with no record,
too many targets, or a target the gen does not define stays exactly as
before, apart from now naming its site to the runtime.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000
A, B = BASE + 0x20, BASE + 0x30
SITE = BASE          # `call dword ptr [ecx + 4]` sits at the function start


def _image():
    img = b"\xff\x51\x04" + b"\xc3"          # call [ecx+4] ; ret
    img += b"\xcc" * (0x20 - len(img)) + b"\xc3"
    img += b"\xcc" * (0x30 - len(img)) + b"\xc3"
    img += b"\xcc" * 16
    config._install(
        [config.Section(".text", BASE, len(img), 0x0000, len(img), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="icall-guard-test")
    return img


def _db():
    def fn(va, size):
        return {"start": f"0x{va:08X}", "end": va + size, "_addr": va,
                "size": size, "name": f"sub_{va:08X}"}
    return {BASE: fn(BASE, 4), A: fn(A, 1), B: fn(B, 1)}


def test_a_recorded_site_is_guarded_with_the_generic_path_as_fallback():
    img, db = _image(), _db()
    tr = FunctionTranslator(img, db, icall_sites={SITE: [B, A]})
    c = tr.translate_function(BASE, db[BASE])
    assert f"if (_icall_target == 0x{A:08X}u) {{ RECOMP_ICALL_GUARD_HIT(); RECOMP_ABI_CALL(0x{A:08X}u, sub_{A:08X}); }}" in c, c
    assert f"RECOMP_ABI_CALL(0x{B:08X}u, sub_{B:08X})" in c, c
    assert f"RECOMP_ICALL_GUARD_MISS(); RECOMP_ICALL_SAFE_AT(_icall_target, _icall_esp, 0x{SITE:08X}u)" in c, c
    assert "/* indirect call, guarded 2 */" in c, c
    # The esp save the fallback restores from must still be planted, and the
    # block the post-pass opens for it must be closed: the first build of
    # this spelling failed on "function definition is not allowed here"
    # because the closing brace was keyed on the old macro name.
    assert "_icall_esp = g_esp" in c, c
    assert c.count("{") == c.count("}"), c


def test_an_unrecorded_site_names_itself_and_nothing_else():
    img, db = _image(), _db()
    c = FunctionTranslator(img, db).translate_function(BASE, db[BASE])
    assert f"RECOMP_ICALL_SAFE_AT(_icall_target, _icall_esp, 0x{SITE:08X}u)" in c, c
    assert "RECOMP_ICALL_GUARD_HIT" not in c, c
    assert "_icall_esp = g_esp" in c, c
    assert c.count("{") == c.count("}"), c


def test_a_target_the_gen_does_not_define_leaves_the_site_unguarded():
    img, db = _image(), _db()
    tr = FunctionTranslator(img, db, icall_sites={SITE: [A, BASE + 0x300]})
    c = tr.translate_function(BASE, db[BASE])
    assert "RECOMP_ICALL_GUARD_HIT" not in c, c


def test_more_than_four_targets_leaves_the_site_unguarded():
    img, db = _image(), _db()
    many = [A, B, BASE + 0x21, BASE + 0x22, BASE + 0x23]
    for va in many:
        db.setdefault(va, {"start": f"0x{va:08X}", "end": va + 1, "_addr": va,
                           "size": 1, "name": f"sub_{va:08X}"})
    tr = FunctionTranslator(img, db, icall_sites={SITE: many})
    c = tr.translate_function(BASE, db[BASE])
    assert "RECOMP_ICALL_GUARD_HIT" not in c, c
