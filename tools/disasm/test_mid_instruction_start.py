"""A start inside an instruction of the body before it is no start.

BLiNX 2's sub_001CDFB0 dispatches through a two-entry arm table at
0x001CE000. resync_jump_tables leaves tables under three entries in the
stream, so the sweep decoded the second entry, 0x001CDFE8 (`e8 df 1c 00 ..`),
as `call 0x001CFCE8`. That address is the middle of
`mov dword ptr [0xEC5340], 2` in sub_001CFCA0. The phantom call target ended
sub_001CFCA0 there, the lifted body ran off its end without its ret, and
every caller came back with esp four bytes low: the second Tom Tom Gang rank
exam hung on it.

A weak start (a call or jump target, an alias) that the body in front of it
decodes straight across is refused; one the bytes vouch for (a prologue) or a
seed names stays, and so does one crossed only by bytes the walk reached past
a table or a terminator. When the crossing body rests on weaker evidence than
the start, the start stays if real code calls it.
"""
import os
import struct
import sys
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.engine import DisasmEngine  # noqa: E402
from tools.disasm.functions import FunctionDetector  # noqa: E402
from tools.disasm.labels import LabelManager  # noqa: E402
from tools.disasm.xrefs import XRefTracker  # noqa: E402

BASE = 0x10000


class Image:
    base_address = BASE
    entry_point = BASE

    def __init__(self, data):
        self.data = data
        self.image_size = len(data)
        self.section = SimpleNamespace(name=".text", virtual_addr=BASE,
                                       virtual_size=len(data), executable=True)
        self.sections = [self.section]

    def get_section_at_va(self, addr):
        return self.section if BASE <= addr < BASE + len(self.data) else None

    def get_section_data(self, section):
        return self.data

    def read_bytes_at_va(self, addr, size):
        return self.data[addr - BASE:addr - BASE + size]

    def read_u32_at_va(self, addr):
        off = addr - BASE
        if off < 0 or off + 4 > len(self.data):
            return None
        return struct.unpack_from("<I", self.data, off)[0]


def _engine(data):
    image = Image(data)
    engine = DisasmEngine(image)
    engine.linear_sweep(image.section)
    engine.resync_jump_tables()
    return engine, image


def _straddled(prologue):
    """F stores a dword; G calls into the middle of F's store."""
    # F+0:  push ebp ; mov ebp, esp                     55 8b ec
    # F+3:  mov dword ptr [0x00012000], IMM             c7 05 <addr> <imm>
    # F+13: pop ebp ; ret                               5d c3
    # F+15: cc
    # G:    push ebp ; mov ebp, esp ; call F+9 ; pop ebp ; ret
    # IMM spells a prologue (55 8b ec ..) when `prologue` is set.
    imm = b"\x55\x8b\xec\x90" if prologue else b"\x02\x00\x00\x00"
    f = b"\x55\x8b\xec" + b"\xc7\x05" + struct.pack("<I", BASE + 0x2000)
    f += imm + b"\x5d\xc3" + b"\xcc"
    g_at = BASE + len(f)
    inside = BASE + 9
    g = b"\x55\x8b\xec" + b"\xe8" + struct.pack("<i", inside - (g_at + 8))
    g += b"\x5d\xc3"
    return f + g + b"\xcc" * 16, inside


def _detect(data):
    engine, image = _engine(data)
    det = FunctionDetector(engine, image, XRefTracker(), LabelManager())
    det.detect_all([image.section])
    return det


def test_a_call_target_inside_an_instruction_is_refused():
    data, inside = _straddled(prologue=False)
    det = _detect(data)
    assert BASE in det.functions
    assert inside not in det.functions
    assert det.functions[BASE].end == BASE + 15
    assert det.mid_instruction_starts.get(inside) is not None


def test_a_start_that_decodes_as_a_prologue_stays():
    data, inside = _straddled(prologue=True)
    det = _detect(data)
    assert inside in det.functions
    assert inside not in det.mid_instruction_starts


def _store(imm):
    """mov dword ptr [BASE+0x2000], imm (c7 05 <addr> <imm>), 10 bytes."""
    return b"\xc7\x05" + struct.pack("<I", BASE + 0x2000) + imm


def _caller(at, targets):
    """push ebp ; mov ebp, esp ; call each target ; pop ebp ; ret"""
    code = b"\x55\x8b\xec"
    for target in targets:
        code += b"\xe8" + struct.pack("<i", target - (at + len(code) + 5))
    return code + b"\x5d\xc3"


def test_a_crossing_past_a_terminator_keeps_the_start():
    # F+0:  push ebp ; mov ebp, esp                     55 8b ec
    # F+3:  jz F+12                                     74 07
    # F+5:  pop ebp ; ret                               5d c3
    # F+7:  bytes the walk reaches only past that ret   c7 05 <addr> <imm>
    # The jz keeps the walk going past the ret, but nothing lands on F+7,
    # so the store there may be data that decodes. A start inside it holds.
    f = b"\x55\x8b\xec" + b"\x74\x07" + b"\x5d\xc3" + _store(b"\x02\0\0\0")
    f += b"\xcc"
    inside = BASE + 13
    data = f + _caller(BASE + len(f), [inside]) + b"\xcc" * 16
    det = _detect(data)
    assert inside in det.functions
    assert inside not in det.mid_instruction_starts


def test_a_crossing_past_a_jump_table_keeps_the_start():
    # F+0:  push ebp ; mov ebp, esp                     55 8b ec
    # F+3:  jmp dword ptr [eax*4 + F+12]                ff 24 85 <F+12>
    # F+10: npad 2                                      8b ff
    # F+12: three arms, all at F+34
    # F+24: bytes after the table before any arm        c7 05 <addr> <imm>
    # F+34: arm: pop ebp ; ret                          5d c3
    arm = BASE + 34
    f = b"\x55\x8b\xec" + b"\xff\x24\x85" + struct.pack("<I", BASE + 12)
    f += b"\x8b\xff" + struct.pack("<III", arm, arm, arm)
    f += _store(b"\x02\0\0\0") + b"\x5d\xc3" + b"\xcc"
    inside = BASE + 30
    data = f + _caller(BASE + len(f), [inside]) + b"\xcc" * 16
    det = _detect(data)
    assert inside in det.functions
    assert inside not in det.mid_instruction_starts


def test_a_seeded_start_inside_an_instruction_stays():
    data, inside = _straddled(prologue=False)
    engine, image = _engine(data)
    det = FunctionDetector(engine, image, XRefTracker(), LabelManager())
    det._add_candidate(inside, 0.95, "seed_vtable_thunk")
    det.detect_all([image.section])
    assert inside in det.functions
    assert inside not in det.mid_instruction_starts


def test_two_crossed_starts_in_a_row_are_both_refused():
    # F: push ebp ; mov ebp, esp ; two stores ; pop ebp ; ret ; cc
    # G calls into the immediate of each store.
    f = b"\x55\x8b\xec" + _store(b"\x02\0\0\0") + _store(b"\x03\0\0\0")
    f += b"\x5d\xc3\xcc"
    first, second = BASE + 9, BASE + 19
    data = f + _caller(BASE + len(f), [first, second]) + b"\xcc" * 16
    det = _detect(data)
    assert det.functions[BASE].end == BASE + 25
    assert first in det.mid_instruction_starts
    assert second in det.mid_instruction_starts
    assert first not in det.functions and second not in det.functions


def test_a_refused_start_is_not_added_back_by_a_weak_pass():
    data, inside = _straddled(prologue=False)
    det = _detect(data)
    det._add_candidate(inside, 0.90, "call_target")
    det._add_candidate(inside, 0.88, "tail_jump_target")
    assert inside not in det._candidates
    det._add_candidate(inside, 0.95, "gap_prologue")
    assert det._candidates[inside] == (0.95, "gap_prologue")


def test_a_called_start_crossed_by_a_weaker_body_stays():
    # H: push ebp ; mov ebp, esp ; pop ebp ; ret ; cc cc cc
    # A: one byte (b8) the cc-boundary pass takes as a start; the sweep reads
    #    b8 8b 44 24 04 as `mov eax, imm` across S.
    # S: an FPO function, mov eax, [esp+4] ; ret, called from G.
    h = b"\x55\x8b\xec\x5d\xc3" + b"\xcc" * 3
    a_at = BASE + len(h)
    s_at = a_at + 1
    body = h + b"\xb8" + b"\x8b\x44\x24\x04\xc3" + b"\xcc" * 2
    data = body + _caller(BASE + len(body), [s_at]) + b"\xcc" * 16
    det = _detect(data)
    assert det._candidates.get(a_at, (0, ""))[1] == "cc_boundary"
    assert s_at in det.functions
    assert s_at not in det.mid_instruction_starts
