"""MSVC's virtual-call thunk is a function, and nothing else was finding it.

    mov eax, [ecx]              8B 01
    jmp dword ptr [eax + N]     FF A0 <disp32>

The compiler emits these for pointers-to-virtual-member-functions and for
interface forwarding. They exist only as values in tables, they end in an
indirect tail jump so probes_as_returning_body rejects them, and they are
packed back to back with no int3 between them so the padding boundary pass
never sees them either.

Half-Life 2 has 568 and was finding 41. Each missed one is an indirect call
the runtime cannot resolve, so it is skipped rather than made.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32  # noqa: E402

from tools.disasm.engine import DisasmEngine  # noqa: E402


class _Section:
    def __init__(self, va, size, executable=True):
        self.virtual_addr = va
        self.virtual_size = size
        self.executable = executable


class _Image:
    def __init__(self, va, data, executable=True):
        self.va = va
        self.data = data
        self.section = _Section(va, len(data), executable)

    def get_section_at_va(self, addr):
        if self.va <= addr < self.va + len(self.data):
            return self.section
        return None

    def read_bytes_at_va(self, addr, n):
        off = addr - self.va
        if off < 0 or off >= len(self.data):
            return b""
        return self.data[off:off + n]


def _engine(data, va=0x00583BE2, executable=True):
    eng = DisasmEngine.__new__(DisasmEngine)
    eng.image = _Image(va, data, executable)
    eng._cs = Cs(CS_ARCH_X86, CS_MODE_32)
    eng._cs.detail = True
    return eng, va


THUNK = bytes.fromhex("8b01") + bytes.fromhex("ffa03c020000")   # jmp [eax+0x23c]
THUNK_SHORT = bytes.fromhex("8b01") + bytes.fromhex("ff6010")   # jmp [eax+0x10]


class VcallThunkTest(unittest.TestCase):
    def test_disp32_thunk_is_recognised(self):
        eng, va = _engine(THUNK + b"\x90" * 8)
        self.assertTrue(eng.probes_as_vcall_thunk(va))

    def test_disp8_thunk_is_recognised(self):
        eng, va = _engine(THUNK_SHORT + b"\x90" * 8)
        self.assertTrue(eng.probes_as_vcall_thunk(va))

    def test_back_to_back_thunks_are_each_recognised(self):
        # The real layout: no padding between them.
        eng, va = _engine(THUNK * 4)
        for i in range(4):
            self.assertTrue(eng.probes_as_vcall_thunk(va + i * len(THUNK)),
                            "thunk %d not recognised" % i)

    def test_an_ordinary_function_is_not_a_thunk(self):
        # push ebp; mov ebp, esp; ret
        eng, va = _engine(bytes.fromhex("55") + bytes.fromhex("8bec")
                          + bytes.fromhex("c3") + b"\x90" * 8)
        self.assertFalse(eng.probes_as_vcall_thunk(va))

    def test_a_direct_tail_jump_is_not_a_thunk(self):
        # mov eax, [ecx]; jmp <rel32> -- dispatch has to go through the vtable
        eng, va = _engine(bytes.fromhex("8b01") + bytes.fromhex("e900000000")
                          + b"\x90" * 8)
        self.assertFalse(eng.probes_as_vcall_thunk(va))

    def test_jump_through_a_different_register_is_not_a_thunk(self):
        # mov eax, [ecx]; jmp dword ptr [edx + 0x10] -- not the register loaded
        eng, va = _engine(bytes.fromhex("8b01") + bytes.fromhex("ff6210")
                          + b"\x90" * 8)
        self.assertFalse(eng.probes_as_vcall_thunk(va))

    def test_non_executable_section_is_rejected(self):
        eng, va = _engine(THUNK + b"\x90" * 8, executable=False)
        self.assertFalse(eng.probes_as_vcall_thunk(va))
