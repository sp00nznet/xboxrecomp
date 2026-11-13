import json
import struct
import subprocess

from tools.prof import prof_buckets
from tools.prof.prof_buckets import (
    buckets,
    classify,
    flips_per_s,
    guest_names_from,
    log_summaries,
    parse_prof,
    pdb_symbols,
    pe_image_base,
    pe_sections,
    symbols,
)

PROF = """# recomp-prof 1 hz 100 rounds 1000 samples 4000 dropped 0 mode all
thread 0 10 1000 ?
thread 1 11 1000 nv2a-ack
thread 2 12 1000 dxvk-cs
thread 3 13 1000 mcpx.apu_thread
module 0 0x1000 C:\\windows\\system32\\ntdll.dll
module 1 0x2000 C:\\windows\\system32\\d3d11.dll
module 2 0x3000 C:\\windows\\system32\\ucrtbase.dll
s 0 2 0x10 500
s 0 0 0x10 500
s 1 1 0x20 300
s 1 0 0x20 700
s 2 2 0x30 100
s 2 0 0x30 900
s 3 2 0x40 50
s 3 0 0x40 950
"""


def test_classify_by_symbol_and_thread():
    assert classify("?", "x.exe", "sub_0005F960", {"sub_0005F960"}) == "guest-spin"
    assert classify("?", "x.exe", "sub_00012345", set()) == "guest"
    assert classify("?", "x.exe", "xbox_KeWaitForSingleObject", set()) == "kernel"
    assert classify("?", "x.exe", "xbox_SpinWaitKick", set()) == "guest-spin"
    assert classify("nv2a-ack", "x.exe", "nv2a_pb_exec_method", set()) == "executor"
    assert classify("nv2a-ack", "x.exe", "nv2a_pb_d3d11_draw", set()) == "renderer"
    assert classify("nv2a-ack", "d3d11.dll", None, set()) == "driver"
    assert classify("nv2a-ack", "?", None, set()) == "unix"
    assert classify("audio_client_main", "mmdevapi.dll", None, set()) == "wait"
    assert classify("?", "x.exe", "sub_00165630_gen", set()) == "guest"
    assert classify("?", "x.exe", "d3d_kickoff", set()) == "kernel"
    assert (
        classify("?", "x.exe", "d3d_kickoff", set(), frozenset({"d3d_kickoff"}))
        == "guest"
    )
    assert (
        classify(
            "?",
            "x.exe",
            "d3d_kickoff",
            {"d3d_kickoff"},
            frozenset({"d3d_kickoff"}),
        )
        == "guest-spin"
    )
    assert classify("nv2a-ack", "ntdll.dll", None, set()) == "wait"
    assert classify("nv2a-ack", "ucrtbase.dll", None, set()) == "executor"
    assert classify("dxvk-cs", "ucrtbase.dll", None, set()) == "driver"
    assert classify("mcpx.apu_thread", "ucrtbase.dll", None, set()) == "audio"
    assert classify("Main", "nvwgf2umx.dll", None, set()) == "driver"


def test_ms_per_flip():
    prof = parse_prof(PROF)
    # 1000 rounds at 100 Hz = 10 s = 300 flips at 30 flips/s; a sample is
    # 10 ms of the window, 1/30 ms per flip.
    r = buckets(prof, fps=30.0)
    assert r["window_s"] == 10.0
    assert r["threads"]["?"]["busy_ms"] == round(500 * 10 / 300, 3)
    assert r["threads"]["nv2a-ack"]["buckets"]["driver"] == 10.0
    assert r["threads"]["dxvk-cs"]["busy_share"] == 10.0
    assert r["totals_ms_per_flip"]["audio"] == round(50 * 10 / 300, 3)


def test_log_parsing():
    log = "\n".join(
        [
            "[PACING] flip %d t_us %d batches 10" % (i, 1_000_000 + i * 33_333)
            for i in range(301)
        ]
        + [
            "[PB-PERF] flip 5 wall 33.30 ms walk 20.00 ms (draw 12.00, decode 8.00)"
            " methods 100 batches 9000 present 1.00 kicks 3 kickwait 0.50"
            " stops 0 draws 9000",
            "[D3D11-PROF] flip 5 gpu 4.000 ms present 2.000 ms",
            "[PB-PERF] flip 6 wall 33.30 ms walk 20.00 ms (draw 12.00, decode 4.00)"
            " methods 100 batches 9000 present 1.00 kicks 3 kickwait 0.50"
            " stops 0 draws 9000 settle 4.00",
            # decode is a difference of two clocks and can read below 0.
            "[PB-PERF] flip 7 wall 33.30 ms walk 20.00 ms (draw 12.00, decode -0.02)"
            " methods 100 batches 9000 present 1.00 kicks 3 kickwait 0.50"
            " stops 0 draws 9000 settle 7.02",
        ]
    )
    fps, n = flips_per_s(log, 5.0)
    assert abs(fps - 30.0) < 0.1 and n == 151
    s = log_summaries(log)
    assert s["pb_perf"]["walk"] == 20.0 and s["pb_perf"]["batches"] == 9000
    # A line before settle reads 0 for it, and a negative decode
    # is kept: medians of the three.
    assert s["pb_perf"]["flips"] == 3
    assert s["pb_perf"]["settle"] == 4.0 and s["pb_perf"]["decode"] == 4.0
    assert s["gpu"]["gpu_ms_p50"] == 4.0


def fake_pe(path, sections=(0x1000, 0x200000)):
    """A PE32+ header: image base 0x140000000, one section per RVA."""
    d = bytearray(1024)
    d[:2] = b"MZ"
    off = 0x40
    struct.pack_into("<I", d, 0x3C, off)
    d[off : off + 4] = b"PE\0\0"
    struct.pack_into("<H", d, off + 6, len(sections))
    struct.pack_into("<H", d, off + 20, 0xF0)
    struct.pack_into("<H", d, off + 24, 0x20B)
    struct.pack_into("<Q", d, off + 24 + 24, 0x140000000)
    sh = off + 24 + 0xF0
    for i, rva in enumerate(sections):
        struct.pack_into("<I", d, sh + 40 * i + 12, rva)
    path.write_bytes(bytes(d))
    return str(path)


PUBLICS = """
                       Public Symbols
============================================================
  Records
  10 | S_PUB32 [size = 28] `sub_0004BC80`
           flags = function, addr = 0001:256
  20 | S_PUB32 [size = 28] `xbox_NtClose`
           flags = function, addr = 0001:4096
  30 | S_PUB32 [size = 40] `some_data`
           flags = none, addr = 0002:16
  40 | S_PUB32 [size = 28] `d3d_kickoff`
           flags = function, addr = 0001:2048
  50 | S_PUB32 [size = 28] `nv2a_pb_exec_method`
           flags = function, addr = 0002:64
"""


def fake_tools(monkeypatch, publics=PUBLICS):
    """nm finds no symbols in a stripped PE; llvm-pdbutil prints PUBLICS."""

    def run(cmd, **kw):
        if cmd[0] == "pdbutil":
            return subprocess.CompletedProcess(cmd, 0, publics, "")
        return subprocess.CompletedProcess(cmd, 1, "", "no symbols")

    monkeypatch.setattr(prof_buckets.subprocess, "run", run)


def test_pe_header(tmp_path):
    exe = fake_pe(tmp_path / "g.exe")
    assert pe_image_base(exe) == 0x140000000
    assert pe_sections(exe) == {1: 0x1000, 2: 0x200000}
    (tmp_path / "x.bin").write_bytes(b"\x7fELF" + bytes(60))
    assert pe_image_base(str(tmp_path / "x.bin")) is None


def test_pdb_publics_to_rvas(tmp_path, monkeypatch):
    exe = fake_pe(tmp_path / "g.exe")
    fake_tools(monkeypatch)
    starts, names = pdb_symbols(exe, str(tmp_path / "g.pdb"), "pdbutil")
    # SECT:OFF -> section RVA + OFF, functions only, sorted by address
    assert list(zip(starts, names)) == [
        (0x1000 + 256, "sub_0004BC80"),
        (0x1000 + 2048, "d3d_kickoff"),
        (0x1000 + 4096, "xbox_NtClose"),
        (0x200000 + 64, "nv2a_pb_exec_method"),
    ]


def test_symbols_falls_back_to_the_pdb(tmp_path, monkeypatch, capsys):
    exe = fake_pe(tmp_path / "g.exe")
    fake_tools(monkeypatch)
    # no PDB beside it: exe samples bucket by thread, with a warning
    assert symbols(exe, nm="nm", pdbutil="pdbutil") == ([], [])
    assert "no symbols" in capsys.readouterr().err
    (tmp_path / "g.pdb").write_bytes(b"")
    starts, names = symbols(exe, nm="nm", pdbutil="pdbutil")
    assert names[0] == "sub_0004BC80" and starts[0] == 0x1100


def test_guest_names_from_symbols_toml(tmp_path):
    t = tmp_path / "symbols.toml"
    t.write_text(
        '[[symbol]]\naddr = "0x002E7F30"\nname = "d3d_kickoff"\nkind = "function"\n'
        '[[symbol]]\naddr = "0x00400000"\nname = "g_score"\nkind = "global"\n'
        '[[symbol]]\naddr = "0x00010000"\nname = "stage_tick"\n'
    )
    assert guest_names_from(str(t)) == {"d3d_kickoff", "stage_tick"}


def test_main_reads_names(tmp_path, monkeypatch, capsys):
    """--names reaches the buckets: a named guest function on the guest
    thread is guest, not kernel."""
    exe = fake_pe(tmp_path / "g.exe")
    (tmp_path / "g.pdb").write_bytes(b"")
    fake_tools(monkeypatch)
    (tmp_path / "symbols.toml").write_text(
        '[[symbol]]\naddr = "0x0001A000"\nname = "d3d_kickoff"\nkind = "function"\n'
    )
    prof = tmp_path / "prof.txt"
    prof.write_text(
        "# recomp-prof 1 hz 100 rounds 1000 samples 300 dropped 0 mode all\n"
        "thread 0 10 1000 ?\n"
        "module 0 0x140000000 C:\\game\\g.exe\n"
        "s 0 0 0x%x 200\n"
        "s 0 0 0x%x 100\n" % (0x1000 + 2048 + 8, 0x1000 + 4096 + 8)
    )
    args = [str(prof), "--exe", exe, "--nm", "nm", "--pdbutil", "pdbutil", "--json"]
    prof_buckets.main(args + ["--names", str(tmp_path / "symbols.toml")])
    b = json.loads(capsys.readouterr().out)["buckets"]["threads"]["?"]["buckets"]
    assert b == {"guest": 200, "kernel": 100}
    prof_buckets.main(args)
    b = json.loads(capsys.readouterr().out)["buckets"]["threads"]["?"]["buckets"]
    assert b == {"kernel": 300}
