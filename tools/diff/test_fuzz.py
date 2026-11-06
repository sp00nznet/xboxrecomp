from .fuzz import make_case


def test_make_case_is_deterministic_and_valid():
    a, b = make_case(7, 8), make_case(7, 8)
    assert a.data == b.data
    assert a.data != make_case(8, 8).data
    assert a.data["stop"] == {"instructions": 8}
