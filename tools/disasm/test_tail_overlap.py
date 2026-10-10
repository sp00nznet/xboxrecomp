"""Reachable tail jumps can recover unclaimed out-of-phase sweep bytes."""
import struct

from tools.disasm.engine import DisasmEngine
from tools.disasm.functions import FunctionDetector
from tools.disasm.labels import LabelManager
from tools.disasm.loader import BinaryImage, SectionInfo


def test_tail_realigns_a_gap_without_splitting_another_body():
    base = 0x10000
    good, bad = base + 17, base + 7
    code = (b"\xe9" + struct.pack("<i", good - base - 5)
            + bytes.fromhex("b833c08901c3")
            + b"\xe9" + struct.pack("<i", bad - (base + 16))
            + bytes.fromhex("b933c08901c3c3"))
    text = SectionInfo(".text", base, len(code), 0, len(code), False, True, "")
    image = BinaryImage("synthetic", code, base, len(code), base, 0, [text])
    engine = DisasmEngine(image)
    engine.linear_sweep(text)
    detector = FunctionDetector(engine, image, None, LabelManager())
    for addr in (base + 5, base + 11):
        detector._add_candidate(addr, 1.0, "known")
    detector.detect_all([text])
    assert good in detector.functions
    assert engine.instructions[good].mnemonic == "xor"
    assert base + 16 not in engine.instructions
    assert bad not in detector.functions
    assert detector.functions[base + 5].end == base + 11
    assert engine.instructions[base + 5].size == 5
