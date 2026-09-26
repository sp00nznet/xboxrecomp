import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


class FrameBridgeLifterTest(unittest.TestCase):
    def test_register_indirect_call_snapshots_target(self):
        instruction = Instruction(0, 2, "call", "eax", "ffd0")
        instruction.operands = [Operand(type="reg", reg="eax")]

        lifted = Lifter().lift_instruction(instruction)

        self.assertEqual(len(lifted), 1)
        self.assertIn("uint32_t _icall_target = eax", lifted[0])
        self.assertIn(
            "RECOMP_ICALL_SAFE_AT(_icall_target, _icall_esp, ", lifted[0])

    def test_esp_relative_indirect_call_snapshots_target_before_push(self):
        instruction = Instruction(0, 4, "call", "dword ptr [esp + 0xc]", "ff54240c")
        instruction.operands = [Operand(
            type="mem", mem_base="esp", mem_disp=0xc, mem_size=4)]

        lifted = Lifter().lift_instruction(instruction)

        self.assertEqual(len(lifted), 1)
        self.assertIn("uint32_t _icall_target = MEM32(esp + 0xC)", lifted[0])
        self.assertLess(
            lifted[0].index("uint32_t _icall_target"),
            lifted[0].index("PUSH32(esp,"))
        self.assertIn(
            "RECOMP_ICALL_SAFE_AT(_icall_target, _icall_esp, ", lifted[0])

    def test_call_without_local_ebp_keeps_existing_bridge(self):
        instruction = Instruction(0, 5, "call", "0x123456", "e800000000")
        instruction.call_target = 0x00123456

        lifted = Lifter().lift_instruction(instruction)

        self.assertEqual(len(lifted), 1)
        # RECOMP_ABI_CALL is the direct call: it expands to (fn)() unless
        # -DRECOMP_ABI_CHECK is set.
        self.assertIn("sub_00123456", lifted[0])


if __name__ == "__main__":
    unittest.main()
