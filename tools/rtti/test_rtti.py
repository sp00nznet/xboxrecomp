"""RTTI uses caller image flags and stays inside file-backed section ranges."""
from pathlib import Path
import struct
import tempfile
import unittest
from tools.conformance import mkxbe
from tools.rtti import Image, recover, seeds

def fixture(code_name=b"custom", code_flags=6):
    data = bytearray(mkxbe.build())
    data.extend(bytes(0x1300 - len(data)))
    struct.pack_into("<I", data, 0x11C, 2)
    struct.pack_into("<I", data, 0x400, code_flags)
    data[0x600:0x610] = code_name.ljust(16, b"\0")
    struct.pack_into("<6I", data, 0x438, 2, 0x20000, 0x200, 0x1100, 0x200, 0x10610)
    data[0x610:0x617] = b".rdata\0"
    name = b".?AVWidget@@\0"
    data[0x1108:0x1108+len(name)] = name
    struct.pack_into("<5I", data, 0x1140, 0, 0, 0, 0x20000, 0x20080)
    struct.pack_into("<4I", data, 0x1180, 0, 0, 1, 0x200A0)
    struct.pack_into("<I", data, 0x11A0, 0x200C0)
    struct.pack_into("<I", data, 0x11C0, 0x20000)
    struct.pack_into("<3I", data, 0x1200, 0x20040, mkxbe.TEXT_VA, 0)
    return data

class RttiImageTest(unittest.TestCase):
    def test_executable_flag_and_legacy_section_names(self):
        for name, flags, expected in ((b"custom", 6, [mkxbe.TEXT_VA]),
                                      (b"custom", 2, []),
                                      (b".text", 2, [mkxbe.TEXT_VA])):
            with self.subTest(name=name, flags=flags), tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "input.xbe"
                path.write_bytes(fixture(name, flags))
                result = recover(path)
                self.assertEqual(seeds(result), expected)
                if expected:
                    self.assertEqual(result["primary_len"][".?AVWidget@@"], 1)

    def test_word_reads_do_not_cross_raw_section_or_file_end(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "input.xbe"
            data = fixture()
            path.write_bytes(data)
            image = Image(path)
            self.assertIsNotNone(image.u32(mkxbe.TEXT_VA))
            self.assertIsNone(image.u32(mkxbe.TEXT_VA + len(mkxbe.build()) - mkxbe.TEXT_OFF - 3))
            self.assertIsNone(image.u32(0x201FF))
            # Header claims the same raw extent, but the actual file is truncated.
            path.write_bytes(data[:-1])
            self.assertIsNone(Image(path).u32(0x201FC))
            path.write_bytes(data[:0x1141])
            self.assertEqual(seeds(recover(path)), [])

if __name__ == "__main__":
    unittest.main()
