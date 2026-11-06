import struct

import pytest

from .checkpoint_decode import FIELDS, decode as decode_checkpoints
from .trace_decode import RECORD, decode as decode_trace


def test_checkpoint_roundtrip_and_truncation(tmp_path):
    path = tmp_path / "cp.bin"
    path.write_bytes(struct.pack("<10I", *range(10)) + struct.pack("<10I", *range(10, 20)))
    rows = decode_checkpoints(path)
    assert [r["sequence"] for r in rows] == [0, 10] and rows[1]["eflags"] == 19
    assert tuple(rows[0]) == FIELDS
    path.write_bytes(path.read_bytes()[:-1])
    with pytest.raises(ValueError):
        decode_checkpoints(path)


def test_trace_roundtrip_and_truncation(tmp_path):
    path = tmp_path / "t.bin"
    path.write_bytes(RECORD.pack(1, 2, 0x1000, 3, 4, 5, 6))
    assert list(decode_trace(path)) == [dict(sequence=1, thread=2, eip=0x1000, type=3, arg0=4, arg1=5, arg2=6)]
    path.write_bytes(path.read_bytes() + b"\0")
    with pytest.raises(ValueError):
        list(decode_trace(path))
