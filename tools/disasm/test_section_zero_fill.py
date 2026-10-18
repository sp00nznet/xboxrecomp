"""
Section bytes include the loader's zero fill past raw_size.

Spartan: Total Warrior (XDK 5849) has a D3DX section whose raw size stops one
byte short of its last instruction: D3DXMatrixLookAtLH ends `leave; ret 0x10`
and the ret's third byte, the immediate's zero high byte, is the loader's
zero fill. Cut at raw_size the ret did not decode, the function ended at
`leave`, and its translation returned without popping its return address and
arguments, so the caller restored edi/esi/ebx from the wrong stack slots and
the front end asked the section table for an empty name.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.engine import DisasmEngine  # noqa: E402
from tools.disasm.loader import BinaryImage, SectionInfo  # noqa: E402

VA = 0x00237F90
# fld1; fstp [eax+0x3c]; leave; ret 0x10 -- minus the ret's last byte.
TAIL = bytes.fromhex("d9e8 d9583c c9 c210")


def _image(raw, virtual_size, executable=True):
    sec = SectionInfo(name="D3DX", virtual_addr=VA, virtual_size=virtual_size,
                      raw_addr=0, raw_size=len(raw), writable=False,
                      executable=executable, flags="")
    img = BinaryImage(filepath="", raw_data=raw + b"\xcc" * 64,
                      base_address=VA, image_size=virtual_size,
                      entry_point=VA, kernel_thunk_addr=0, sections=[sec])
    return img, sec


def test_instruction_crossing_raw_end_decodes():
    img, sec = _image(TAIL, virtual_size=len(TAIL) + 8)
    engine = DisasmEngine(img)
    engine.linear_sweep(sec)
    ret = engine.instructions.get(VA + 6)
    assert ret is not None and ret.is_ret, "ret 0x10 straddling raw end must decode"
    assert ret.size == 3


def test_no_instruction_starts_in_the_fill():
    # BLiNX 2's XGRPH ends in data that decodes as `ss: add [eax], al`
    # across raw end; finishing it is fine, decoding the fill after it is not.
    img, sec = _image(TAIL, virtual_size=len(TAIL) + 0x40)
    engine = DisasmEngine(img)
    engine.linear_sweep(sec)
    engine.decode_at(VA + len(TAIL) + 2)
    assert max(engine.instructions) < VA + len(TAIL)
    # The first fill byte is the boundary: nothing decodes there either.
    assert engine.decode_at(VA + len(TAIL)) == 0
    assert max(engine.instructions) < VA + len(TAIL)


def test_fill_is_zeros_not_the_next_file_bytes():
    img, sec = _image(TAIL, virtual_size=len(TAIL) + 8)
    data = img.get_section_data(sec)
    assert data[len(TAIL):] == bytes(8)


def test_fill_is_bounded_to_one_instruction():
    img, sec = _image(TAIL, virtual_size=len(TAIL) + 0x1000)
    assert len(img.get_section_data(sec)) == len(TAIL) + 14


def test_data_sections_keep_their_file_bytes():
    img, sec = _image(TAIL, virtual_size=len(TAIL) + 8, executable=False)
    assert img.get_section_data(sec) == TAIL


def test_code_section_without_a_virtual_tail_gets_no_fill():
    # virtual_size <= raw_size: the file bytes, and nothing after them.
    for virtual_size in (len(TAIL), len(TAIL) - 2):
        img, sec = _image(TAIL, virtual_size=virtual_size)
        assert img.get_section_data(sec) == TAIL


def test_bss_only_code_section_has_no_bytes():
    img, sec = _image(b"", virtual_size=0x40)
    assert img.get_section_data(sec) == b""


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("ok")
