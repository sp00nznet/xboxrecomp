"""Weak callbacks need a closed CFG, not a short linear path to a ret."""
import struct

import pytest

from tools.disasm.test_returning_body_switch import BASE, _engine, _dispatching_body
from tools.disasm.functions import FunctionDetector, Function
from tools.disasm.labels import LabelManager
from tools.disasm.loader import BinaryImage, SectionInfo


def test_long_callback_and_loop_have_no_instruction_budget():
    for body in (b"\x40" * 320 + b"\xc3", b"\x40\xeb\xfd"):
        engine, _ = _engine(body)
        assert engine.probes_as_callback_body(BASE, BASE + len(body))


@pytest.mark.parametrize("body", [
    b"\x74\x02\xc3\x90\x0f",  # ret on one arm, invalid other arm
    b"\x74\x02\xc3",           # branch outside the gap
    b"\x74\xff\xc3",           # branch into its own instruction
    b"\x74\x01\xb8\x00\x00\x00\x00\xc3",  # overlapping streams
    b"\x40",                    # falls off the gap
    b"\xff\xe0",                # unresolved indirect jump
    b"\x74\x01\xc3\xf4",       # ret does not excuse privileged other arm
    b"\xcd\x03\xc3", b"\x0f\x0b\xc3", b"\xfa\xc3",
    b"\x0f\x20\xc0\xc3", b"\xec\xc3", b"\xcc\xc3",
])
def test_invalid_or_open_cfg_is_rejected(body):
    engine, _ = _engine(body)
    assert not engine.probes_as_callback_body(BASE, BASE + len(body))


def test_no_return_call_padding_is_the_only_trap_exception():
    body = b"\xe8\x00\x01\x00\x00\xcc"
    engine, _ = _engine(body)
    assert engine.probes_as_callback_body(BASE, BASE + len(body))
    body = b"\x74\x05" + body
    engine, _ = _engine(body)
    assert not engine.probes_as_callback_body(BASE, BASE + len(body))


def test_all_measured_switch_arms_must_close_inside_gap():
    body, table = _dispatching_body(BASE)
    engine, _ = _engine(body)
    assert engine.probes_as_callback_body(BASE, BASE + len(body))
    engine.image.data = body[:10] + b"\xfa" + body[11:]
    assert not engine.probes_as_callback_body(BASE, BASE + len(body))
    engine.image.data = body[:24] + struct.pack("<III", BASE + 7, BASE + 10, BASE - 1)
    assert not engine.probes_as_callback_body(BASE, BASE + len(body))


def test_next_function_is_a_hard_bound():
    engine, _ = _engine(b"\x40\xc3")
    assert not engine.probes_as_callback_body(BASE, BASE + 1)


def test_table_word_inside_a_recovered_callback_is_not_a_new_entry():
    body = b"\xc3" + b"\xcc" * 15 + b"\x53" + b"\x40" * 320 + b"\x5b\xc3"
    table = struct.pack("<II", BASE + 16, BASE + 17)
    text = SectionInfo('.text', BASE, len(body), 0, len(body), False, True, '')
    data = SectionInfo('.data', BASE + 0x1000, len(table), len(body), len(table), False, False, '')
    image = BinaryImage('synthetic', body + table, 0, 0x20000, BASE, 0, [text, data])
    from tools.disasm.engine import DisasmEngine
    engine = DisasmEngine(image)
    engine.linear_sweep(text)
    detector = FunctionDetector(engine, image, None, LabelManager())
    detector._pass_known_addresses()
    detector._build_functions([text])
    assert detector._pass_data_ptr_targets([text])
    assert BASE + 16 in detector._alias_entries
    assert BASE + 17 not in detector._alias_entries
    detector._pass_data_ptr_targets([text])
    assert BASE + 17 not in detector._alias_entries


def test_existing_shared_body_still_allows_an_explicit_table_alias():
    from tools.disasm.engine import DisasmEngine
    body = b"\x40\xc3"
    table = struct.pack("<I", BASE + 1)
    text = SectionInfo('.text', BASE, len(body), 0, len(body), False, True, '')
    data = SectionInfo('.data', BASE + 0x1000, len(table), len(body), len(table), False, False, '')
    image = BinaryImage('synthetic', body + table, 0, 0x20000, BASE, 0, [text, data])
    engine = DisasmEngine(image)
    engine.linear_sweep(text)
    detector = FunctionDetector(engine, image, None, LabelManager())
    detector.functions[BASE] = Function(BASE, BASE + 2, 'owner')
    detector._alias_entries[BASE] = BASE + 2
    assert detector._pass_data_ptr_targets([text])
    assert detector._alias_entries[BASE + 1] == BASE + 2


def test_immediate_suffix_cannot_split_a_long_table_callback():
    from tools.disasm.engine import DisasmEngine
    callback = BASE + 16
    suffix = callback + 80
    body = b"\xb8" + struct.pack('<I', suffix) + b"\xc3" + b"\x90" * 10
    body += b"\x53" + b"\x40" * 200 + b"\x5b\xc3"
    table = struct.pack('<I', callback)
    text = SectionInfo('.text', BASE, len(body), 0, len(body), False, True, '')
    data = SectionInfo('.data', BASE + 0x1000, len(table), len(body), len(table), False, False, '')
    image = BinaryImage('synthetic', body + table, 0, 0x20000, BASE, 0, [text, data])
    engine = DisasmEngine(image)
    engine.linear_sweep(text)
    detector = FunctionDetector(engine, image, None, LabelManager())
    detector.detect_all([text])
    assert callback in detector.functions
    assert suffix not in detector._candidates
    assert suffix not in detector.functions
    assert detector.functions[callback].end >= callback + 203


@pytest.mark.parametrize('valid', [True, False])
def test_table_thunk_chain_requires_a_proven_destination(valid):
    from tools.disasm.engine import DisasmEngine
    body = bytearray(b'\xcc' * 460)
    for off in (0, 32, 96):
        body[off] = 0xc3
    for source, target in ((16, 64), (64, 128)):
        body[source:source + 5] = b'\xe9' + struct.pack('<i', target - source - 5)
    body[128:449] = b'\x40' * 320 + b'\xc3'
    if not valid:
        body[128] = 0xf4
    table = struct.pack('<I', BASE + 16)
    text = SectionInfo('.text', BASE, len(body), 0, len(body), False, True, '')
    data = SectionInfo('.data', BASE + 0x1000, len(table), len(body), len(table), False, False, '')
    image = BinaryImage('synthetic', bytes(body) + table, 0, 0x20000, BASE, 0, [text, data])
    engine = DisasmEngine(image)
    engine.linear_sweep(text)
    detector = FunctionDetector(engine, image, None, LabelManager())
    for off in (0, 32, 96):
        detector.functions[BASE + off] = Function(BASE + off, BASE + off + 1, 'known')
    assert not engine.probes_as_callback_body(BASE + 16, BASE + 32)
    detector._pass_data_ptr_targets([text])
    assert (BASE + 16 in detector._alias_entries) == valid


def test_entry_frame_rejects_an_epilogue_without_its_saves():
    for body, accepted in ((b'\x5b\xc3', False), (b'\x53\x5b\xc3', True),
                           (b'\x83\xc4\x08\xc3', False),  # add esp,8; ret
                           (b'\xc9\xc3', False),            # leave; ret
                           (b'\x83\xec\x08\x83\xc4\x08\xc3', True),
                           (b'\x55\x89\xe5\xc9\xc3', True)):
        engine, _ = _engine(body)
        assert engine.probes_as_callback_body(
            BASE, BASE + len(body), require_entry_frame=True) == accepted


def test_entry_frame_rejects_flags_a_partial_writer_leaves():
    # inc defines ZF but leaves CF, so jc still reads the owner's carry.
    for body, accepted in ((b'\x40\x74\x00\xc3', True), (b'\x40\x72\x00\xc3', False)):
        engine, _ = _engine(body)
        assert engine.probes_as_callback_body(
            BASE, BASE + len(body), require_entry_frame=True) == accepted


def test_entry_frame_allows_a_stack_neutral_forwarder():
    for prefix, accepted in ((b'\x89\xc8', True), (b'\x50', False)):
        branch = b'\xe9' + struct.pack('<i', 20 - len(prefix) - 5)
        body = (prefix + branch).ljust(20, b'\xcc') + b'\xc3'
        engine, _ = _engine(body)
        assert engine.probes_as_callback_body(
            BASE, BASE + 20, tail_targets={BASE + 20},
            require_entry_frame=True) == accepted


def test_backward_external_tail_is_not_a_closed_loop():
    prefix = b'\x6a\x01\xe8\x00\x01\x00\x00'
    body = prefix + b'\xe9' + struct.pack('<i', -16 - len(prefix) - 5)
    engine, _ = _engine(body)
    assert engine.probes_as_callback_body(
        BASE, BASE + len(body), tail_targets={BASE - 16})
    assert not engine.probes_as_callback_body(
        BASE, BASE + len(body), tail_targets={BASE - 16}, require_entry_frame=True)


def test_shared_callback_with_its_own_frame_remains_callable():
    from tools.disasm.engine import DisasmEngine
    body = b'\xc3' + b'\x90' * 15 + b'\x74\x06\xc3' + b'\x90' * 5
    body += b'\x53' + b'\x40' * 100 + b'\x5b\xc3'
    table = struct.pack('<II', BASE + 16, BASE + 24)
    text = SectionInfo('.text', BASE, len(body), 0, len(body), False, True, '')
    data = SectionInfo('.data', BASE + 0x1000, len(table), len(body), len(table), False, False, '')
    image = BinaryImage('synthetic', body + table, 0, 0x20000, BASE, 0, [text, data])
    engine = DisasmEngine(image)
    engine.linear_sweep(text)
    detector = FunctionDetector(engine, image, None, LabelManager())
    detector.functions[BASE] = Function(BASE, BASE + 1, 'known')
    detector._pass_data_ptr_targets([text])
    assert {BASE + 16, BASE + 24} <= detector._alias_entries.keys()


@pytest.mark.parametrize("prefix, accepted", [
    (b"", False), (b"\x53\x89\xc1", False), (b"\x53\x85\xc0", True),
    (b"\x53\xf3\xa5\x85\xc0", True),  # DF is not a caller condition code
])
def test_table_suffix_cannot_borrow_flags_from_an_existing_body(prefix, accepted):
    from tools.disasm.engine import DisasmEngine
    # A table word names the suffix after the owner's cmp. The suffix has
    # a balanced frame (or none), but its entry branch needs the owner's ZF.
    suffix = BASE + 3
    restore = b"\x5b" if prefix else b""
    body = b"\x83\xf8\x01" + prefix + b"\x74\x01\x40" + restore + b"\xc3"
    table = struct.pack("<I", suffix)
    text = SectionInfo('.text', BASE, len(body), 0, len(body), False, True, '')
    data = SectionInfo('.data', BASE + 0x1000, 4, len(body), 4, False, False, '')
    image = BinaryImage('synthetic', body + table, 0, 0x20000, BASE, 0, [text, data])
    engine = DisasmEngine(image)
    engine.linear_sweep(text)
    detector = FunctionDetector(engine, image, None, LabelManager())
    detector.detect_all([text])
    assert (suffix in detector.functions) is accepted
    assert detector.functions[BASE].end == BASE + len(body)
