"""Replacing a signature must release its old name mapping."""
from unittest.mock import patch
from . import config
from .crt_identifier import identify_crt_functions


def test_longer_signature_replacement():
    code = bytes.fromhex("aabbcc90aabb90")
    functions = [{"start": "0x1000", "size": 4},
                 {"start": "0x1004", "size": 3}]
    signatures = [("short", bytes.fromhex("aabb"), None, 0),
                  ("long", bytes.fromhex("aabbcc"), None, 0)]
    with patch.object(config, "SECTIONS", [(".text", 0x1000, len(code), 0)]), \
         patch.object(config, "CRT_SIGNATURES", signatures):
        result = identify_crt_functions(code, functions)
        assert {a: r["name"] for a, r in result.items()} == {
            0x1000: "long", 0x1004: "short"}, result


if __name__ == "__main__":
    test_longer_signature_replacement()
    print("CRT matcher: longer signature replacement preserves both names")
