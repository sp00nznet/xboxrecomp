"""Alias entries in a gap end at the first entry past their body, in-section.

An alias that lands in a gap has no enclosing body, so its end used to be the
next function start, measured before the other aliases existed. A run of gap
aliases with no function between them (C++ dynamic initialisers) then each
spanned the whole run, and one at the end of a section ran into the next
section's data. An alias inside a function keeps the enclosing end: that
overlap is what lets a tail jump share the body's epilogue.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.functions import FunctionDetector, Function  # noqa: E402

SEC_LO, SEC_HI = 0x1000, 0x1100


class _Insn:
    def __init__(self, addr, ret=False, jcc=None):
        self.address = addr
        self.end_address = addr + 4
        self.is_ret = ret
        self.is_jump = False
        self.is_cond_jump = jcc is not None
        self.is_branch = jcc is not None
        self.jump_target = jcc
        self.jump_table = None


class _Section:
    name = "TEXT"
    virtual_addr = SEC_LO
    virtual_size = SEC_HI - SEC_LO
    raw_size = SEC_HI - SEC_LO
    executable = True


class _ShortSection(_Section):
    raw_size = 0xC0                     # the rest is an unbacked virtual tail


class _Image:
    section = _Section

    def get_section_at_va(self, addr):
        return self.section() if SEC_LO <= addr < SEC_HI else None

    def get_section_data(self, sec):
        # As BinaryImage: the file bytes, then up to 14 bytes of the
        # loader's zero fill to finish an instruction cut by raw_size.
        fill = max(min(sec.virtual_size - sec.raw_size, 14), 0)
        return bytes(sec.raw_size + fill)


class _Engine:
    jump_tables = {}

    def __init__(self, insns):
        self.instructions = {i.address: i for i in insns}

    def get_instruction(self, addr):
        return self.instructions.get(addr)

    def get_instructions_in_range(self, lo, hi):
        return [i for a, i in sorted(self.instructions.items()) if lo <= a < hi]


class _Labels:
    def get(self, addr):
        return None

    def auto_name_function(self, *args):
        pass


def _built(insns, functions, aliases, section=_Section):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(insns)
    det.image = _Image()
    det.image.section = section
    det.labels = _Labels()
    det.functions = {s: Function(start=s, end=e, name=f"sub_{s:08X}")
                     for s, e in functions}
    det._alias_entries = dict(aliases)
    det._build_alias_entries()
    return {a: det.functions[a].end for a in aliases}


def _code(lo, hi, rets=(), jccs=None):
    jccs = jccs or {}
    return [_Insn(a, ret=a in rets, jcc=jccs.get(a)) for a in range(lo, hi, 4)]


class AliasBoundsTest(unittest.TestCase):
    def test_a_run_of_gap_aliases_does_not_span_the_run(self):
        # Three "insn; ret" initialisers between two functions.
        insns = _code(0x1020, 0x1050, rets={0x1024, 0x1034, 0x1044})
        ends = _built(insns, [(0x1000, 0x1010), (0x1080, 0x1090)],
                      {0x1020: 0x1080, 0x1030: 0x1080, 0x1040: 0x1080})
        self.assertEqual(ends, {0x1020: 0x1030, 0x1030: 0x1040,
                                0x1040: 0x1080})

    def test_an_alias_inside_a_gap_function_does_not_cut_it(self):
        # 0x1020 branches over an early ret to 0x1040; a data-table hit at
        # 0x1024 must not end it before its own branch target.
        insns = _code(0x1020, 0x1054, rets={0x1028, 0x1040, 0x1050},
                      jccs={0x1020: 0x1040})
        ends = _built(insns, [(0x1000, 0x1010), (0x1080, 0x1090)],
                      {0x1020: 0x1080, 0x1024: 0x1080, 0x1050: 0x1080})
        self.assertEqual(ends, {0x1020: 0x1050, 0x1024: 0x1050,
                                0x1050: 0x1080})

    def test_gap_alias_stops_at_its_section_end(self):
        insns = _code(0x10F0, 0x1120)       # decodes on into the next section
        ends = _built(insns, [(0x1000, 0x1010)], {0x10F0: 0x1200})
        self.assertEqual(ends, {0x10F0: SEC_HI})

    def test_gap_alias_stops_at_its_sections_backed_bytes(self):
        # The fill past raw_size only finishes a cut instruction; with none
        # cut, the bound is the backed bytes, never the virtual tail.
        insns = _code(0x10A0, 0x1120)
        ends = _built(insns, [(0x1000, 0x1010)], {0x10A0: 0x1200},
                      section=_ShortSection)
        self.assertEqual(ends, {0x10A0: SEC_LO + 0xC0})

    def test_gap_alias_keeps_a_ret_cut_by_raw_size(self):
        # Spartan's D3DX: `leave; ret 0x10` with the ret's last byte in the
        # zero fill. Clamping at raw_size dropped the ret from the alias.
        insns = _code(0x10A0, 0x10BC)
        leave = _Insn(0x10BC)
        leave.end_address = 0x10BD
        ret = _Insn(0x10BD, ret=True)
        ret.end_address = 0x10C0 + 1
        ends = _built(insns + [leave, ret], [(0x1000, 0x1010)],
                      {0x10A0: 0x1200}, section=_ShortSection)
        self.assertEqual(ends, {0x10A0: 0x10C1})

    def test_alias_inside_a_function_shares_its_end(self):
        insns = _code(0x1000, 0x1080, rets={0x1024, 0x1044})
        ends = _built(insns, [(0x1000, 0x1080)],
                      {0x1020: 0x1080, 0x1040: 0x1080})
        self.assertEqual(ends, {0x1020: 0x1080, 0x1040: 0x1080})


if __name__ == "__main__":
    unittest.main()
