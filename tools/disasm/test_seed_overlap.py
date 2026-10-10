"""Explicit seeds realign unclaimed sweep bytes, never a detected body."""
from tools.disasm.disasm import Disassembler
from tools.disasm.loader import BinaryImage, SectionInfo


def test_seed_realigns_only_outside_detected_bodies(monkeypatch):
    base = 0x10000
    # The first mov is real code; the second is an out-of-phase data sweep
    # swallowing xor eax,eax / mov [ecx],eax at the explicit entry.
    code = bytes.fromhex("b801020304c3 90 b933c08901c3c3")
    text = SectionInfo(".text", base, len(code), 0, len(code), False, True, "")
    image = BinaryImage("synthetic", code, base, len(code), base, 0, [text])
    monkeypatch.setattr("tools.disasm.disasm.load_image", lambda *args: image)
    good, bad = base + 8, base + 2
    disasm = Disassembler("synthetic", force=True, stats_only=True,
                          seed_functions=[good, bad])
    assert disasm.run()
    assert good in disasm.func_detector.functions
    assert disasm.engine.instructions[good].mnemonic == "xor"
    assert base + 7 not in disasm.engine.instructions
    assert disasm.engine.instruction_covering(good) is None
    assert bad not in disasm.func_detector.functions
    assert disasm.func_detector.functions[base].end == base + 6
    assert disasm.engine.instructions[base].size == 5
