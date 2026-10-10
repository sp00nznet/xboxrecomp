"""Padding alone is not evidence that the following bytes are code."""
from tools.disasm.test_returning_body_switch import BASE, _engine
from tools.disasm.functions import FunctionDetector
from tools.disasm.labels import LabelManager


def test_padding_followed_by_data_or_a_real_leaf():
    for body, accepted in ((b"\x40\xcc\x70\x01\xc3", False),
                           (b"\x40\xc3", True),
                           (b"\x8b\x44\x24\x04\xff\xe0", True)):
        engine, image = _engine(b"\xc3" + b"\xcc" * 15 + body)
        detector = FunctionDetector(engine, engine.image, None, LabelManager())
        detector._pass_cc_boundaries(image.section)
        assert (BASE + 16 in detector._candidates) == accepted
