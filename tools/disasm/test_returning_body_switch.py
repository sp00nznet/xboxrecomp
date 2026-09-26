"""A body that opens with a switch dispatch still reaches its ret.

probes_as_returning_body ended the probe at any unconditional jmp that was not
an immediate forward branch, so every `jmp [reg*4 + table]` looked like a tail
call. A function reached only as an immediate (`mov eax, offset fn`), with no
prologue and packed right after the previous function's ret, that opens with
such a dispatch was then refused by the imm-ref pass, and nothing else finds
it: the cc-boundary pass needs int3 padding and the gap-prologue pass needs a
prologue. Its switch was translated nowhere, and the gap-prologue pass instead
started functions at its later arms (`mov eax, 1 ; ret` probes as a constant
stub) and at the alignment padding in front of the table, decoding the table
itself as code.

A dispatch through a table the engine has measured continues in its arms, so
the probe follows it there. A dispatch through an unmeasured table is still
tail-call shaped.
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


def _dispatching_body(at):
    """A switch function at `at`, its table right after the last arm."""
    # at+0:  jmp dword ptr [eax*4 + TABLE]      ff 24 85 <TABLE>
    # at+7:  arm0: xor eax, eax ; ret            31 c0 c3
    # at+10: arm1: mov eax, 1 ; ret              b8 01 00 00 00 c3
    # at+16: arm2: mov eax, 2 ; ret              b8 02 00 00 00 c3
    # at+22: npad 2 (mov edi, edi), so the table is dword aligned when `at` is
    # at+24: TABLE: arm0, arm1, arm2
    table = at + 24
    data = b"\xff\x24\x85" + struct.pack("<I", table)
    data += b"\x31\xc0\xc3"
    data += b"\xb8\x01\x00\x00\x00\xc3"
    data += b"\xb8\x02\x00\x00\x00\xc3"
    data += b"\x8b\xff"
    data += struct.pack("<III", at + 7, at + 10, at + 16)
    return data, table


def test_a_dispatch_through_a_measured_table_reaches_its_arms():
    data, table = _dispatching_body(BASE)
    engine, _ = _engine(data + b"\xcc" * 16)
    assert table in engine.jump_tables
    assert engine.probes_as_returning_body(BASE)


def test_a_dispatch_through_an_unknown_table_is_still_tail_shaped():
    # Same bytes, but the jmp names a table the engine never measured: nothing
    # says where the arms are, so the old answer stands.
    data, _ = _dispatching_body(BASE)
    data = data[:3] + struct.pack("<I", BASE + 0x100) + data[7:]
    engine, _ = _engine(data + b"\xcc" * 16)
    assert not engine.probes_as_returning_body(BASE)


def test_an_immediate_that_names_a_switch_function_becomes_a_function():
    # BASE+0:  push ebp ; mov ebp, esp ; mov eax, offset FN ; pop ebp ; ret
    # BASE+10: FN, packed straight after the ret -- no int3, no prologue
    caller = b"\x55\x8b\xec\xb8" + b"\0\0\0\0" + b"\x5d\xc3"
    fn = BASE + len(caller)
    caller = caller[:4] + struct.pack("<I", fn) + caller[8:]
    body, table = _dispatching_body(fn)
    data = caller + body + b"\xcc" * 16
    engine, image = _engine(data)
    assert table in engine.jump_tables
    det = FunctionDetector(engine, image, XRefTracker(), LabelManager())
    det.detect_all([image.section])
    assert fn in det.functions, sorted(hex(a) for a in det.functions)
    # ...and none of its arms was mistaken for a function of its own.
    for arm in (fn + 7, fn + 10, fn + 16):
        assert arm not in det.functions, hex(arm)
