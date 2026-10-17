"""A function whose frame __SEH_prolog built still owns that frame.

__SEH_prolog stashes the caller's ebp in the new frame and does
"lea ebp, [esp+0x10]" on its behalf, so the caller has a real frame without
ever writing ebp itself. Classifying those as frameless means the frame is
never re-published across their calls: every callee with a prologue overwrites
g_seh_ebp on entry and nothing restores it, so the next frameless callee --
an MSVC __finally funclet, say -- inherits a frame belonging to a call that
already returned.

Half-Life 2's KeyValues loader is one: sub_005A05E3 gets its frame from
__SEH_prolog, then calls funclets that read the parent's locals through
g_seh_ebp. One of them leaves a critical section via [[ebp-0x2c]+0x580], and
with a dead frame that dereferenced a string out of the file it had just
parsed.
"""
import unittest


class _Insn:
    def __init__(self, mnemonic, op_str="", call_target=None):
        self.mnemonic = mnemonic
        self.op_str = op_str
        self.call_target = call_target


class _Lifter:
    SEH_PROLOG = 0x005B2D78


class _T:
    """Just the two methods under test, without building a real translator."""
    from tools.recomp.translator import FunctionTranslator
    _func_has_prologue = FunctionTranslator._func_has_prologue
    _func_owns_a_frame = FunctionTranslator._func_owns_a_frame
    _func_has_offset_frame = FunctionTranslator._func_has_offset_frame

    def __init__(self):
        self.lifter = _Lifter()


class SehFrameOwnerTest(unittest.TestCase):
    def setUp(self):
        self.t = _T()

    def test_classic_prologue_owns_a_frame(self):
        insns = [_Insn("push", "ebp"), _Insn("mov", "ebp, esp")]
        self.assertTrue(self.t._func_owns_a_frame(insns))

    def test_seh_prologue_call_owns_a_frame(self):
        # push <frame size>; push <scope table>; call __SEH_prolog
        insns = [_Insn("push", "0x13c"), _Insn("push", "0x770858"),
                 _Insn("call", "0x5b2d78", call_target=0x005B2D78)]
        self.assertFalse(self.t._func_has_prologue(insns))
        self.assertTrue(self.t._func_owns_a_frame(insns))

    def test_a_genuinely_frameless_function_does_not(self):
        insns = [_Insn("mov", "eax, dword ptr [esp + 4]"), _Insn("ret")]
        self.assertFalse(self.t._func_owns_a_frame(insns))

    def test_a_call_to_something_else_does_not(self):
        insns = [_Insn("call", "0x401000", call_target=0x00401000)]
        self.assertFalse(self.t._func_owns_a_frame(insns))

    def test_no_seh_prolog_in_the_binary_is_not_a_frame(self):
        self.t.lifter.SEH_PROLOG = None
        insns = [_Insn("call", "0x5b2d78", call_target=0x005B2D78)]
        self.assertFalse(self.t._func_owns_a_frame(insns))

    def test_msvc_offset_frame_owns_a_frame(self):
        # MSVC offset frame: push ebp; lea ebp, [esp - 0x70]
        insns = [_Insn("push", "ebp"), _Insn("lea", "ebp, [esp - 0x70]"),
                 _Insn("sub", "esp, 0x274")]
        self.assertFalse(self.t._func_has_prologue(insns))
        self.assertTrue(self.t._func_owns_a_frame(insns))

    def test_lea_ebp_from_another_register_is_not_a_frame(self):
        insns = [_Insn("push", "ebp"), _Insn("lea", "ebp, [eax + 4]")]
        self.assertFalse(self.t._func_owns_a_frame(insns))
