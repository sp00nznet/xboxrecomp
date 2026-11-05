"""FCMOVcc lifting: the conditional x87 move must happen.

The eight FCMOVcc forms were left as comments (`/* FPU: fcmove ... */`), so the
move never happened. MSVC compiles float min/max to

    fld a; fld b; fcom st(1); fnstsw ax; test ah, 1;
    fcmove st(0), st(1); fxch st(1); fstp st(0)

and without the move min(x, 1.0) always returned 1.0 and max(x, 0.0) always
0.0. Jet Set Radio Future clamps its screen-fade colors that way, so the fade
went back to zero every frame and the boot never left its first logo.

Each form reads the flags of the jcc with the same condition code.
"""

from tools.recomp.disasm import BasicBlock, Instruction, Operand
from tools.recomp.lifter import Lifter, lift_basic_block


def _reg(name, size=4):
    op = Operand("reg")
    op.reg = name
    op.size = size
    return op


def _imm(value, size=1):
    op = Operand("imm")
    op.imm = value
    op.size = size
    return op


def _insn(address, mnemonic, op_str, operands, size=2):
    insn = Instruction(address, size, mnemonic, op_str, "00" * size)
    insn.operands = operands
    return insn


def _fcmov(mnemonic, source=1, address=0):
    return _insn(address, mnemonic, f"st(0), st({source})",
                 [_reg("st(0)", 10), _reg(f"st({source})", 10)])


def _lift_block(instructions):
    lifter = Lifter()
    lifter.func_start = 0
    lifter.func_end = 0x40
    generated, _ = lift_basic_block(
        lifter, BasicBlock(start=0, instructions=instructions))
    return "\n".join(generated)


def test_msvc_min_idiom_moves_under_the_test_flags():
    block = [
        _insn(0, "fcom", "st(1)", [_reg("st(1)", 10)]),
        _insn(2, "fnstsw", "ax", [_reg("ax", 2)]),
        _insn(4, "test", "ah, 1", [_reg("ah", 1), _imm(1)], size=3),
        _fcmov("fcmove", 1, address=7),
    ]
    body = _lift_block(block)
    move = [line for line in body.splitlines() if "fcmove" in line]
    assert move, body
    assert "FPU: fcmove" not in body, body
    # je after `test ah, 1` is ZF = ((ah & 1) == 0).
    assert move[0].startswith("if (") and "fp_top() = fp_st1();" in move[0], move[0]
    # The fork lifts `test` as a cmp of (ah & 1) against 0, so ZF reads as
    # CMP_EQ(_fa, _fb) there.
    assert ("TEST_Z" in move[0] or "== 0" in move[0]
            or "CMP_EQ(_fa, _fb)" in move[0]), move[0]


def test_every_form_moves_its_source_register():
    for mnemonic in ("fcmovb", "fcmove", "fcmovbe", "fcmovu",
                     "fcmovnb", "fcmovne", "fcmovnbe", "fcmovnu"):
        block = [
            _insn(0, "test", "ah, 0x41", [_reg("ah", 1), _imm(0x41)], size=3),
            _fcmov(mnemonic, 3, address=3),
        ]
        body = _lift_block(block)
        assert "FPU: fcmov" not in body, (mnemonic, body)
        assert "fp_top() = g_fp_stack[(g_fp_top + 3) & 7];" in body, (mnemonic, body)


def test_parity_forms_read_parity():
    block = [
        _insn(0, "test", "ah, 0x44", [_reg("ah", 1), _imm(0x44)], size=3),
        _fcmov("fcmovu", 1, address=3),
    ]
    assert "RECOMP_PARITY8" in _lift_block(block)


def test_untracked_flags_fall_back_to_flags_variable():
    body = "\n".join(Lifter().lift_instruction(_fcmov("fcmovne", 2)))
    assert body.startswith("if (_flags") and "g_fp_stack[(g_fp_top + 2) & 7]" in body, body
