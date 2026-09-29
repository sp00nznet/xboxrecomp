"""A jump table indexed from its last slot is still a jump table.

MSVC's memmove dispatches its backward tail through `jmp [ecx*4 + LAST]`, the
address of the table's final entry, with the index counting down from there.
resync_jump_tables measured tables forward from the displacement only, so it
saw one entry, dropped the table as too short, and the function ended at it:
every arm and the epilogue after the table were lost, and the runtime then hit
an indirect jump into code no function covered (MechAssault: memcpy at
0x001ED440 ended at 0x001ED6BC instead of 0x001ED77D, and the heap it built
strings in came up garbage).
"""
import struct

from tools.disasm.functions import FunctionDetector
from tools.disasm.labels import LabelManager
from tools.disasm.test_returning_body_switch import BASE, _engine
from tools.disasm.xrefs import XRefTracker

ARMS = b"\x31\xc0\xc3" + b"\xb8\x01\x00\x00\x00\xc3" + b"\xb8\x02\x00\x00\x00\xc3"


def _backward_switch():
    # +0:  jmp dword ptr [ecx*4 + TABLE+8]     ff 24 8d <TABLE+8>
    # +7:  lea ecx, [ecx] ; mov edi, edi        8d 49 00 8b ff   (padding)
    # +12: TABLE: arm0, arm1, arm2
    # +24: arm0: xor eax, eax ; ret             31 c0 c3
    # +27: arm1: mov eax, 1 ; ret               b8 01 00 00 00 c3
    # +33: arm2: mov eax, 2 ; ret               b8 02 00 00 00 c3
    table = BASE + 12
    arms = (BASE + 24, BASE + 27, BASE + 33)
    data = b"\xff\x24\x8d" + struct.pack("<I", table + 8)
    data += b"\x8d\x49\x00\x8b\xff"
    data += struct.pack("<III", *arms) + ARMS
    return data + b"\xcc" * 16, table, arms


def test_table_is_measured_down_from_its_last_slot():
    data, table, arms = _backward_switch()
    engine, _ = _engine(data)
    assert engine.jump_tables.get(table) == table + 12
    assert engine.jump_table_entries(table + 8) == list(arms)


def test_function_keeps_the_arms_after_the_table():
    data, _, arms = _backward_switch()
    engine, image = _engine(data)
    det = FunctionDetector(engine, image, XRefTracker(), LabelManager())
    det.detect_all([image.section])
    fn = det.functions[BASE]
    assert fn.end >= arms[2] + 6, hex(fn.end)


def test_forward_table_right_after_its_jmp_does_not_eat_the_jmp():
    # The displacement's own bytes are the four before a table parked straight
    # after its jmp, and they are an in-section address. They are not a slot.
    table = BASE + 7
    arms = (BASE + 19, BASE + 22, BASE + 28)
    data = b"\xff\x24\x85" + struct.pack("<I", table)
    data += struct.pack("<III", *arms) + ARMS
    engine, _ = _engine(data + b"\xcc" * 16)
    assert engine.jump_tables.get(table) == table + 12
    assert BASE in engine.instructions


def test_an_epilogue_before_a_table_is_not_a_slot():
    # `mov eax, <far in-section address>` ends where the table begins, so its
    # immediate is the dword before it -- the shape `ret N` makes for real
    # (Wreckless lost three epilogues to it). A slot has to point near the
    # arms the table's other slots point at.
    pad = 0x3000
    table = BASE + 5
    arms = (BASE + 17, BASE + 20, BASE + 26)
    data = b"\xb8" + struct.pack("<I", BASE + pad)          # mov eax, far
    data += struct.pack("<III", *arms) + ARMS
    data += b"\xff\x24\x85" + struct.pack("<I", table)      # the dispatch
    data += b"\xcc" * (pad + 16 - len(data))
    engine, _ = _engine(data)
    assert engine.jump_tables.get(table) == table + 12
    assert BASE in engine.instructions
