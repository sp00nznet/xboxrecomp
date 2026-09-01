"""Regression tests for function pointers stored as immediates."""

from tools.disasm import config
from tools.disasm.engine import DisasmEngine
from tools.disasm.functions import FunctionDetector
from tools.disasm.labels import LabelManager
from tools.disasm.loader import BinaryImage, SectionInfo
from tools.disasm.xrefs import XRefTracker


BASE = 0x00011000
DATA_ADDRESS = 0x009D9DBC


def _imm32(value):
    return value.to_bytes(4, "little")


def _detect(body):
    text = SectionInfo(
        name=".text",
        virtual_addr=BASE,
        virtual_size=len(body),
        raw_addr=0,
        raw_size=len(body),
        writable=False,
        executable=True,
        flags="",
    )
    image = BinaryImage(
        filepath="<synthetic>",
        raw_data=bytes(body),
        base_address=BASE,
        image_size=len(body),
        entry_point=BASE,
        kernel_thunk_addr=0,
        sections=[text],
    )
    engine = DisasmEngine(image)
    engine.linear_sweep(text)
    detector = FunctionDetector(
        engine, image, XRefTracker(), LabelManager())
    detector.detect_all([text])
    return detector


def test_stored_code_pointer_becomes_function_candidate():
    callback = BASE + 0x20
    body = bytearray(b"\x90" * 0x40)
    body[0x00:0x03] = b"\x55\x8B\xEC"  # push ebp; mov ebp, esp
    body[0x03:0x0D] = (
        b"\xC7\x05" + _imm32(DATA_ADDRESS) + _imm32(callback))
    body[0x0D] = 0xC3
    body[0x20:0x23] = b"\x83\xF8\x05"  # cmp eax, 5
    body[0x23] = 0xC3

    detector = _detect(body)

    assert callback in detector.functions
    confidence, method = detector._candidates[callback]
    assert confidence == config.CONFIDENCE_CODE_POINTER
    assert method == "code_pointer"


def test_immediate_must_land_on_an_instruction_boundary():
    middle_of_instruction = BASE + 0x22
    body = bytearray(b"\x90" * 0x40)
    body[0x00:0x03] = b"\x55\x8B\xEC"
    body[0x03:0x08] = b"\xB8" + _imm32(middle_of_instruction)
    body[0x08] = 0xC3
    body[0x20:0x25] = b"\x3D" + _imm32(BASE)
    body[0x25] = 0xC3

    detector = _detect(body)

    assert middle_of_instruction not in detector.functions
    assert middle_of_instruction not in detector._candidates
