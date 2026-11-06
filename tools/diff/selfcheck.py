from .case import Case
from .compare import first_divergence, format_divergence

def main():
    case = Case({"name": "selfcheck", "code": "90", "entry_eip": 0x1000,
                 "stop": {"instructions": 1},
                 "state": {name: 0 for name in ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "eflags")},
                 "memory": [], "calls": []}).validate()
    assert case.data["name"] == "selfcheck"
    divergence = first_divergence([{"eip": 0x1000, "eax": 1}], [{"eip": 0x1000, "eax": 2}])
    assert divergence and divergence.reason == "eax"
    assert "FIRST DIVERGENCE" in format_divergence(divergence)
    print("diff selfcheck: PASS")

if __name__ == "__main__":
    main()
