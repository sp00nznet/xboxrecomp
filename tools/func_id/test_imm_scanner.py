"""Raw immediate candidate scanning includes the final complete encoding."""
import struct
from unittest.mock import patch
from . import config
from .imm_scanner import scan_immediate_refs


def test_section_tail():
    for opcode in (0x68, *range(0xB8, 0xC0)):
        for padding in (b"", b"\x90"):
            code = padding + bytes([opcode]) + struct.pack("<I", 0x2000)
            settings = dict(TEXT_RAW_ADDR=0, TEXT_VA_SIZE=len(code),
                            TEXT_VA_START=0x1000, RDATA_VA_START=0x2000,
                            RDATA_VA_END=0x2004, DATA_VA_START=0, DATA_VA_END=0)
            with patch.multiple(config, **settings):
                functions = [{"start": "0x1000"}]
                assert scan_immediate_refs(code, functions) == {0x2000: [0x1000]}
                assert scan_immediate_refs(code[:-1], functions) == {}


if __name__ == "__main__":
    test_section_tail()
    print("Immediate scanner: nine opcodes, padding and truncation passed")
