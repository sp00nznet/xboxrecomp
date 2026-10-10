"""Late table aliases must expose their complete tail chain without splits."""
import struct

import pytest

from tools.disasm.engine import DisasmEngine
from tools.disasm.functions import FunctionDetector
from tools.disasm.labels import LabelManager
from tools.disasm.loader import BinaryImage, SectionInfo


@pytest.mark.parametrize("conditional", [False, True])
def test_late_alias_tail_chain_reaches_an_interior_alias(conditional):
    base = 0x10000
    code = bytearray(b"\x90" * 0x410)
    chain = list(range(base + 0x300, base + 0x1C0 - 1, -0x20))
    enclosing, interior = base + 0x80, base + 0x88

    def put(addr, data):
        code[addr - base:addr - base + len(data)] = data

    def jump(addr, target):
        put(addr, b"\xe9" + struct.pack("<i", target - addr - 5))

    for addr in (base, base + 0x100, base + 0x400):
        put(addr, b"\xc3")
    put(enclosing, b"\x74\x06")  # keep the shared return inside this body
    jump(enclosing + 2, base)
    put(interior, b"\xb8\x01\x00\x00\x00\xc3")
    for addr, target in zip(chain, chain[1:] + [interior]):
        put(addr, b"\x8b\x44\x24\x04")  # synthetic argument forwarder
        if conditional and target == interior:
            put(addr + 4, b"\x0f\x85" + struct.pack("<i", target - addr - 10)
                + b"\xc3")
        else:
            jump(addr + 4, target)
    # Branch-shaped data after the first thunk lies in its borrowed range.
    bogus = base + 0x60
    jump(chain[0] + 9, bogus)
    put(bogus, b"\xc3")
    table = struct.pack("<II", enclosing, chain[0])
    text = SectionInfo(".text", base, len(code), 0, len(code), False, True, "")
    data = SectionInfo(".data", base + 0x1000, len(table), len(code),
                       len(table), False, False, "")
    image = BinaryImage("synthetic", bytes(code) + table, 0, 0x20000,
                        base, 0, [text, data])
    engine = DisasmEngine(image)
    for insn in engine._cs.disasm(bytes(code), base):
        engine.instructions[insn.address] = engine._classify_instruction(insn)
    detector = FunctionDetector(engine, image, None, LabelManager())
    for addr in (base + 0x100, base + 0x400):
        detector._add_candidate(addr, 1.0, "seed")

    detector.detect_all([text])

    assert set(chain + [enclosing, interior]) <= detector.functions.keys()
    assert detector.functions[interior].detection_method == "tail_jump_alias"
    assert interior not in detector._candidates
    assert bogus not in detector.functions
    assert detector.functions[enclosing].end == base + 0x100
    for addr in (base, base + 0x100, base + 0x400):
        assert detector.functions[addr].end == addr + 1
    assert not detector._pass_tail_jump_targets([text])
