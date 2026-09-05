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
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.functions import FunctionDetector  # noqa: E402


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
    def get_section_at_va(self, addr):
        return _Section()


class _Engine:
    def __init__(self, insns, prologues):
        self.instructions = {i.address: i for i in insns}
        self._prologues = set(prologues)

    def probes_as_prologue(self, addr):
        return addr in self._prologues


def _detector(insns, functions, prologues):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(insns, prologues)
    det.image = _Image()
    det.functions = {f.start: f for f in functions}
    det._candidates = {}
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

    def test_bytes_that_are_not_a_prologue_are_ignored(self):
        insns = [_Insn(0x00476EAF, 1, is_ret=True)]
        funcs = [_Func(0x00476EA0, 0x00476EB0), _Func(0x004771C0, 0x004771D0)]
        det = _detector(insns, funcs, prologues=set())
        self.assertFalse(det._pass_gap_prologues([]))

    def test_a_non_ret_instruction_starts_nothing(self):
        insns = [_Insn(0x00476EAF, 1, is_ret=False)]
        funcs = [_Func(0x00476EA0, 0x00476EB0), _Func(0x004771C0, 0x004771D0)]
        det = _detector(insns, funcs, prologues={0x00476EB0})
        self.assertFalse(det._pass_gap_prologues([]))

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

