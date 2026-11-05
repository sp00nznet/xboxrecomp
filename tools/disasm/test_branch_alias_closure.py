"""Branches that leave a lifted body land on an entry, not a stub.

Each function and each alias is lifted as its own C body over [start, end). A
direct jmp or jcc whose target is outside that range becomes a tail call to
sub_<target>. When nothing starts at the target, that is an unresolved stub
which adds 4 to esp and returns, as if the guest had executed a `ret`. The
native chain unwinds and the guest frame does not.

The fixture is a function F split at a lone int3 into two primary bodies, A
and B. The halves and their aliases branch into each other's interiors:

- B has a `jne` into the middle of A. The tail-jump pass looks only at
  unconditional jumps, and the orphan pass looks only at gaps.
- An alias that starts partway into A and runs to A's end has a `jle` back
  to a label before its own start but inside its primary. Every pass tests
  "leaves its function" against the primary bodies, so it looks like an
  intra-function branch.

Before the closure pass both went to stubs: the function returned with its
frame still on the guest stack, and its caller recursed until the native
stack overflowed.

Run: python3 -m pytest tools/disasm/test_branch_alias_closure.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.functions import Function, FunctionDetector  # noqa: E402


class _Insn:
    def __init__(self, addr, size=2, mnemonic="mov", target=None,
                 is_jump=False, is_cond_jump=False):
        self.address = addr
        self.size = size
        self.mnemonic = mnemonic
        self.jump_target = target
        self.is_jump = is_jump
        self.is_cond_jump = is_cond_jump


class _Engine:
    def __init__(self, insns):
        self.instructions = {i.address: i for i in insns}

    def get_instructions_in_range(self, start, end):
        return [self.instructions[a] for a in sorted(self.instructions)
                if start <= a < end]


def _detector(insns, functions, aliases=None):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(insns)
    det.functions = {s: Function(start=s, end=e, name=f"sub_{s:08X}")
                     for s, e in functions}
    det._alias_entries = dict(aliases or {})
    det._candidates = {}
    return det


# Two primary bodies, as the int3 split leaves them, plus one alias the
# earlier passes already found.
BASE = 0x10000
A_START, A_END = BASE, BASE + 0x3FB
B_START, B_END = BASE + 0x3FB, BASE + 0x63E
ALIAS = BASE + 0x30B             # an alias into A, running to A_END
LOOP_HEAD = BASE + 0xB7          # inside A, before ALIAS
LOOP_LABEL = BASE + 0x320        # inside A and inside ALIAS
JLE_AT = BASE + 0x330            # in ALIAS: jle LOOP_HEAD
JNE_AT = BASE + 0x500            # in B: jne LOOP_LABEL


def _split_function_shape():
    insns = [_Insn(a) for a in range(A_START, B_END, 0x10)]
    insns += [
        _Insn(LOOP_HEAD),
        _Insn(LOOP_LABEL),
        _Insn(JLE_AT, mnemonic="jle", target=LOOP_HEAD, is_cond_jump=True),
        _Insn(JNE_AT, mnemonic="jne", target=LOOP_LABEL, is_cond_jump=True),
        _Insn(JNE_AT + 0x10, mnemonic="jmp", target=A_START + 0x10, is_jump=True),
    ]
    return insns


def test_jcc_into_another_body_becomes_alias():
    det = _detector(_split_function_shape(), [(A_START, A_END), (B_START, B_END)])
    det._pass_branch_alias_closure()
    assert det._alias_entries.get(LOOP_LABEL) == A_END


def test_branch_back_out_of_an_alias_becomes_alias():
    det = _detector(_split_function_shape(), [(A_START, A_END), (B_START, B_END)],
                    aliases={ALIAS: A_END})
    det._pass_branch_alias_closure()
    # JLE_AT sits inside the alias [ALIAS, A_END) and branches to before
    # its start: an entry is needed there even though the primary covers it.
    assert det._alias_entries.get(LOOP_HEAD) == A_END


def test_closure_follows_new_aliases():
    # Only the jcc in B is visible at first. The alias it creates at
    # LOOP_LABEL contains the jle to LOOP_HEAD, which needs an entry of its own.
    det = _detector(_split_function_shape(), [(A_START, A_END), (B_START, B_END)])
    det._pass_branch_alias_closure()
    assert LOOP_LABEL in det._alias_entries
    assert det._alias_entries.get(LOOP_HEAD) == A_END


def test_intra_body_and_known_targets_are_left_alone():
    insns = _split_function_shape()
    det = _detector(insns, [(A_START, A_END), (B_START, B_END)])
    det._pass_branch_alias_closure()
    # The jmp to A_START + 0x10 is a known instruction inside A, so it gets
    # an alias too. A branch to a function start must not.
    assert A_START not in det._alias_entries
    assert B_START not in det._alias_entries
    det2 = _detector([_Insn(0x100), _Insn(0x110, mnemonic="jne", target=0x100,
                                          is_cond_jump=True)],
                     [(0x100, 0x200)])
    assert det2._pass_branch_alias_closure() == 0


def test_targets_in_gaps_or_mid_instruction_are_skipped():
    insns = [
        _Insn(0x100),
        _Insn(0x110, mnemonic="jne", target=0x250, is_cond_jump=True),  # gap
        _Insn(0x120, mnemonic="jne", target=0x305, is_cond_jump=True),  # mid
        _Insn(0x250),        # decodes, but no body covers it
        _Insn(0x300, size=8),
    ]
    det = _detector(insns, [(0x100, 0x200), (0x300, 0x400)])
    assert det._pass_branch_alias_closure() == 0
    assert det._alias_entries == {}
