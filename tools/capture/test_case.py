import json

from . import case as capture


class FakeImage:
    def read_bytes_at_va(self, va, size):
        return bytes([0x90] * (size - 1) + [0xC3]) if va == 0x1000 else None


def _state(tmp_path, **extra):
    regs = {r: 0 for r in ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "eflags")}
    path = tmp_path / "state.json"
    path.write_text(json.dumps({"state": regs, **extra}))
    return path


def test_capture_writes_replayable_local_case(tmp_path, monkeypatch):
    monkeypatch.setattr(capture, "load_image", lambda *_: FakeImage())
    xbe = tmp_path / "default.xbe"
    xbe.write_bytes(b"XBEH")
    out = capture.capture_case(xbe, "0x1000", _state(tmp_path, end_eip=0x1004), tmp_path / "out")
    case = json.loads(out.read_text())
    assert case["code"] == "909090c3" and case["entry_eip"] == 0x1000
    assert case["capture"]["local_only"] and len(case["capture"]["xbe_sha256"]) == 64
    assert {"name", "code", "entry_eip", "stop", "state", "memory"} <= case.keys()


def test_capture_rejects_missing_end_and_unbacked_range(tmp_path, monkeypatch):
    import pytest
    monkeypatch.setattr(capture, "load_image", lambda *_: FakeImage())
    xbe = tmp_path / "default.xbe"
    xbe.write_bytes(b"XBEH")
    with pytest.raises(ValueError):
        capture.capture_case(xbe, 0x1000, _state(tmp_path), tmp_path / "o1")
    with pytest.raises(ValueError):
        capture.capture_case(xbe, 0x2000, _state(tmp_path, end_eip=0x2004), tmp_path / "o2")
