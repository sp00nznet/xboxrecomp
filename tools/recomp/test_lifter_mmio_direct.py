"""A mov to or from a constant APU register calls the model directly.

DirectSound updates every voice with plain movs to fixed addresses in the
trapped APU window, 7,500-8,000 a second in BLiNX 2's stage 5-1. Trapped, each
costs a Wine exception dispatch under Proton (about 51 us), which put the main
thread under 30 fps. A mov is one access, so it can go straight to the model;
every other form keeps the fault, because the lifter re-reads its memory
operand (flags, read-modify-write) and a device register must see exactly the
accesses the guest made.
"""
import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block

PIO_FREE = 0xFE820010
SET_CURRENT_VOICE = 0xFE8202F8


def _const(addr, size=4, seg=None):
    return Operand(type="mem", mem_disp=addr, mem_size=size, mem_seg=seg)


def _reg(name):
    return Operand(type="reg", reg=name)


def _imm(v):
    return Operand(type="imm", imm=v)


def _lift(mnemonic, *ops):
    insn = Instruction(0, 5, mnemonic, "", "90")
    insn.operands = list(ops)
    lifted, _ = lift_basic_block(
        Lifter(), BasicBlock(start=0, instructions=[insn]))
    return "\n".join(lifted)


class DirectMovTest(unittest.TestCase):
    def test_dword_load_and_store(self):
        self.assertIn("eax = XBOX_MMIO_R32(0xFE820010u);",
                      _lift("mov", _reg("eax"), _const(PIO_FREE)))
        self.assertIn("XBOX_MMIO_W32(0xFE8202F8u, eax);",
                      _lift("mov", _const(SET_CURRENT_VOICE), _reg("eax")))

    def test_immediate_store(self):
        g = _lift("mov", _const(SET_CURRENT_VOICE), _imm(0x1234))
        self.assertIn("XBOX_MMIO_W32(0xFE8202F8u, 0x1234);", g)

    def test_narrow_widths_keep_the_register_merge(self):
        g = _lift("mov", _reg("al"), _const(PIO_FREE, 1))
        self.assertIn("SET_LO8(eax, XBOX_MMIO_R8(0xFE820010u));", g)
        g = _lift("mov", _reg("ax"), _const(PIO_FREE, 2))
        self.assertIn("SET_LO16(eax, XBOX_MMIO_R16(0xFE820010u));", g)
        self.assertIn("XBOX_MMIO_W8(0xFE820010u, LO8(eax));",
                      _lift("mov", _const(PIO_FREE, 1), _reg("al")))
        self.assertIn("XBOX_MMIO_W16(0xFE820010u, LO16(eax));",
                      _lift("mov", _const(PIO_FREE, 2), _reg("ax")))

    def test_no_plain_access_left_for_a_direct_mov(self):
        g = _lift("mov", _reg("eax"), _const(PIO_FREE))
        self.assertNotIn("MEM32(", g)


class StillTrappedTest(unittest.TestCase):
    """Forms that keep the plain MEMxx access and so the fault path."""

    def _assert_trapped(self, g):
        self.assertNotIn("XBOX_MMIO_", g)
        self.assertRegex(g, r"(MEM(8|16|32)|XBOX_PTR)\(0xFE82")

    def test_other_forms_at_a_constant_apu_address(self):
        c = _const(PIO_FREE)
        cases = {
            "or": (c, _imm(1)),
            "add": (c, _reg("eax")),
            "cmp": (c, _imm(0)),
            "test": (c, _imm(0x80)),
            "inc": (c,),
            "xchg": (c, _reg("eax")),
            "lock xadd": (c, _reg("eax")),
            "movzx": (_reg("eax"), _const(PIO_FREE, 1)),
            "push": (c,),
        }
        for m, ops in cases.items():
            with self.subTest(mnemonic=m):
                self._assert_trapped(_lift(m, *ops))

    def test_register_based_and_fs_operands(self):
        based = Operand(type="mem", mem_base="eax", mem_disp=PIO_FREE, mem_size=4)
        g = _lift("mov", _reg("eax"), based)
        self.assertNotIn("XBOX_MMIO_", g)
        g = _lift("mov", _reg("eax"), _const(0xFE820010, seg="fs"))
        self.assertNotIn("XBOX_MMIO_", g)


class RangeEdgeTest(unittest.TestCase):
    def _direct(self, addr, size=4):
        return "XBOX_MMIO_" in _lift("mov", _reg("eax"), _const(addr, size))

    def test_edges(self):
        self.assertFalse(self._direct(0xFE7FFFFC))
        self.assertTrue(self._direct(0xFE800000))
        self.assertTrue(self._direct(0xFE82FFFC))
        # A dword straddling the trap end is partly RAM: leave it alone.
        self.assertFalse(self._direct(0xFE82FFFE))
        self.assertTrue(self._direct(0xFE82FFFE, 2))
        # The GP and EP DSP memories are plain RAM, not trapped.
        self.assertFalse(self._direct(0xFE830000))
        self.assertFalse(self._direct(0xFE83FF10))
        self.assertFalse(self._direct(0xFE85FFFC))


class NegativeDisplacementTest(unittest.TestCase):
    """The disassembler gives addresses above 2 GiB as negative decimals.

    Most of BLiNX 2's APU movs come through this way (`[-25157620]`), which
    a grep for "0xFE8" misses; the lifter must still see them as unsigned.
    """

    def test_negative_operands_convert(self):
        self.assertIn("eax = XBOX_MMIO_R32(0xFE80200Cu);",
                      _lift("mov", _reg("eax"), _const(-25157620)))
        self.assertIn("XBOX_MMIO_W32(0xFE801300u, eax);",
                      _lift("mov", _const(-25160960), _reg("eax")))

    def test_negative_operand_below_the_trap_stays_plain(self):
        # -25165828 is 0xFE7FFFFC, the dword just below the trap.
        g = _lift("mov", _reg("eax"), _const(-25165828))
        self.assertNotIn("XBOX_MMIO_", g)
        self.assertIn("MEM32(", g)


if __name__ == "__main__":
    unittest.main()
