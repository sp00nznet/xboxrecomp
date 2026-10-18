"""A function packed straight after another function's switch table.

MSVC parks a switch table after the function's last ret, aligns it with
`mov edi, edi`, and the linker then packs the next function right after the
table. Nothing marks that start. The int3 boundary pass finds a table, not a
ret, in front of it, and the gap-prologue pass looks only after a ret (and,
since #182, skips the `mov edi, edi` padding in front of a table rather than
taking it for a hot-patch prologue; test_padding_in_front_of_a_table_is_not_a
_prologue keeps that covered here).

Fixtures are synthetic: hand-assembled bytes, no game data.
"""
import os
import struct
import sys
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.engine import DisasmEngine  # noqa: E402
from tools.disasm.functions import Function, FunctionDetector  # noqa: E402
from tools.disasm.labels import LabelManager  # noqa: E402
from tools.disasm.xrefs import XRefTracker  # noqa: E402

BASE = 0x10000
PAD = b"\x8b\xff"                       # mov edi, edi


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


def _detect(data):
    engine, image = _engine(data)
    det = FunctionDetector(engine, image, XRefTracker(), LabelManager())
    det.detect_all([image.section])
    return det, engine


def _arm(value):
    """mov eax, value ; pop ebp ; ret -- 7 bytes"""
    return b"\xb8" + struct.pack("<I", value) + b"\x5d\xc3"


# The follower: frameless, no prologue the gap pass knows, reached by nothing.
#   mov edx, [esp+4] ; mov eax, [edx] ; add eax, 1 ; ret
FOLLOWER = b"\x8b\x54\x24\x04" + b"\x8b\x02" + b"\x83\xc0\x01" + b"\xc3"


def _owner_with_table(index_table=b"", n_arms=4, bound=None, bias=0,
                      align_after_index=True, follower_code=FOLLOWER):
    """A switch function at BASE, its table (and optional index bytes) after
    the last ret, then the follower. Returns (data, table, follower address,
    padding address).

        BASE+0:  push ebp ; mov ebp, esp
        BASE+3:  mov eax, [ebp+8]
                 nop x k                     so the table is dword aligned
                 [cmp eax, bound ; ja arm0]  when bound is given
                 [movzx eax, byte ptr [eax + INDEX - bias]]   with index bytes
                 jmp dword ptr [eax*4 + TABLE]
                 arm0 .. armN                7 bytes each
                 mov edi, edi                padding, 2 bytes past a dword
                 TABLE: n_arms dwords
                 [INDEX bytes ; [int3 to a 16-byte boundary]]
                 follower
    """
    head = b"\x55\x8b\xec" + b"\x8b\x45\x08"
    check = b"\x83\xf8" + bytes([bound]) + b"\x77" if bound is not None else b""
    load = 7 if index_table else 0
    before_pad = len(head) + len(check) + (1 if check else 0) + load + 7 \
        + 7 * n_arms
    head += b"\x90" * ((2 - (BASE + before_pad)) % 4)
    dispatch_at = BASE + len(head) + len(check) + (1 if check else 0) + load
    arms_at = dispatch_at + 7
    arms = [arms_at + 7 * i for i in range(n_arms)]
    pad_at = arms_at + 7 * n_arms
    table = pad_at + len(PAD)
    assert table % 4 == 0
    index_at = table + 4 * n_arms
    tail = index_table
    if index_table and align_after_index:
        tail += b"\xcc" * ((-(index_at + len(index_table))) % 16)
    follower = index_at + len(tail)

    code = head
    if check:
        # ja rel8 to arm0, just past the load and the dispatch
        code += check + bytes([load + 7])
    if index_table:
        code += b"\x0f\xb6\x80" + struct.pack("<I", index_at - bias)
    code += b"\xff\x24\x85" + struct.pack("<I", table)
    assert BASE + len(code) == arms_at
    code += b"".join(_arm(i) for i in range(n_arms))
    code += PAD
    code += struct.pack("<%dI" % n_arms, *arms)
    code += tail
    assert BASE + len(code) == follower
    code += follower_code + b"\xcc" * 16
    return code, table, follower, pad_at


def test_function_after_a_switch_table_is_found():
    data, table, follower, pad = _owner_with_table()
    det, engine = _detect(data)
    assert table in engine.jump_tables
    assert follower in det.functions, sorted(hex(a) for a in det.functions)
    assert det.functions[follower].detection_method == "jump_table_follower"
    assert det.functions[follower].end == follower + len(FOLLOWER)


def test_padding_in_front_of_a_table_is_not_a_prologue():
    # The old failure: a function on the `mov edi, edi`, swallowing the table
    # and the follower behind it.
    data, _, follower, pad = _owner_with_table()
    det, _ = _detect(data)
    assert pad not in det.functions, hex(pad)
    owner = det.functions[BASE]
    assert owner.end <= pad, hex(owner.end)


def test_a_two_level_switch_index_table_is_stepped_over():
    # movzx eax, byte ptr [eax + INDEX] ; jmp [eax*4 + TABLE]. The index bytes
    # are small arm numbers, decode as `add`, and probe as a body easily.
    index = bytes([0, 1, 1, 2, 0, 3, 3, 3, 2, 1, 0])
    data, table, follower, _ = _owner_with_table(index)
    det, engine = _detect(data)
    index_at = table + 16
    assert index_at in engine.byte_tables
    assert follower in det.functions, sorted(hex(a) for a in det.functions)
    for addr in range(index_at, index_at + len(index)):
        assert addr not in det.functions, hex(addr)


def test_a_switch_arm_after_its_table_is_not_a_follower():
    # MSVC's memcpy parks arms straight after a table. When the owner was
    # measured short, the arm sits in a gap, but starting a function there
    # would clamp the owner: it is the owner's code.
    #   BASE+0: jmp [eax*4 + TABLE] ; ret ; mov edi, edi
    #   TABLE:  3 dwords, the last pointing just past the table
    #   ARM:    xor eax, eax ; ret
    table = BASE + 10
    arm = table + 12
    data = b"\xff\x24\x85" + struct.pack("<I", table) + b"\xc3" + PAD
    data += struct.pack("<III", BASE + 7, BASE + 7, arm)
    data += b"\x31\xc0\xc3" + b"\xcc" * 16
    engine, image = _engine(data)
    assert table in engine.jump_tables
    det = FunctionDetector(engine, image, XRefTracker(), LabelManager())
    det.functions = {BASE: Function(BASE, BASE + 8, "sub_owner")}
    assert not det._pass_jump_table_followers([image.section])
    assert arm not in det._candidates


def test_a_table_inside_a_function_has_no_follower():
    # When the owning body covers the bytes after the table, they are its own
    # code, not a new function.
    data, _, follower, _ = _owner_with_table()
    engine, image = _engine(data)
    det = FunctionDetector(engine, image, XRefTracker(), LabelManager())
    det.functions = {BASE: Function(BASE, follower + len(FOLLOWER), "sub_owner")}
    assert not det._pass_jump_table_followers([image.section])


# --- index tables in front of the follower -----------------------------------

# A framed follower: push ebp ; mov ebp, esp ; mov eax, [ebp+8] ; pop ebp ; ret
FRAMED = b"\x55\x8b\xec\x8b\x45\x08\x5d\xc3"


def _big_index(n_arms, count=37):
    return bytes(i % n_arms for i in range(count))


def test_a_large_switch_index_table_is_sized_by_its_bound_check():
    # 96 arms: index bytes run up to 0x24 here, and the follower packed
    # straight after them opens with 0x55, which is below 96 too. The
    # `cmp eax, N; ja` in front of the load says where the table ends.
    index = _big_index(0x60)
    data, table, follower, _ = _owner_with_table(
        index, n_arms=0x60, bound=len(index) - 1, align_after_index=False,
        follower_code=FRAMED)
    det, engine = _detect(data)
    assert engine.index_table_length(table + 4 * 0x60) == len(index)
    assert follower in det.functions, sorted(hex(a) for a in det.functions)
    assert follower + 1 not in det.functions


def test_a_large_switch_without_a_bound_stops_at_a_frame():
    # No bound check to read. The walk over bytes below 96 would eat the
    # follower's `push ebp` and start it a byte late, without its frame.
    index = _big_index(0x60)
    data, table, follower, _ = _owner_with_table(
        index, n_arms=0x60, align_after_index=False, follower_code=FRAMED)
    det, _ = _detect(data)
    assert follower + 1 not in det.functions
    assert follower in det.functions, sorted(hex(a) for a in det.functions)


def test_a_large_switch_without_a_bound_or_padding_is_refused():
    # Frameless follower packed straight after the index bytes: nothing says
    # where the table ends, so nothing is started at all.
    index = _big_index(0x60)
    data, table, follower, _ = _owner_with_table(
        index, n_arms=0x60, align_after_index=False)
    det, _ = _detect(data)
    index_at = table + 4 * 0x60
    for addr in range(index_at, follower + len(FOLLOWER)):
        assert addr not in det.functions, hex(addr)


def test_an_unnamed_small_index_table_is_stepped_over():
    # The lowest case is 2, so the load names INDEX - 2 and nothing names
    # the table itself. Two bytes below `arms` (4) still mark it as one.
    index = bytes([0, 1, 1, 2, 0, 3, 3, 3, 2, 1, 0])
    data, table, follower, _ = _owner_with_table(index, bias=2)
    det, engine = _detect(data)
    index_at = table + 16
    assert index_at not in engine.byte_tables
    assert follower in det.functions, sorted(hex(a) for a in det.functions)
    for addr in range(index_at, index_at + len(index)):
        assert addr not in det.functions, hex(addr)


def test_bytes_no_function_starts_with_are_refused():
    # After the table, bytes that are no index table (5 >= 4 arms) but are
    # `add` opcodes all the same: data of some other kind, not a function.
    junk = b"\x05\x05\x05\x05\x05\xc3"
    data, table, follower, _ = _owner_with_table(follower_code=junk)
    det, _ = _detect(data)
    assert follower not in det.functions


def test_a_decode_from_before_the_table_vetoes_the_follower():
    # When an instruction that starts before the table runs across the
    # follower's address, that address is the middle of an instruction. A
    # later decode_at can lay one there; a 3-entry table is short enough for
    # a 15-byte instruction to span it.
    from tools.disasm.engine import Instruction
    data, table, follower, _ = _owner_with_table(n_arms=3)
    assert follower == table + 12
    engine, image = _engine(data)
    assert table in engine.jump_tables
    engine.instructions[table - 1] = Instruction(
        address=table - 1, size=15, mnemonic="mov", op_str="eax, 0",
        bytes_hex="")
    engine._sorted_addrs = None
    det = FunctionDetector(engine, image, XRefTracker(), LabelManager())
    det.functions = {BASE: Function(BASE, table - 2, "sub_owner")}
    assert not det._pass_jump_table_followers([image.section])
