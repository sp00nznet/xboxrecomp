import json

import pytest

from tools.doctor import (_flatten_recomp_stats, build_report, parse_icall_feedback,
                          parse_runtime_log, rank_priorities)


def test_flatten_recomp_stats_merges_categories():
    stats = {
        "game": {"total": 10, "translated": 9, "failed": 1,
                 "unimplemented": {"foo": [1, 2], "bar": [3]}},
        "crt": {"total": 5, "translated": 5, "failed": 0,
                "unimplemented": {"foo": [4]}},
    }
    out = _flatten_recomp_stats(stats)
    assert out["total"] == 15
    assert out["translated"] == 14
    assert out["failed"] == 1
    assert out["unimplemented"] == {"foo": 3, "bar": 1}


def test_parse_icall_feedback_tolerates_truncated_and_merges_flags(tmp_path):
    path = tmp_path / "icalls.txt"
    # Runtime output is zero-padded hexadecimal without a 0x prefix. Keep a
    # prefixed row too because hand-edited/debug feedback files commonly use it.
    path.write_text("# icall-feedback v1\n00001000 1\n00002000 2\n0x1000 2\ntruncated\n")
    out = parse_icall_feedback(path)
    assert out["targets"] == 2
    assert out["resolved"] == 1
    assert out["unresolved"] == 2
    assert out["both"] == 1
    assert out["unresolved_vas"] == [0x1000, 0x2000]


def test_parse_runtime_log_counts_real_problem_spellings(tmp_path):
    path = tmp_path / "run.log"
    path.write_text(
        "[KERNEL] unresolved ordinal 123\n"
        "[D3D8] unsupported render state 999\n"
        "audio unsupported stream packet\n"
        "[ICALL] Failed to resolve VA 0x12345678\n"
    )
    out = parse_runtime_log(path)
    assert sum(out["kernel_stub"].values()) == 1
    assert sum(out["d3d_unsupported"].values()) == 1
    assert sum(out["audio_unsupported"].values()) == 1
    assert sum(out["unresolved_icall"].values()) == 1


def test_runtime_log_totals_are_not_truncated_to_top_25(tmp_path):
    path = tmp_path / "many.log"
    path.write_text("".join(
        f"[D3D8] unsupported render state {i}\n" for i in range(30)
    ))
    out = parse_runtime_log(path)
    assert len(out["d3d_unsupported"]) == 30
    assert sum(out["d3d_unsupported"].values()) == 30


def test_build_report_ranks_translation_and_icall_failures(tmp_path):
    functions = tmp_path / "functions.json"
    identified = tmp_path / "identified.json"
    abi = tmp_path / "abi.json"
    recomp = tmp_path / "recomp.json"
    icalls = tmp_path / "icalls.txt"
    functions.write_text(json.dumps([{"start": "0x1000"}, {"start": "0x2000"}]))
    identified.write_text(json.dumps({"0x1000": {}}))
    abi.write_text(json.dumps({"0x1000": {}, "0x2000": {}}))
    recomp.write_text(json.dumps({"total": 2, "translated": 1, "failed": 1,
                                  "unimplemented": {"fxam": [0x1010]}}))
    icalls.write_text("00003000 2\n")
    report = build_report(str(functions), str(identified), str(abi), str(recomp),
                          str(icalls), None)
    reasons = " ".join(item["reason"] for item in report["priorities"])
    assert report["pipeline"]["functions"] == 2
    assert "failed translation" in reasons
    assert "unimplemented instructions" in reasons
    assert "indirect targets were unresolved" in reasons


def test_build_report_marks_malformed_existing_artifact_invalid(tmp_path):
    functions = tmp_path / "functions.json"
    identified = tmp_path / "identified.json"
    abi = tmp_path / "abi.json"
    recomp = tmp_path / "recomp.json"
    functions.write_text("{not-json")
    identified.write_text("{}")
    abi.write_text("{}")
    recomp.write_text("{}")

    report = build_report(str(functions), str(identified), str(abi), str(recomp))
    assert report["pipeline"]["missing_artifacts"] == []
    assert report["pipeline"]["invalid_artifacts"] == ["functions"]
    assert any("invalid/unreadable artifacts" in item["reason"]
               for item in report["priorities"])


def test_rank_priorities_promotes_runtime_icall_without_feedback_file():
    report = {
        "pipeline": {"missing_artifacts": [], "invalid_artifacts": []},
        "recompiler": {"failed": 0, "unimplemented": {}},
        "icalls": {"unresolved": 0},
        "runtime": {"unresolved_icall": {"0x12345678": 3}},
    }
    out = rank_priorities(report)
    assert out[0]["severity"] == "high"
    assert out[0]["area"] == "control-flow"
    assert "3 runtime warning hit" in out[0]["reason"]


def test_rank_priorities_has_clean_fallback():
    report = {
        "pipeline": {"missing_artifacts": [], "invalid_artifacts": []},
        "recompiler": {"failed": 0, "unimplemented": {}},
        "icalls": {"unresolved": 0},
        "runtime": {},
    }
    out = rank_priorities(report)
    assert out == [{"severity": "info", "area": "bring-up",
                    "reason": "no known blockers found in supplied artifacts/logs"}]


@pytest.mark.parametrize("name, value", [
    ("functions", 42), ("functions", [42]), ("functions", [{"start": "bad"}]),
    ("identified", 42), ("identified", [{"start": "0x1000", "category": []}]),
    ("abi", 42), ("abi", [{"address": None}]),
    ("recomp", 42), ("recomp", {"game": 42}),
    ("recomp", {"total": "bad", "translated": 0, "failed": 0}),
    ("recomp", {"total": 1, "translated": 0, "failed": -1}),
    ("recomp", {"total": 1, "translated": 1, "failed": 0, "unimplemented": []}),
    ("recomp", {"total": 1, "translated": 1, "failed": 0,
                "unimplemented": {"fxam": "bad"}}),
])
def test_build_report_rejects_invalid_artifact_shapes(tmp_path, name, value):
    paths = {key: tmp_path / f"{key}.json"
             for key in ("functions", "identified", "abi", "recomp")}
    for path in paths.values():
        path.write_text("{}")
    paths[name].write_text(json.dumps(value))
    report = build_report(**paths)
    assert report["pipeline"]["invalid_artifacts"] == [name]
    assert any("invalid/unreadable artifacts" in item["reason"]
               for item in report["priorities"])


def test_kernel_log_preserves_real_thunk_and_apc_ordinals(tmp_path):
    path = tmp_path / "run.log"
    path.write_text(
        "[12:34:56.789] ERROR [THUNK ] Unresolved kernel ordinal 999\n"
        "[KERNEL] file I/O APC 0x00123456 unresolved (kernel ordinal 219)\n"
        "[KERNEL] file I/O APC 0x00123456 unresolved (kernel ordinal 236)\n"
    )
    assert parse_runtime_log(path)["kernel_stub"] == {
        "ordinal 999": 1, "ordinal 219": 1, "ordinal 236": 1,
    }


@pytest.mark.parametrize("mapping", [False, True])
def test_identified_count_excludes_unknown_functions(tmp_path, mapping):
    from tools.func_id.output import _build_enriched_db

    functions = [{"start": f"0x{addr:x}", "end": f"0x{addr+16:x}", "size": 16,
                  "name": f"sub_{addr:x}", "section": ".text"}
                 for addr in (0x1000, 0x2000)]
    identified = _build_enriched_db(functions, {}, {
        0x1000: {"name": "memcpy", "confidence": 1.0, "method": "signature"},
    }, {}, {})
    if mapping:
        identified = {entry["start"]: entry for entry in identified}
    path = tmp_path / "identified.json"
    path.write_text(json.dumps(identified))
    report = build_report(identified=path)
    assert report["pipeline"]["invalid_artifacts"] == []
    assert report["pipeline"]["identified_functions"] == 1
