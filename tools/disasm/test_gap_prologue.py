"""A function that begins right after a ret, with no padding between.

_pass_cc_boundaries only sees a boundary when the compiler left int3 padding
to the next alignment. When the next function already starts on the boundary
there is no padding, and a clean prologue sits immediately after the previous
function's ret with nothing marking it. It is not a call target if it is only
reached through a vtable, and the prologue pass looks for "push ebp; mov ebp,
esp", which an FPO function like "sub esp, 0x18" does not have.

MSVC parks out-of-line tails after a ret too, and from here they look
identical. The difference is that a tail belongs to the function above it, so
_find_function_end has already covered it -- it is not in a gap. Restricting
to gaps separates the two by construction. Half-Life 2 has 20 of these: 12 in
gaps, 8 tails.
"""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.engine import DisasmEngine  # noqa: E402
from tools.disasm.functions import FunctionDetector  # noqa: E402
from tools.disasm.labels import LabelManager  # noqa: E402
from tools.disasm.test_jump_table_follower import BASE, Image  # noqa: E402
from tools.disasm.xrefs import XRefTracker  # noqa: E402


class _Insn:
    def __init__(self, addr, size, is_ret=False):
        self.address = addr
        self.size = size
        self.end_address = addr + size
        self.is_ret = is_ret


class _Section:
    executable = True


class _Func:
    def __init__(self, start, end):
        self.start = start
        self.end = end


class _Image:
    bytes_at = {}

    def get_section_at_va(self, addr):
        return _Section()

    def read_bytes_at_va(self, addr, n):
        return bytes(self.bytes_at.get(addr + i, 0) for i in range(n))


class _Engine:
    jump_tables = {}
    entries = {}

    def jump_table_entries(self, tbl):
        return self.entries.get(tbl, [])

    def get_instruction(self, addr):
        return self.instructions.get(addr)

    def __init__(self, insns, prologues):
        self.instructions = {i.address: i for i in insns}
        self._prologues = set(prologues)
        self._stub = False
        self.jump_tables = {}

    def probes_as_prologue(self, addr):
        return addr in self._prologues

    def probes_as_constant_stub(self, addr):
        return self._stub

    bodies = ()

    def probes_as_callback_body(self, addr, upper, **kw):
        return addr in self.bodies


def _detector(insns, functions, prologues):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(insns, prologues)
    det.image = _Image()
    det.functions = {f.start: f for f in functions}
    det._candidates = {}
    det._alias_entries = {}
    det.added = []
    det._add_candidate = lambda addr, conf, why: det.added.append((addr, why))
    return det


class GapPrologueTest(unittest.TestCase):
    def test_prologue_in_a_gap_after_a_ret_is_a_function(self):
        # sub_00476EA0 ends with a ret at 0x00476EAF; 0x00476EB0 is unclaimed.
        insns = [_Insn(0x00476EAF, 1, is_ret=True)]
        funcs = [_Func(0x00476EA0, 0x00476EB0), _Func(0x004771C0, 0x004771D0)]
        det = _detector(insns, funcs, prologues={0x00476EB0})
        self.assertTrue(det._pass_gap_prologues([]))
        self.assertIn((0x00476EB0, "gap_prologue"), det.added)

    def test_an_out_of_line_tail_is_not_split(self):
        # Same shape, but the enclosing function's body already covers it.
        insns = [_Insn(0x00476EAF, 1, is_ret=True)]
        funcs = [_Func(0x00476EA0, 0x00476F00)]
        det = _detector(insns, funcs, prologues={0x00476EB0})
        self.assertFalse(det._pass_gap_prologues([]))
        self.assertEqual(det.added, [])

    def test_bytes_that_are_neither_a_prologue_nor_a_stub_are_ignored(self):
        insns = [_Insn(0x00476EAF, 1, is_ret=True)]
        funcs = [_Func(0x00476EA0, 0x00476EB0), _Func(0x004771C0, 0x004771D0)]
        det = _detector(insns, funcs, prologues=set())
        det.engine._stub = False
        self.assertFalse(det._pass_gap_prologues([]))

    def test_a_constant_stub_counts_even_without_a_prologue(self):
        """"mov eax, <imm32>; ret" has no frame to recognise."""
        insns = [_Insn(0x00476EAF, 1, is_ret=True)]
        funcs = [_Func(0x00476EA0, 0x00476EB0), _Func(0x004771C0, 0x004771D0)]
        det = _detector(insns, funcs, prologues=set())
        det.engine._stub = True
        self.assertTrue(det._pass_gap_prologues([]))
        self.assertIn((0x00476EB0, "gap_prologue"), det.added)

    def test_a_constant_stub_inside_a_function_is_still_not_split(self):
        # The same two instructions occur as a return path in larger
        # functions; the gap test is what keeps them intact.
        insns = [_Insn(0x00476EAF, 1, is_ret=True)]
        funcs = [_Func(0x00476EA0, 0x00476F00)]
        det = _detector(insns, funcs, prologues=set())
        det.engine._stub = True
        self.assertFalse(det._pass_gap_prologues([]))

    def test_padding_before_a_jump_table_is_not_a_function(self):
        insns = [_Insn(0x1000, 1, is_ret=True), _Insn(0x1001, 2)]
        det = _detector(insns, [_Func(0x1000, 0x1001)], {0x1001})
        det.engine.jump_tables = {0x1003: 0x1013}
        self.assertFalse(det._pass_gap_prologues([]))
        self.assertEqual(det.added, [])

    def test_a_non_ret_instruction_starts_nothing(self):
        insns = [_Insn(0x00476EAF, 1, is_ret=False)]
        funcs = [_Func(0x00476EA0, 0x00476EB0), _Func(0x004771C0, 0x004771D0)]
        det = _detector(insns, funcs, prologues={0x00476EB0})
        self.assertFalse(det._pass_gap_prologues([]))

    # Panzer Dragoon Orta pads with nop, not int3:
    #
    #     0x0026B629  ret 0xc
    #     0x0026B62C  nop x4
    #     0x0026B630  mov ecx, [esp+4]   <- a COM forwarder, vtable-only
    def _padded(self, ret_end, start):
        insns = [_Insn(ret_end - 3, 3, is_ret=True)]
        funcs = [_Func(ret_end - 0x20, ret_end), _Func(0x0026B690, 0x0026B6A0)]
        det = _detector(insns, funcs, prologues={start})
        det.image.bytes_at = {a: 0x90 for a in range(ret_end, start)}
        return det

    def test_a_start_past_nop_padding_is_a_function(self):
        det = self._padded(0x0026B62C, 0x0026B630)
        self.assertTrue(det._pass_gap_prologues([]))
        self.assertEqual(det.added, [(0x0026B630, "gap_prologue")])

    def test_a_closed_body_past_nop_padding_is_a_function(self):
        # The forwarder ends `jmp [ecx+0x18]`: no prologue, no constant stub.
        det = self._padded(0x0026B62C, 0x0026B630)
        det.engine._prologues = set()
        det.engine.bodies = {0x0026B630}
        self.assertTrue(det._pass_gap_prologues([]))
        self.assertEqual(det.added, [(0x0026B630, "gap_prologue")])

    def test_a_closed_body_with_no_padding_in_front_is_not_enough(self):
        det = self._padded(0x0026B630, 0x0026B630)
        det.engine._prologues = set()
        det.engine.bodies = {0x0026B630}
        self.assertFalse(det._pass_gap_prologues([]))

    def test_nops_that_reach_no_alignment_are_not_padding(self):
        det = self._padded(0x0026B62C, 0x0026B62E)
        self.assertFalse(det._pass_gap_prologues([]))

    # SSX 3's memcpy, whose tail-copy switch (table 0x00213BCC) keeps its
    # arms after the table, each ending in a ret and padded to the next:
    #
    #     0x00213BE4  mov al, [esi]; ...; leave; ret
    #     0x00213BEF  nop
    #     0x00213BF0  mov al, [esi]; ...; leave; ret   <- arm 3, closed body
    #     0x00213C04  mov al, [esi]; ...                <- arm 4
    #
    # Started as a function, arm 3 clamped memcpy at 0x00213BEF and left
    # arm 4 owned by nothing: a three-byte copy jumped to an unresolved
    # address and the game crashed entering Single Event.
    def test_a_switch_arm_past_padding_is_not_a_function(self):
        det = self._padded(0x00213BEF, 0x00213BF0)
        det.engine._prologues = set()
        det.engine.bodies = {0x00213BF0}
        det.engine.jump_tables = {0x00213BCC: 0x00213BDC}
        det.engine.entries = {
            0x00213BCC: [0x00213BDC, 0x00213BE4, 0x00213BF0, 0x00213C04]}
        self.assertFalse(det._pass_gap_prologues([]))
        self.assertEqual(det.added, [])

    def test_a_switch_arm_straight_after_a_ret_is_not_a_function(self):
        insns = [_Insn(0x0001003F, 1, is_ret=True)]
        funcs = [_Func(0x00010030, 0x00010040), _Func(0x00010100, 0x00010110)]
        det = _detector(insns, funcs, prologues={0x00010040})
        det.engine.jump_tables = {0x00010010: 0x00010018}
        det.engine.entries = {0x00010010: [0x00010020, 0x00010040]}
        self.assertFalse(det._pass_gap_prologues([]))


# Hand-assembled, no game data; the shape mirrors SSX 3's memcpy. The engine
# measures only the forward tail table: the backward-indexed site reads its
# first slot at arm code, so it never anchors. The arms are reached from that
# one table alone.
_TAIL = b"\x8b\x45\x08\x5e\x5f\xc9\xc3"     # mov eax,[ebp+8]; pop esi; pop edi; leave; ret


def _copy(n):
    """mov al,[esi+i]; mov [edi+i],al for i < n."""
    return b"".join(b"\x8a\x46" + bytes([i]) + b"\x88\x47" + bytes([i])
                    if i else b"\x8a\x06\x88\x07" for i in range(n))


def _memcpy_blob():
    """(code, table, arms): a memcpy whose 0-3 byte tails follow its table."""
    head = (b"\x55\x8b\xec\x57\x56"                    # push ebp; mov ebp,esp; push edi; push esi
            b"\x8b\x75\x0c\x8b\x7d\x08\x8b\x4d\x10"     # mov esi,src; mov edi,dst; mov ecx,n
            b"\x8b\xd1\x83\xe2\x03"                     # mov edx,ecx; and edx,3
            b"\x83\xe9\x04\x73\x07")                    # sub ecx,4; jae forward
    pre = len(head) + 14
    pad = (0xC - (BASE + pre + 2)) % 16                  # the table lands at 0xC mod 16
    table = BASE + pre + pad + 2
    bodies = [_TAIL + b"\x90", _copy(1) + _TAIL + b"\x90",
              _copy(2) + _TAIL + b"\x8d\x49\x00", _copy(3) + _TAIL + b"\x90"]
    arms, at = [], table + 16
    for body in bodies:
        arms.append(at)
        at += len(body)
    code = (head
            + b"\xff\x24\x8d" + struct.pack("<I", arms[0])   # jmp [ecx*4 + arm 0], ecx < 0
            + b"\xff\x24\x95" + struct.pack("<I", table)     # jmp [edx*4 + table]
            + b"\x90" * pad + b"\x8b\xff"
            + struct.pack("<4I", *arms) + b"".join(bodies))
    return code + b"\xcc" * (-(BASE + len(code)) % 16), table, arms


class MemcpySwitchArmsTest(unittest.TestCase):
    """Arms after a table, each closed by a ret, stay with their function.

    The two-byte arm sits on a 16-byte boundary after the one-byte arm's nop,
    which is exactly what the gap sweep takes for a new function.
    """

    def test_no_arm_after_the_table_is_split_off(self):
        code, table, arms = _memcpy_blob()
        self.assertEqual(arms[2] % 16, 0)
        image = Image(code)
        engine = DisasmEngine(image)
        engine.linear_sweep(image.section)
        engine.resync_jump_tables()
        self.assertIn(table, engine.jump_tables)
        det = FunctionDetector(engine, image, XRefTracker(), LabelManager())
        det.detect_all([image.section])
        self.assertIn(BASE, det.functions)
        self.assertEqual([a for a in arms if a in det.functions], [])


class SehPrologueShapeTest(unittest.TestCase):
    """A function whose frame __SEH_prolog builds has no prologue to find.

        push <frame size>
        push <scope table>
        call __SEH_prolog

    None of the usual shapes match, because the helper does "push ebp; mov
    ebp, esp" on the caller's behalf. Half-Life 2 has one at 0x001F572B
    reached only through a vtable: unresolved, the call was skipped rather
    than made, and the arguments already pushed for it stayed on the stack --
    which shifted the caller's frame so its "pop ebx" restored the wrong slot.
    """
    import os as _os
    import sys as _sys

    def _probe(self, code):
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        from tools.disasm.engine import DisasmEngine

        class _Sec:
            virtual_addr = 0x001F5000
            virtual_size = 0x1000
            executable = True

        class _Img:
            def get_section_at_va(self, addr):
                return _Sec()

            def get_section_data(self, sec):
                return bytes([0x90]) * 0x72B + code

        eng = DisasmEngine.__new__(DisasmEngine)
        eng.image = _Img()
        eng._cs = Cs(CS_ARCH_X86, CS_MODE_32)
        eng._cs.detail = True
        return eng.probes_as_prologue(0x001F572B)

    def test_seh_prologue_is_a_prologue(self):
        # push 0x18 ; push 0x00772760 ; call rel32
        code = bytes.fromhex("6a18") + bytes.fromhex("6860277700")              + bytes.fromhex("e800000000")
        self.assertTrue(self._probe(code))

    def test_two_pushes_without_a_call_are_not(self):
        code = bytes.fromhex("6a18") + bytes.fromhex("6860277700")              + bytes.fromhex("90909090")
        self.assertFalse(self._probe(code))

    def test_a_single_immediate_push_is_not(self):
        code = bytes.fromhex("6a18") + bytes.fromhex("c3") + bytes([0x90]) * 8
        self.assertFalse(self._probe(code))

    def test_chkstk_prologue_is_a_prologue(self):
        # mov eax, 0x8028 ; call rel32 -- Half-Life 2's 0x00426370
        code = bytes.fromhex("b828800000") + bytes.fromhex("e8067a1800")
        self.assertTrue(self._probe(code))

    def test_mov_eax_without_a_call_is_not(self):
        code = bytes.fromhex("b828800000") + bytes.fromhex("c3") + bytes([0x90]) * 8
        self.assertFalse(self._probe(code))
