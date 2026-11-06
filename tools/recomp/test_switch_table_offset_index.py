"""
Self-check for switch tables whose displacement is not slot 0 of the table.

Run: py -3 tools/recomp/test_switch_table_offset_index.py

MSVC's CRT memcpy/memmove dispatch their unaligned lead and trail bytes with

    and  eax, 3               ; 1..3 on this path, never 0
    jmp  [eax*4 + LeadUpVec - 4]

    sub  ecx, 4               ; -4..-1 when fewer than 4 dwords remain
    jmp  [ecx*4 + TrailUpVec + 16]

so reading forward from the displacement finds the previous instruction's
bytes (first form) or the code after the table (second form). Both were
lifted as unresolvable indirect tail jumps: Burnout 3's memcpy returned
without copying whenever the destination was unaligned.

The switch the lifter emits compares the loaded value against its arms, so
the arms only need to be found, not indexed. Both forms must produce a local
switch with every arm as a goto.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000
TABLE = BASE + 12
ARMS = [BASE + 24, BASE + 30, BASE + 36]


def _translate(image):
    config._install(
        [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="switch-offset-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


def _image(prefix, disp):
    """prefix; jmp [idx*4 + disp]; pad to TABLE; table; three arms."""
    code = prefix + b"\xFF\x24" + (b"\x85" if prefix[1] == 0xE0 else b"\x8D")
    code += disp.to_bytes(4, "little")
    code += b"\x90" * (TABLE - BASE - len(code))
    code += b"".join(a.to_bytes(4, "little") for a in ARMS)
    for n in range(1, 4):
        code += b"\xB8" + n.to_bytes(4, "little") + b"\xC3"   # mov eax, n; ret
    return code


def _assert_local_switch(code, what):
    assert "switch: 3 entries" in code, f"{what}: no local switch\n{code}"
    for arm in ARMS:
        assert f"goto loc_{arm:08X};" in code, f"{what}: arm {arm:#x} missing"


def test_index_starts_at_one():
    # and eax, 3; jmp [eax*4 + TABLE-4] -- slot 0 is the jmp's own bytes
    _assert_local_switch(_translate(_image(b"\x83\xE0\x03", TABLE - 4)),
                         "index-from-1 table")


def test_arms_below_the_base():
    # sub ecx, 4; jmp [ecx*4 + TABLE+12] -- base is one past the last arm
    _assert_local_switch(_translate(_image(b"\x83\xE9\x04", TABLE + 12)),
                         "negative-index table")


def _single_arm_image(after):
    """jmp [eax*4 + TABLE]; table [arm, after]; arm: mov eax, 1; ret."""
    code = b"\xFF\x24\x85" + TABLE.to_bytes(4, "little")
    code += b"\x90" * (TABLE - BASE - len(code))
    code += ARMS[0].to_bytes(4, "little") + after.to_bytes(4, "little")
    code += b"\x90" * (ARMS[0] - BASE - len(code))
    return code + b"\xB8\x01\x00\x00\x00\xC3"


def test_single_arm_ended_by_null_slot():
    # A null slot right after the one in-function arm closes a real table.
    code = _translate(_single_arm_image(0))
    assert "switch: 1 entries" in code, code
    assert f"goto loc_{ARMS[0]:08X};" in code, code
    # Any other word after a lone arm is still too weak to call a switch.
    assert "switch:" not in _translate(_single_arm_image(0x01010000))


if __name__ == "__main__":
    test_index_starts_at_one()
    test_arms_below_the_base()
    test_single_arm_ended_by_null_slot()
    print("ok")
