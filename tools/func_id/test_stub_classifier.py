"""SSE chains must not depend on a captured run's maximum length."""
from unittest.mock import patch
from . import config
from .stub_classifier import classify_stubs


def test_long_chain():
    op = bytes.fromhex("f30f100500200000")
    for count in (2, 16, 64):
        code = op * count + b"\xc3"
        with patch.object(config, "SECTIONS", [(".text", 0x1000, len(code), 0)]):
            result = classify_stubs(code, [{"start": "0x1000", "size": len(code)}])
            assert result[0x1000]["stub_type"] == ("float_copy" if count == 2 else "float_chain")
            bad = bytearray(code)
            bad[2] = 0xFF
            assert not classify_stubs(bad, [{"start": "0x1000", "size": len(code)}])


if __name__ == "__main__":
    test_long_chain()
    print("SSE chains: short/long recognition and invalid opcode rejection passed")
