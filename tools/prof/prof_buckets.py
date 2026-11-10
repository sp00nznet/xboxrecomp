"""Split a frame's CPU cost into buckets from a RECOMP_TRACE=prof report.

  python3 -m tools.prof.prof_buckets PROF.txt --exe GAME.exe --log game-stdio.log
      [--names config/symbols.toml] [--spin sub_X,sub_Y] [--nm NM]
      [--pdbutil llvm-pdbutil] [--json]

The in-process sampler (src/platform/host_prof.c) counts program counters per
thread and writes them as module offsets. This names them with the exe's
symbol table (nm / llvm-nm, as the bench does; for a stripped Windows exe,
the publics of the PDB beside it) and files each sample in one
bucket by the thread it ran on and where it was:

  guest       the recompiled title (sub_<VA>, or a name from --names)
  guest-spin  the title's busy-waits on our runtime (--spin, xbox_SpinWait*)
  kernel      toolkit kernel/HLE code on guest threads (xbox_*, Nt*, ...)
  executor    the pushbuffer walker's decode (nv2a_* outside the backends)
  renderer    the GPU backend in the exe (D3D11 or Metal) and its present
  driver      d3d11.dll, dxgi, DXVK's threads, winevulkan, the vendor's
              user-mode driver
  unix        Wine only: a pc outside every PE module (Mesa/RADV working, or a
              thread blocked in a Unix syscall; perf tells them apart)
  audio       the APU / DSP threads
  wait        a thread parked in a wait (ntdll, win32u, kernelbase)
  other       the rest

With the log's [PACING] flip series it turns samples into milliseconds per
flip over the sampled window, per thread and per bucket. Threads run in
parallel: the frame's cost is the busiest pipeline stage (the guest thread,
the walker, DXVK's cs thread, the GPU), not their sum. The [D3D11-PROF]
per-flip lines (RECOMP_TRACE=d3d11_prof=all) and the [PB-PERF] lines
(RECOMP_TRACE=flip) are summarised next to it when the log has them.

Sampler modes: "all" (Wine: every thread every round, so a thread's samples
outside the wait modules are its busy time), "running-since" (native
Windows: only threads that ran since the last round), "running-now"
(macOS). In every mode, ms per flip = samples / hz / flips-per-second.
"""

import argparse
import bisect
import json
import os
import re
import shutil
import statistics
import struct
import subprocess
import sys
import tomllib

BUCKETS = (
    "guest",
    "guest-spin",
    "kernel",
    "executor",
    "renderer",
    "driver",
    "unix",
    "audio",
    "wait",
    "other",
)

WAIT_MODULES = ("ntdll.dll", "win32u.dll", "kernelbase.dll", "kernel32.dll")
DRIVER_MODULES = (
    "d3d11.dll",
    "dxgi.dll",
    "winevulkan.dll",
    "vulkan-1.dll",
    "d3dcompiler_47.dll",
    "libvkd3d-shader-1.dll",
)
# Vendor user-mode drivers on native Windows.
DRIVER_MODULE_RE = re.compile(r"^(nv|amd|ati|igd|igc|igx)\w*\.dll$", re.IGNORECASE)
AUDIO_MODULES = ("xaudio2_8.dll", "xaudio2_9.dll", "mmdevapi.dll", "audioses.dll")

RENDERER_RE = re.compile(
    r"^(_?nv2a_pb_d3d11|_?nv2a_pb_metal|d3d11_|metal_|ao_|present_|ovl_|dbg_|"
    r"nv2a_scene_ao|nv2a_present|nv2a_overlay|nv2a_dbg|librashader|lrs_|chain_|"
    r"flip_log|d11p_|occ_|nv2a_occ)"
)
EXECUTOR_RE = re.compile(r"^(nv2a_|vsh_|pb_|d3d8_|xbox_Nv2a|tex_|rc_|swizzle)")
KERNEL_RE = re.compile(
    r"^(xbox_|kernel_|Ke|Nt|Rtl|Ex|Io|Ob|Ps|Mm|Hal|Xe|Av|Dbg|Fsc|Xc|recomp_|guest_|g_)"
)
SPIN_RE = re.compile(r"^xbox_SpinWait")
# A generated function, by address, or a hook-split one (sub_X_gen).
GUEST_RE = re.compile(r"^sub_[0-9A-Fa-f]{8}(_gen)?$")
AUDIO_THREAD_RE = re.compile(r"^(mcpx|apu|faudio|audio|xaudio)", re.IGNORECASE)
GUEST_THREAD_RE = re.compile(r"^(guest-|\?$|process-main|cat_recomp|main)")
WALKER_THREADS = ("nv2a-ack", "nv2a-kick")
DRIVER_THREAD_RE = re.compile(r"^(dxvk-|nv(?!2a)|amd|vkd3d|wine_vk)", re.IGNORECASE)

PACING_RE = re.compile(r"^\[PACING\] flip (\d+) t_us (\d+)")
PBPERF_RE = re.compile(
    r"^\[PB-PERF\] flip (\d+) wall ([\d.]+) ms walk ([\d.]+) ms \(draw ([\d.]+), "
    r"decode ([\d.]+)\) methods (\d+) batches (\d+) present ([\d.]+) kicks (\d+) "
    r"kickwait ([\d.]+)"
)
D11P_RE = re.compile(r"^\[D3D11-PROF\] flip (\d+) gpu ([\d.]+) ms present ([\d.]+) ms")


def parse_prof(text):
    """The sampler's report -> {"header", "threads", "modules", "rows"}."""
    head, threads, modules, rows = {}, {}, {}, []
    for line in text.splitlines():
        p = line.split()
        if not p:
            continue
        if p[0] == "#" and len(p) > 2 and p[1] == "recomp-prof":
            it = iter(p[3:])
            head = dict(zip(it, it))
        elif p[0] == "thread" and len(p) >= 5:
            threads[int(p[1])] = " ".join(p[4:])
        elif p[0] == "module" and len(p) >= 4:
            modules[int(p[1])] = re.split(r"[\\/]", " ".join(p[3:]))[-1]
        elif p[0] == "s" and len(p) == 5:
            rows.append((int(p[1]), int(p[2]), int(p[3], 16), int(p[4])))
    return {"header": head, "threads": threads, "modules": modules, "rows": rows}


def pe_image_base(path):
    try:
        with open(path, "rb") as f:
            d = f.read(4096)
    except OSError:
        return None
    if d[:2] != b"MZ":
        return None
    off = struct.unpack_from("<I", d, 0x3C)[0]
    if d[off : off + 4] != b"PE\0\0":
        return None
    opt = off + 24
    if struct.unpack_from("<H", d, opt)[0] == 0x20B:
        return struct.unpack_from("<Q", d, opt + 24)[0]
    return struct.unpack_from("<I", d, opt + 28)[0]


def pe_sections(path):
    """The RVA of each PE section, by its 1-based index (PDB addr = SECT:OFF)."""
    with open(path, "rb") as f:
        d = f.read(4096)
    off = struct.unpack_from("<I", d, 0x3C)[0]
    n = struct.unpack_from("<H", d, off + 6)[0]
    sh = off + 24 + struct.unpack_from("<H", d, off + 20)[0]
    return {i + 1: struct.unpack_from("<I", d, sh + 40 * i + 12)[0] for i in range(n)}


PUB_RE = re.compile(r"S_PUB32 \[size = \d+\] `([^`]+)`")
PUB_ADDR_RE = re.compile(r"flags = [^,]*function[^,]*, addr = (\d+):(\d+)")


def pdb_symbols(exe, pdb, pdbutil=None):
    """The function publics of a release exe's PDB (lld writes publics only,
    without -g): llvm-pdbutil from llvm-mingw, which every build has. A
    static function has no public, so its samples go to the public before
    it, in practice a function of the same source file; the buckets stay
    right, the per-function names do not."""
    pdbutil = pdbutil or shutil.which("llvm-pdbutil")
    if not pdbutil:
        raise SystemExit("no llvm-pdbutil to read %s (pass --pdbutil)" % pdb)
    r = subprocess.run(
        [pdbutil, "dump", "--publics", pdb],
        check=False,
        capture_output=True,
        text=True,
        errors="replace",
    )
    if r.returncode:
        raise SystemExit("%s %s: %s" % (pdbutil, pdb, r.stderr.strip()))
    sect = pe_sections(exe)
    syms, name = [], None
    for line in r.stdout.splitlines():
        m = PUB_RE.search(line)
        if m:
            name = m.group(1)
            continue
        m = PUB_ADDR_RE.search(line)
        if m and name and int(m.group(1)) in sect:
            syms.append((sect[int(m.group(1))] + int(m.group(2)), name))
        name = None
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


def symbols(exe, nm=None, pdbutil=None):
    """(sorted offsets from the load address, names) of the exe's text: its
    symbol table, else the PDB beside it."""
    nm = nm or shutil.which("llvm-nm") or shutil.which("nm")
    if not nm:
        raise SystemExit("no nm or llvm-nm to symbolise %s" % exe)
    r = subprocess.run(
        [nm, "-n", exe], check=False, capture_output=True, text=True, errors="replace"
    )
    base = pe_image_base(exe)
    if r.returncode and base is None:
        raise SystemExit("%s %s: %s" % (nm, exe, r.stderr.strip()))
    pe = base is not None
    syms = []
    for line in r.stdout.splitlines():
        p = line.split(None, 2)
        if len(p) != 3:
            continue
        addr, kind, name = p
        if base is None and name == "__mh_execute_header":
            base = int(addr, 16)
        if kind in "Tt":
            syms.append((int(addr, 16), name if pe else name.removeprefix("_")))
    syms.sort()
    if not syms and pe:
        pdb = (
            exe[: -len(".exe")] + ".pdb"
            if exe.lower().endswith(".exe")
            else exe + ".pdb"
        )
        if os.path.exists(pdb):
            return pdb_symbols(exe, pdb, pdbutil)
        # Without its PDB the exe's samples still bucket by thread.
        print(
            "prof_buckets: %s has no symbols and no %s beside it; exe samples"
            " bucket by thread only" % (exe, pdb),
            file=sys.stderr,
        )
    base = base or 0
    return [a - base for a, _ in syms], [n for _, n in syms]


def guest_names_from(path):
    """The function names a game's config/symbols.toml gives guest
    addresses: the lifter emits those functions under them, not sub_<VA>."""
    with open(path, "rb") as f:
        doc = tomllib.load(f)
    return frozenset(
        e["name"]
        for e in doc.get("symbol", [])
        if "name" in e and e.get("kind", "function") == "function"
    )


def classify(thread, module, fn, spin, guest_names=frozenset()):
    """The bucket of one sample: thread is its OS name, module the module's
    file name, fn the exe's function (None outside the exe)."""
    t = thread or "?"
    m = (module or "?").lower()
    if fn is None:
        if m in WAIT_MODULES:
            return "wait"
        # Wine's audio pump idles between periods with its pc in mmdevapi
        # (the syscall stub is inlined), so the "all" sampler sees it busy.
        if t == "audio_client_main" and m == "mmdevapi.dll":
            return "wait"
        # Under Wine, a pc outside every PE module is on the Unix side:
        # the Vulkan driver working, or a thread blocked in a syscall.
        if m == "?":
            return "unix"
        if (
            DRIVER_THREAD_RE.match(t)
            or m in DRIVER_MODULES
            or DRIVER_MODULE_RE.match(m)
        ):
            return "driver"
        if AUDIO_THREAD_RE.match(t) or m in AUDIO_MODULES:
            return "audio"
        # The C runtime (memcpy, ...) works for whoever called it.
        return role_bucket(t)
    if fn in spin or SPIN_RE.match(fn):
        return "guest-spin"
    if AUDIO_THREAD_RE.match(t):
        return "audio"
    if GUEST_RE.match(fn) or fn in guest_names:
        return "guest"
    if RENDERER_RE.match(fn):
        return "renderer"
    if EXECUTOR_RE.match(fn):
        return "executor"
    if KERNEL_RE.match(fn):
        return "kernel"
    return role_bucket(t)


def role_bucket(t):
    if t in WALKER_THREADS:
        return "executor"
    if GUEST_THREAD_RE.match(t):
        return "kernel"
    if DRIVER_THREAD_RE.match(t):
        return "driver"
    if AUDIO_THREAD_RE.match(t):
        return "audio"
    return "other"


def flips_per_s(log_text, window_s):
    """Flips per second over the log's last window_s seconds of [PACING]
    flips (the sampler runs to the exit), and the count of them."""
    pts = [
        (int(m.group(1)), int(m.group(2)))
        for m in map(PACING_RE.match, log_text.splitlines())
        if m
    ]
    if len(pts) < 2:
        return None, 0
    end = pts[-1][1]
    lo = end - int(window_s * 1e6) if window_s else pts[0][1]
    sel = [p for p in pts if p[1] >= lo]
    if len(sel) < 2:
        return None, 0
    span = (sel[-1][1] - sel[0][1]) / 1e6
    return (len(sel) - 1) / span if span > 0 else None, len(sel)


def log_summaries(log_text, last=None):
    """Medians of the [PB-PERF] and [D3D11-PROF] per-flip lines (the last
    `last` of each when given)."""
    pb, gp = [], []
    for line in log_text.splitlines():
        m = PBPERF_RE.match(line)
        if m:
            pb.append([float(x) for x in m.groups()])
            continue
        m = D11P_RE.match(line)
        if m:
            gp.append((float(m.group(2)), float(m.group(3))))
    if last:
        pb, gp = pb[-last:], gp[-last:]
    out = {}
    if pb:
        cols = (
            "wall",
            "walk",
            "draw",
            "decode",
            "methods",
            "batches",
            "present",
            "kicks",
            "kickwait",
        )
        out["pb_perf"] = {
            c: round(statistics.median(r[i + 1] for r in pb), 3)
            for i, c in enumerate(cols)
        }
        out["pb_perf"]["flips"] = len(pb)
    if gp:
        g = sorted(x[0] for x in gp)
        out["gpu"] = {
            "flips": len(gp),
            "gpu_ms_p50": round(statistics.median(g), 3),
            "gpu_ms_p95": round(g[min(len(g) - 1, int(0.95 * len(g)))], 3),
            "present_ms_p50": round(statistics.median(x[1] for x in gp), 3),
        }
    return out


def buckets(
    prof, exe=None, nm=None, spin=(), fps=None, pdbutil=None, guest_names=frozenset()
):
    mods = prof["modules"]
    hz = float(prof["header"].get("hz", 0)) or 1.0
    exe_name = re.split(r"[\\/]", exe)[-1].lower() if exe else None
    main = {i for i, m in mods.items() if exe_name and m.lower() == exe_name}
    if exe_name and not main:
        # A renamed symbol copy (game-<sha>.exe): the one .exe that ran.
        exes = [i for i, m in mods.items() if m.lower().endswith(".exe")]
        main = set(exes) if len(exes) == 1 else set()
    starts, names = symbols(exe, nm, pdbutil) if main else ([], [])
    spin = set(spin)
    per_thread = {}
    top = {}
    for slot, mod, off, n in prof["rows"]:
        fn = None
        if mod in main and starts:
            k = bisect.bisect_right(starts, off) - 1
            fn = names[k] if k >= 0 else None
        t = prof["threads"].get(slot, "?")
        b = classify(t, mods.get(mod, "?"), fn, spin, guest_names)
        d = per_thread.setdefault(t, dict.fromkeys(BUCKETS, 0))
        d[b] += n
        key = fn or "[%s]" % mods.get(mod, "?")
        top.setdefault(b, {}).setdefault(key, 0)
        top[b][key] += n
    # Samples -> ms per flip: n / hz seconds over the window, / flips in it.
    rounds = int(prof["header"].get("rounds", 0))
    flips = fps * rounds / hz if fps else 0
    scale = (1000.0 / hz / flips) if flips else None

    def ms(n):
        return round(n * scale, 3) if scale else None

    threads = {}
    for t, d in per_thread.items():
        busy = sum(v for k, v in d.items() if k != "wait")
        threads.setdefault(t, {"busy_share": 0, "busy_ms": 0, "buckets": {}})
        th = threads[t]
        th["samples"] = th.get("samples", 0) + sum(d.values())
        th["busy_ms"] = ms(busy)
        th["busy_share"] = round(100.0 * busy / rounds, 1) if rounds else None
        th["buckets"] = {k: ms(v) if scale else v for k, v in d.items() if v}
    totals = dict.fromkeys(BUCKETS, 0)
    for d in per_thread.values():
        for k, v in d.items():
            totals[k] += v
    return {
        "hz": hz,
        "mode": prof["header"].get("mode"),
        "rounds": rounds,
        "window_s": round(rounds / hz, 1),
        "flips_per_s": round(fps, 2) if fps else None,
        "totals_ms_per_flip": {k: ms(v) for k, v in totals.items()}
        if scale
        else totals,
        "threads": dict(
            sorted(threads.items(), key=lambda kv: -(kv[1]["busy_ms"] or 0))
        ),
        "top": {
            b: sorted(d.items(), key=lambda kv: -kv[1])[:8]
            for b, d in top.items()
            if b != "wait"
        },
    }


def report(res, logsum):
    out = []
    w = out.append
    w(
        "sampler %s Hz, mode %s, %s s sampled, %s flips/s"
        % (res["hz"], res["mode"], res["window_s"], res["flips_per_s"])
    )
    unit = "ms/flip" if res["flips_per_s"] else "samples"
    w("")
    w("thread              busy%%  busy %-7s  buckets" % unit)
    for t, th in res["threads"].items():
        if not th["busy_ms"] and res["flips_per_s"]:
            continue
        b = "  ".join(
            "%s %s" % (k, v)
            for k, v in sorted(th["buckets"].items(), key=lambda kv: -kv[1])
            if k != "wait"
        )
        w("%-18s %5s  %8s  %s" % (t[:18], th["busy_share"], th["busy_ms"], b))
    w("")
    w("bucket totals (%s, summed over threads; threads overlap):" % unit)
    for k, v in res["totals_ms_per_flip"].items():
        if k != "wait":
            w("  %-10s %s" % (k, v))
    if "pb_perf" in logsum:
        p = logsum["pb_perf"]
        w("")
        w(
            "[PB-PERF] medians over %d flips: wall %s ms, walk %s (draw %s, decode %s),"
            " present %s, kickwait %s, batches %s"
            % (
                p["flips"],
                p["wall"],
                p["walk"],
                p["draw"],
                p["decode"],
                p["present"],
                p["kickwait"],
                p["batches"],
            )
        )
    if "gpu" in logsum:
        g = logsum["gpu"]
        w(
            "[D3D11-PROF] over %d flips: gpu span p50 %s ms, p95 %s ms;"
            " Present call p50 %s ms"
            % (g["flips"], g["gpu_ms_p50"], g["gpu_ms_p95"], g["present_ms_p50"])
        )
    w("")
    for b, items in res["top"].items():
        w("top %s: %s" % (b, ", ".join("%s %d" % kv for kv in items[:6])))
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("prof")
    ap.add_argument(
        "--exe", help="the exe that ran, or a renamed copy of it (its symbol table)"
    )
    ap.add_argument("--nm", help="nm or llvm-nm to read it with")
    ap.add_argument(
        "--pdbutil",
        help="llvm-pdbutil (llvm-mingw's bin/) for a stripped exe: its .pdb beside it",
    )
    ap.add_argument(
        "--log",
        help="game-stdio.log: [PACING] for ms per flip, [PB-PERF], [D3D11-PROF]",
    )
    ap.add_argument(
        "--spin", default="", help="comma list of the title's busy-wait functions"
    )
    ap.add_argument(
        "--names",
        help="the game's config/symbols.toml: the guest functions it names"
        " (the lifter emits them under those names)",
    )
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    with open(a.prof, encoding="utf-8", errors="replace") as f:
        prof = parse_prof(f.read())
    log = ""
    if a.log:
        with open(a.log, encoding="utf-8", errors="replace") as f:
            log = f.read()
    hz = float(prof["header"].get("hz", 0)) or 1.0
    window = int(prof["header"].get("rounds", 0)) / hz
    fps, nflips = flips_per_s(log, window) if log else (None, 0)
    res = buckets(
        prof,
        a.exe,
        a.nm,
        [s for s in a.spin.split(",") if s],
        fps,
        a.pdbutil,
        guest_names_from(a.names) if a.names else frozenset(),
    )
    logsum = log_summaries(log, last=nflips or None) if log else {}
    if a.json:
        json.dump({"buckets": res, "log": logsum}, sys.stdout, indent=1)
        print()
    else:
        print(report(res, logsum))


if __name__ == "__main__":
    main()
