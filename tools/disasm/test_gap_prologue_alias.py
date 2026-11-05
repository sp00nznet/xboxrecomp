"""The past-padding gap sweep leaves alias bodies whole.

A method reached only through a vtable is an alias entry, not a function, when
the sweep runs, so its bytes looked like a gap. Jet Set Radio Future's
0x000304F0 sits after a ret and padding and opens with `sub esp, 0x104`, so the
sweep made it a function start, and the build then cut it at 0x00030570, a weak
immediate-reference start on one of its own switch arms. Three of its four
arms fell outside the body; the dispatch was lifted as an indirect tail jump
that missed at run time and returned with the 0x104-byte frame still on the
stack.

The same blind spot split out-of-line tails that only the alias reaches by a
jcc, turning each branch into a call to a new function.
"""
import struct

from tools.disasm.engine import DisasmEngine
from tools.disasm.functions import FunctionDetector
from tools.disasm.labels import LabelManager
from tools.disasm.loader import BinaryImage, SectionInfo

BASE = 0x10000
RDATA = 0x11000


def _detect(code: bytearray, vtable: int, seeds):
    data = bytes(code) + struct.pack("<I", vtable)
    text = SectionInfo(".text", BASE, len(code), 0, len(code), False, True, "")
    rdata = SectionInfo(".rdata", RDATA, 4, len(code), 4, False, False, "")
    image = BinaryImage("synthetic", data, 0, 0x20000, BASE, 0, [text, rdata])
    engine = DisasmEngine(image)
    engine.linear_sweep(text)
    engine.resync_jump_tables()
    detector = FunctionDetector(engine, image, None, LabelManager())
    for addr in seeds:
        detector._add_candidate(addr, 1.0, "seed")
    detector.detect_all([text])
    return detector


def test_vtable_method_keeps_its_switch_arms():
    code = bytearray(b"\x90" * 0x1000)
    code[0x00] = 0xC3                                   # previous function
    table = BASE + 0x44
    method = (
        bytes.fromhex("81ec04010000")                   # 0x10 sub esp, 0x104
        + bytes.fromhex("83f803")                       # 0x16 cmp eax, 3
        + bytes.fromhex("7719")                         # 0x19 ja arm3
        + bytes.fromhex("ff2485") + struct.pack("<I", table)  # 0x1b dispatch
        + bytes.fromhex("31c0eb13")                     # 0x22 arm0
        + bytes.fromhex("b801000000eb0c")               # 0x26 arm1
        + bytes.fromhex("b802000000eb05")               # 0x2d arm2
        + bytes.fromhex("b803000000")                   # 0x34 arm3
        + bytes.fromhex("81c404010000c20400")           # 0x39 epilogue
        + bytes.fromhex("8bff"))                        # 0x42 npad 2
    code[0x10:0x10 + len(method)] = method
    arms = (0x22, 0x26, 0x2D, 0x34)
    code[0x44:0x54] = b"".join(struct.pack("<I", BASE + a) for a in arms)
    # Elsewhere, arm1's address taken as an immediate: a weak start.
    code[0x60:0x66] = bytes.fromhex("b826000100c3")
    code[0x70] = 0xC3

    detector = _detect(code, BASE + 0x10, (BASE, BASE + 0x60, BASE + 0x70))

    fn = detector.functions[BASE + 0x10]
    assert fn.detection_method != "gap_prologue"
    assert fn.end >= BASE + 0x42, "cut before its own switch arms"


def test_alias_keeps_its_out_of_line_tail():
    code = bytearray(b"\x90" * 0x1000)
    code[0x00] = 0xC3                                   # previous function
    code[0x10:0x1C] = bytes.fromhex(
        "83ec08"        # 0x10 sub esp, 8
        "85c0"          # 0x13 test eax, eax
        "7509"          # 0x15 jne 0x20
        "83c408"        # 0x17 add esp, 8
        "c3")           # 0x1a ret
    code[0x20:0x29] = bytes.fromhex(
        "b801000000"    # 0x20 mov eax, 1 (past padding, reached only by jne)
        "83c408"        # 0x25 add esp, 8
        "c3")           # 0x28 ret
    code[0x30] = 0xC3

    detector = _detect(code, BASE + 0x10, (BASE, BASE + 0x30))

    assert BASE + 0x20 not in detector.functions, "tail split off"
    assert detector.functions[BASE + 0x10].end >= BASE + 0x29
