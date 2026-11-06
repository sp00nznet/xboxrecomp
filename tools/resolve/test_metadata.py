from .metadata import resolve

def test_secondary_entry_and_unknown():
    entries = [{"guest_start": 0x1000, "guest_end": 0x1020, "symbol": "sub_1000"}]
    assert resolve(entries, 0x1008)["secondary"]
    assert resolve(entries, 0x2000)["entry"] is None


def test_hex_string_bounds():
    entries = [{"guest_start": "0x1000", "guest_end": "0x1020", "symbol": "sub_1000"}]
    assert not resolve(entries, 0x1000)["secondary"]
    assert resolve(entries, 0x1008)["secondary"]
