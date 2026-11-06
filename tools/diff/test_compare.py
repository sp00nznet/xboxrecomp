from .compare import first_divergence

def test_first_divergence():
    result = first_divergence([{"eip": 1, "eax": 2}, {"eip": 3, "cf": 1}],
                              [{"eip": 1, "eax": 2}, {"eip": 3, "cf": 0}])
    assert result.index == 1
    assert result.reason == "cf"
