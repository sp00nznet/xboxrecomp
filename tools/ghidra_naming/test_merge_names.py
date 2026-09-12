"""merge_names must drop IDA's autonames as thoroughly as it drops Ghidra's.

An export that merges `loc_1A2B3C` onto a function is worse than no export:
the recompiler happily emits `void loc_1A2B3C(void)` and the name reads as
recovered ground truth when it is the address in disguise.
"""
import importlib.util
import json
import os
import tempfile

# No package __init__ here (the Ghidra scripts are Jython and must not be
# importable as one), so load the module by path the way its own runner does.
_spec = importlib.util.spec_from_file_location(
    "merge_names", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "merge_names.py"))
merge_names = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(merge_names)


def test_placeholders_dropped():
    for name in ("FUN_00014c20", "LAB_00014c20", "DAT_00014c20",
                 "loc_1A2B3C", "sub_1A2B3C", "nullsub_7", "jpt_401000",
                 "algn_401000", "asc_4A1B00", "stru_4A1B00", "unk_4A1B00",
                 "off_4A1B00", "flt_4A1B00", "dbl_4A1B00", "xmmword_4A1B00",
                 "s_Hello_00abc123", "0x401000", "12345", ""):
        assert merge_names.is_placeholder(name), name


def test_real_names_kept():
    for name in ("CPlayer__Think", "D3DDevice_SetTexture", "memcpy",
                 "Locate", "LoadTexture", "j_memcpy_real"):
        assert not merge_names.is_placeholder(name), name


def test_flirt_library_outranks_a_plain_symbol():
    # IDA marks FLIRT hits IMPORTED; that must win over the same address
    # carrying a duller name from the symbol table, the way Ghidra's FidDb does.
    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, "functions.json"), "w") as f:
            json.dump([{"address": "0x00014C20", "name": "strlen",
                        "is_thunk": False, "source": "IMPORTED"}], f)
        with open(os.path.join(d, "symbols.json"), "w") as f:
            json.dump([{"address": "0x00014C20", "name": "j_strlen_wrapper",
                        "type": "Label", "source": "ANALYSIS",
                        "primary": True}], f)
        final, buckets, _ = merge_names.build_map(d)
        # strlen is a reserved identifier, so it carries its address -- that is
        # the collision rule doing its job, not a placeholder being dropped.
        assert final["0x00014C20"] == "strlen_00014C20"
        assert buckets == {"fidb/library": 1}


def test_ida_autonames_leave_nothing_behind():
    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, "functions.json"), "w") as f:
            json.dump([{"address": "0x%08X" % ea, "name": nm, "is_thunk": False}
                       for ea, nm in ((0x14C20, "sub_14C20"),
                                      (0x14C40, "loc_14C40"),
                                      (0x14C60, "CSound__Play"))], f)
        final, _, _ = merge_names.build_map(d)
        # Doubled underscores collapse -- sanitize's job, not the filter's.
        assert final == {"0x00014C60": "CSound_Play"}


if __name__ == "__main__":
    for fn in (test_placeholders_dropped, test_real_names_kept,
               test_flirt_library_outranks_a_plain_symbol,
               test_ida_autonames_leave_nothing_behind):
        fn()
        print("ok ", fn.__name__)
