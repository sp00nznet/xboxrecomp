"""A weak immediate must not split a callback accepted in the same pass."""
import struct

from tools.disasm.engine import DisasmEngine
from tools.disasm.functions import FunctionDetector
from tools.disasm.labels import LabelManager
from tools.disasm.loader import BinaryImage, SectionInfo


def test_immediate_inside_new_callback_keeps_both_epilogues():
    base = 0x10000
    callback = base + 32
    # Save two registers; each branch restores both. The false entry starts
    # inside mov edx,[esi+4], where its operand byte decodes as push esi.
    body = bytes.fromhex('565785c075068b56045f5ec35f5ec3')
    suffix = callback + 7
    refs = b'\xb8' + struct.pack('<I', callback)
    refs += b'\xba' + struct.pack('<I', suffix) + b'\xc3'
    code = refs.ljust(32, b'\x90') + body
    text = SectionInfo('.text', base, len(code), 0, len(code), False, True, '')
    image = BinaryImage('synthetic', code, 0, 0x20000, base, 0, [text])
    engine = DisasmEngine(image)
    engine.linear_sweep(text)
    detector = FunctionDetector(engine, image, None, LabelManager())
    detector._pass_known_addresses()
    detector._build_functions([text])
    assert detector._pass_imm_ref_targets([text])
    detector.functions.clear()
    detector._build_functions([text])
    assert suffix not in detector.functions
    assert detector.functions[callback].end == callback + len(body)
