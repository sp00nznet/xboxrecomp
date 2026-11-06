from .analyze import Value, analyze, join, UNKNOWN

def test_conservative_provenance():
    state, events = analyze([
        {"address": 1, "mnemonic": "mov", "operands": ["esi", {"kind": "global", "address": 0x431680}]},
        {"address": 2, "mnemonic": "lea", "operands": ["eax", {"kind": "mem", "base": "esi", "disp": 0x14}]},
        {"address": 3, "mnemonic": "call", "operands": ["eax"]},
    ])
    assert str(state["eax"]).startswith("DERIVED")
    assert events[0]["kind"] == "indirect-control"
    assert join(Value("CONST", (1,)), Value("CONST", (2,))).kind == UNKNOWN


def test_disassembled_bytes_feed_analyze():
    # mov eax, [esp+8]; mov ebx, eax; ret
    from tools.recomp.disasm import Disassembler
    from .xbe import _operand
    raw = bytes.fromhex("8B442408" "89C3" "C3")
    insns = Disassembler().disassemble_function(raw, 0x1000, 0x1000 + len(raw))
    norm = [{"address": i.address, "mnemonic": i.mnemonic,
             "operands": [_operand(o) for o in i.operands]} for i in insns]
    state, _ = analyze(norm)
    assert str(state["ebx"]) == "STACK_ARG(+0x8)"
