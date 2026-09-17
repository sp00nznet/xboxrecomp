"""Differential conformance: execute each snippet on the real CPU, then lift it
and execute the lifted C, and compare.

    py -3 -m tools.conformance            # run everything
    py -3 -m tools.conformance -k neg     # just the cases matching a substring

Why this works here and not on other recomp projects: ps3recomp has to build an
independent *model* of PowerPC to check its lifter against, because the host is
not a PPC. We target x86 and run on x86, so the host CPU is the reference
implementation -- an oracle no model can be wrong about.

The flow, per case:

  1. The assembler assembles the snippet (we never hand-encode), bracketed by
     nop markers, and hands back the exact bytes it produced
     MSVC via /FAc or gcc plus objdump.
  2. Those bytes go through our real Disassembler + Lifter.
  3. A harness runs both versions over the same inputs and compares eax.

Needs a 32-bit MSVC (vcvars32) or Docker linux/386 container.
Guest addresses map 1:1 onto host addresses here (g_xbox_mem_offset = 0),
so a memory operand reads the same bytes on both sides.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

from .cases import CASES
from .harness import (MARK, _MARK_BYTES, harness_source, native_source)
from . import corpus_run, xbe_run
from .corpus import CORPUS

_WHY = {c["name"]: c["why"] for c in CASES}
_TOL = {c["name"]: c.get("tol", 0.0) for c in CASES}


# "  00009\t3c ff\t\t cmp\t al, -1"  ->  addr, bytes.
#
# The byte column holds at most five bytes and then WRAPS, with the remainder
# on the next line under a leading tab and no address:
#
#     00009\t8b 0d 00 00 00
#   \t00\t\t mov\t ecx, DWORD PTR _g_in_b
#
# Missing the continuation silently drops every instruction longer than five
# bytes -- which is every 32-bit immediate -- and a dropped instruction is the
# one failure mode this whole tool exists to catch.
_COD_LINE = re.compile(
    r"^\s*([0-9a-fA-F]{5,})\t([0-9a-fA-F]{2}(?: [0-9a-fA-F]{2})*)(?:\t|$)")
_COD_CONT = re.compile(
    r"^\t([0-9a-fA-F]{2}(?: [0-9a-fA-F]{2})*)(?:\t|$)")
_COD_PROC = re.compile(r"^(\S+)\s+PROC\b")
_COD_ENDP = re.compile(r"^(\S+)\s+ENDP\b")


def _find_vcvars():
    for root in (os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                 os.environ.get("ProgramFiles", r"C:\Program Files")):
        for ed in ("Community", "Professional", "Enterprise", "BuildTools"):
            p = os.path.join(root, "Microsoft Visual Studio", "2022", ed,
                             "VC", "Auxiliary", "Build", "vcvars32.bat")
            if os.path.exists(p):
                return p
    return None


def _cl(vcvars, workdir, args):
    """Run cl.exe under a 32-bit toolchain environment."""
    cmd = f'"{vcvars}" >nul 2>&1 && cl /nologo {args}'
    return subprocess.run(cmd, cwd=workdir, shell=True, capture_output=True,
                          text=True)

_IMAGE = "xboxrecomp-conf-i386"

def _docker_image_present():
    # Not just a non-zero exit: with no docker on PATH at all this raises
    # FileNotFoundError, and the caller is the code whose whole job is to
    # print a readable "no toolchain" message instead of a traceback.
    try:
        return subprocess.run(["docker", "image", "inspect", _IMAGE],
                              capture_output=True).returncode == 0
    except OSError:
        return False


class _Container:
    """One Docker container held open for the whole run, not one per command."""

    def __init__(self, workdir, mounts=()):
        cmd = ["docker", "run", "-d", "--rm", "--platform", "linux/386",
               "-v", f"{workdir}:/w", "-w", "/w"]
        for src, dst in mounts:
            cmd += ["-v", f"{src}:{dst}:ro"]
        cmd += [_IMAGE, "sleep", "7200"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        self.cid = r.stdout.strip() if r.returncode == 0 else None
        self.error = "" if self.cid else (r.stdout + r.stderr)

    def run(self, script):
        """Run a shell script inside it; the workdir is /w."""
        return subprocess.run(["docker", "exec", self.cid, "sh", "-c", script],
                              capture_output=True, text=True)

    def close(self):
        if self.cid:
            subprocess.run(["docker", "rm", "-f", self.cid], capture_output=True)
            self.cid = None

# "   1a:\t90                   \tnop"  ->  addr, bytes. objdump wraps a long
# instruction onto a continuation line carrying an address but no mnemonic, so
# both forms feed the same byte stream -- dropping a continuation would drop
# every 32-bit immediate, the exact failure the .cod parser guards against too.
_OBJ_LINE = re.compile(r"^\s*[0-9a-f]+:\t([0-9a-f]{2}(?: [0-9a-f]{2})*)")
_OBJ_PROC = re.compile(r"^[0-9a-f]+ <([^>]+)>:")

def _bytes_from_objdump(dump_text, name):
    """Pull the bytes between the two nop markers out of one function."""
    want, inside, chunks = f"nat_{name}", False, []
    for line in dump_text.splitlines():
        m = _OBJ_PROC.match(line)
        if m:
            if inside:
                break
            inside = (m.group(1) == want)
            continue
        if not inside:
            continue
        m = _OBJ_LINE.match(line)
        if m:
            chunks.append(m.group(1).strip())
    stream = " ".join(chunks)
    first = stream.find(_MARK_BYTES)
    last = stream.rfind(_MARK_BYTES)
    if first < 0 or last <= first:
        raise RuntimeError(f"{name}: could not find both nop markers")
    mid = stream[first + len(_MARK_BYTES):last].strip()
    return bytes.fromhex(mid.replace(" ", ""))

def _bytes_from_listing(cod_text, name):
    """Pull the bytes between the two nop markers out of one function."""
    want, inside, chunks = f"_nat_{name}", False, []
    for line in cod_text.replace(chr(13), "").splitlines():
        m = _COD_PROC.match(line)
        if m:
            inside = (m.group(1) == want)
            continue
        if _COD_ENDP.match(line):
            if inside:
                break
            continue
        if not inside:
            continue
        m = _COD_LINE.match(line)
        if m:
            chunks.append(m.group(2).strip())
            continue
        m = _COD_CONT.match(line)
        if m and chunks:
            chunks.append(m.group(1).strip())
    stream = " ".join(chunks)
    first = stream.find(_MARK_BYTES)
    last = stream.rfind(_MARK_BYTES)
    if first < 0 or last <= first:
        raise RuntimeError(f"{name}: could not find both nop markers")
    mid = stream[first + len(_MARK_BYTES):last].strip()
    return bytes.fromhex(mid.replace(" ", ""))

def _lift(code_bytes):
    """Lift raw bytes through the real pipeline, exactly as recomp would.

    Via lift_basic_block, not lift_instruction: the flag peephole that turns
    `cmp` + `jcc`/`setcc`/`cmovcc` into a real condition lives at block level.
    Lifting one instruction at a time would test a path recomp never uses, and
    would report a stale `_flags` that the block pass never emits.
    """
    from tools.recomp.disasm import BasicBlock, Disassembler
    from tools.recomp.lifter import Lifter, lift_basic_block
    d = Disassembler()
    insns, mnemonics = [], []
    for insn in d._cs.disasm(code_bytes, 0x00100000):
        mnemonics.append(insn.mnemonic)
        insns.append(d._decode_instruction(insn))
    lifter = Lifter()
    # FunctionTranslator sets this per function, and the lifter only bothers
    # producing CF when something consumes it. Without it the snippet would be
    # lifted differently from how recomp would lift the same bytes.
    lifter.needs_cf = any(i.mnemonic in ("sbb", "adc") for i in insns)
    lines, _ = lift_basic_block(lifter, BasicBlock(start=0x00100000,
                                                   instructions=insns))
    return list(lines), mnemonics

def main_with_args(argv, allow_container=True):
    ap = argparse.ArgumentParser(prog="python -m tools.conformance",
                                 description=__doc__.splitlines()[0])
    ap.add_argument("-k", metavar="SUBSTR", help="only cases whose name matches")
    ap.add_argument("--keep", action="store_true",
                    help="keep the generated C and listing for inspection")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--only", choices=("snippets", "corpus"),
                    help="run only one of the two phases (default: both)")
    ap.add_argument("--xbe", metavar="PATH",
                    help="also lift functions out of a real title and run them "
                         "against their own machine code")
    ap.add_argument("--xbe-limit", type=int, default=40,
                    help="how many candidate functions to take (default 40)")
    args = ap.parse_args(argv)

    vcvars = _find_vcvars()
    backend = "msvc" if vcvars else None

    # allow_container is what keeps containers out of `pytest tools/`. The
    # MSVC check above is not a substitute: it happens to skip on macOS, but a
    # Linux CI box with Docker and no MSVC would otherwise start spinning up
    # containers during a unit-test pass.
    if not backend and allow_container and _docker_image_present():
        backend = "docker"

    if not backend:
        print("ERROR: no 32-bit x86 toolchain. Either a 32-bit MSVC "
              "(vcvars32.bat under Visual Studio 2022), or Docker running so a "
              "linux/386 container can supply the CPU.", file=sys.stderr)
        return 2

    cases = [c for c in CASES if not args.k or args.k in c["name"]]
    if not cases and args.only != "corpus":
        print(f"no cases match {args.k!r}", file=sys.stderr)
        return 2

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    runtime_inc = os.path.join(os.path.dirname(root), "templates", "runtime")

    workdir = tempfile.mkdtemp(prefix="xboxrecomp-conf-")
    if args.verbose or args.keep:
        print(f"workdir: {workdir}")

    dk = None
    if backend == "docker":
        dk = _Container(workdir, mounts=[(runtime_inc, "/rt")])
        if not dk.cid:
            print("could not start the linux/386 container:\n" + dk.error,
                  file=sys.stderr)
            if dk:
                dk.close()
            return 2

    if args.xbe:
        rc = _run_xbe(vcvars, workdir, runtime_inc, args)
        if not args.keep:
            import shutil
            shutil.rmtree(workdir, ignore_errors=True)
        return rc

    if args.only == "corpus":
        rc = _run_corpus(vcvars, workdir, runtime_inc, args)
        if not args.keep:
            import shutil
            shutil.rmtree(workdir, ignore_errors=True)
        return rc

    # 1. The toolchain assembles the snippets and tells us the exact bytes.
    #    We never hand-encode: whichever assembler runs is the authority on what
    #    the instruction actually is.
    if backend == "msvc":
        with open(os.path.join(workdir, "native.c"), "w") as f:
            f.write(native_source(cases))
        r = _cl(vcvars, workdir, "/c /FAc /Fanative.cod native.c")
        if r.returncode != 0:
            print("native assembly failed:\n" + r.stdout + r.stderr,
                  file=sys.stderr)
            if dk:
                dk.close()
            return 1
        with open(os.path.join(workdir, "native.cod"), errors="replace") as f:
            listing = f.read()
        extract = _bytes_from_listing
    else:
        with open(os.path.join(workdir, "native.c"), "w") as f:
            f.write(native_source(cases, dialect="gas"))
        # -fno-pic: produce a fixed-address executable and guest addresses have to stay 1:1 with host addresses
        r = dk.run("gcc -m32 -masm=intel -msse -fno-pic -O0 -c native.c "
                         "-o native.o && objdump -d -M intel native.o")
        if r.returncode != 0:
            print("native assembly failed:\n" + r.stdout + r.stderr,
                  file=sys.stderr)
            if dk:
                dk.close()
            return 1
        listing = r.stdout
        extract = _bytes_from_objdump

    # 2. Lift those bytes with the real pipeline.
    prepared, unlifted = [], []
    for c in cases:
        code = extract(listing, c["name"])
        lines, mnemonics = _lift(code)
        # NOP and identity LEA are intentionally emitted as a bare comment.
        dropped = [l for l in lines
                   if l.strip().startswith("/*") and l.strip() != "/* nop */"]
        if dropped:
            unlifted.append((c["name"], mnemonics, dropped))
        prepared.append((c["name"], c["kind"], lines, c["inputs"]))
        if args.verbose:
            print(f"  {c['name']:<20} {code.hex():<28} {len(lines)} C lines")

    # 3. Run both and compare.
    with open(os.path.join(workdir, "harness.c"), "w") as f:
        f.write(harness_source(prepared, _WHY, _TOL))
    if backend == "msvc":
        r = _cl(vcvars, workdir,
                f'/W3 /I"{runtime_inc}" harness.c native.obj /Feharness.exe')
        if r.returncode != 0:
            print("harness build failed:\n" + r.stdout + r.stderr,
                  file=sys.stderr)
            if dk:
                dk.close()
            return 1
        run = subprocess.run([os.path.join(workdir, "harness.exe")],
                             capture_output=True, text=True)
    else:
        r = dk.run("gcc -m32 -msse2 -mfpmath=sse -fno-pic -no-pie -O0 -I/rt "
                   "harness.c native.o "
                   "-o harness -lm")
        if r.returncode != 0:
            print("harness build failed:\n" + r.stdout + r.stderr,
                  file=sys.stderr)
            if dk:
                dk.close()
            return 1

        run = dk.run("./harness")
    print(run.stdout.strip())
    if run.returncode < 0 or run.returncode > 1:
        # A snippet faulted on the native side (idiv overflow, a bad memory
        # operand). Say so -- otherwise the run looks like a silent pass.
        print(f"\nharness terminated abnormally: exit {run.returncode} "
              f"(0x{run.returncode & 0xFFFFFFFF:08X}). The case that faulted is "
              f"the one after the last line printed above.", file=sys.stderr)
        if run.stderr.strip():
            print(run.stderr.strip(), file=sys.stderr)
        if dk:
            dk.close()
        return 1

    if unlifted:
        print("\nInstructions that lifted to a comment (silently no-ops, so the "
              "comparison above cannot see them):")
        for name, mnemonics, dropped in unlifted:
            print(f"  {name:<20} {' '.join(mnemonics)}")
            for d in dropped:
                print(f"      {d.strip()}")

    rc = run.returncode or (1 if unlifted else 0)

    if args.only != "snippets":
        rc = _run_corpus(vcvars, workdir, runtime_inc, args) or rc

    if dk:
        dk.close()
    if not args.keep:
        import shutil
        shutil.rmtree(workdir, ignore_errors=True)
    return rc




def _run_corpus(vcvars, workdir, runtime_inc, args):
    """Phase two: real C functions -- compiled, linked, lifted, compared."""
    nl = chr(10)
    if not vcvars:
        print(nl + "corpus phase SKIPPED: needs a 32-bit MSVC (PE/DLL linking). "
              "The snippet phase above did run and did compare.",
              file=sys.stderr)
        return 0
    fns = [f for f in CORPUS if not args.k or args.k in f["name"]]
    if not fns:
        return 0
    with open(os.path.join(workdir, "corpus.c"), "w") as f:
        f.write("/* corpus: compiled with the target compiler, then lifted "
                "back out of the linked image */" + nl
                + "#define EXP __declspec(dllexport)" + nl
                + nl.join(fn["source"] for fn in fns) + nl)

    # /arch:IA32 because the Xbox CPU is a Pentium III -- SSE1, no SSE2. Modern
    # MSVC defaults to SSE2 and puts doubles in XMM, which no real Xbox binary
    # contains, so without this the corpus would test instructions the target
    # cannot execute and skip the x87 paths every Xbox title actually uses.
    r = _cl(vcvars, workdir, "/c /O2 /GS- /arch:IA32 corpus.c")
    if r.returncode != 0:
        print("corpus compile failed:" + nl + r.stdout + r.stderr,
              file=sys.stderr)
        return 1

    # Linked, at a fixed base with relocations stripped, so every address in
    # the image is final and the harness can map it where it was built for.
    link = subprocess.run(
        f'"{vcvars}" >nul 2>&1 && link /NOLOGO /DLL /NOENTRY /OUT:corpus.dll '
        f'/MAP:corpus.map /BASE:0x{corpus_run.IMAGE_BASE:08X} /FIXED '
        f'/INCREMENTAL:NO corpus.obj kernel32.lib',
        cwd=workdir, shell=True, capture_output=True, text=True)
    if link.returncode != 0:
        print("corpus link failed:" + nl + link.stdout + link.stderr,
              file=sys.stderr)
        return 1

    with open(os.path.join(workdir, "corpus.dll"), "rb") as f:
        dll = f.read()
    with open(os.path.join(workdir, "corpus.map"), errors="replace") as f:
        mp = f.read()

    lifted, sections, base, missing, addr_of, unlifted = corpus_run.lift_all(
        dll, mp, ["_" + fn["name"] for fn in fns])
    if missing:
        print(nl + "Not found in the linked image (inlined away?): "
              + ", ".join(missing), file=sys.stderr)
    entries = [fn for fn in fns if "_" + fn["name"] not in missing]
    if not entries:
        return 1
    wanted = {"_" + fn["name"] for fn in entries}
    if args.verbose:
        for sym, body in lifted:
            mark = " " if sym in wanted else "*"
            print(f"  {mark} {sym:<22} {len(body.splitlines()):>5} C lines")
        print("  (* reached by a call from a corpus function, lifted too)")

    if unlifted:
        print(nl + "Instructions that lifted to a comment (silent no-ops, so "
              "the comparison below cannot see them):", file=sys.stderr)
        for fname, lines in sorted(unlifted.items()):
            print(f"  {fname}", file=sys.stderr)
            for line in lines:
                print(f"      {line}", file=sys.stderr)

    with open(os.path.join(workdir, "corpus_harness.c"), "w") as f:
        f.write(corpus_run.harness_source(entries, lifted, dll, sections, base, addr_of))
    r = _cl(vcvars, workdir,
            f'/W3 /I"{runtime_inc}" corpus_harness.c corpus.obj '
            f'/Fecorpus_harness.exe')
    if r.returncode != 0:
        print("corpus harness build failed:" + nl + r.stdout + r.stderr,
              file=sys.stderr)
        return 1
    run = subprocess.run([os.path.join(workdir, "corpus_harness.exe")],
                         capture_output=True, text=True)
    print(run.stdout.strip())
    if run.returncode < 0 or run.returncode > 1:
        print(nl + f"corpus harness terminated abnormally: exit "
              f"{run.returncode} (0x{run.returncode & 0xFFFFFFFF:08X})",
              file=sys.stderr)
        return 1
    return run.returncode




def _run_xbe(vcvars, workdir, runtime_inc, args):
    """Phase three: a real title's own functions, lifted and run against it."""
    nl = chr(10)
    if not vcvars:
        print(nl + "xbe phase SKIPPED: needs a 32-bit MSVC to build its harness.",
              file=sys.stderr)
        return 0
    from tools.recomp.disasm import Disassembler

    data, sections, base = xbe_run.load(args.xbe)
    entries, needed, closure = xbe_run.find_candidates(
        data, sections, args.xbe_limit, Disassembler()._cs)
    if not entries:
        print("found no self-contained functions to compare", file=sys.stderr)
        return 1
    lifted, rejected = xbe_run.lift(data, sections, needed)
    have = {va for va, _, _ in lifted}
    # An entry is only usable if everything it calls lifted cleanly too.
    callable_ = [row for row in lifted
                 if row[0] in closure and closure[row[0]] <= have]
    print(f"{os.path.basename(args.xbe)}: {len(entries)} entry points, "
          f"{len(needed)} functions in their call closure, "
          f"{len(callable_)} comparable, {len(rejected)} rejected")
    if rejected and args.verbose:
        for va, why in rejected:
            print(f"  rejected sub_{va:08X}: {why[0]}", file=sys.stderr)
    if not callable_:
        return 1

    with open(os.path.join(workdir, "xbe_harness.c"), "w") as f:
        f.write(xbe_run.harness_source(os.path.abspath(args.xbe), sections,
                                       lifted, callable_))
    r = _cl(vcvars, workdir,
            f'/W3 /EHa /I"{runtime_inc}" xbe_harness.c /Fexbe_harness.exe')
    if r.returncode != 0:
        print("xbe harness build failed:" + nl + r.stdout + r.stderr,
              file=sys.stderr)
        return 1
    # Re-run, quarantining whatever brought the harness down, until it gets
    # through. A title's code given arguments it never expected can corrupt the
    # process before any handler runs; the only reliable way to survive that is
    # to find out which function did it and leave that one out.
    exe = os.path.join(workdir, "xbe_harness.exe")
    skips, run = [], None
    for _ in range(40):
        cmd = [exe, os.path.abspath(args.xbe)]
        if skips:
            cmd.append(",".join(f"{va:08X}" for va in skips))
        run = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        started = re.findall(r"@RUN ([0-9A-Fa-f]{8})", run.stdout)
        if run.returncode in (0, 1):
            break
        if not started:
            break
        casualty = int(started[-1], 16)
        if casualty in skips:
            break
        skips.append(casualty)

    print(nl.join(l for l in run.stdout.splitlines()
                  if not l.startswith("@RUN ")).strip())
    if skips:
        print(nl + f"{len(skips)} function(s) crashed the harness and were "
              f"quarantined: "
              + ", ".join(f"sub_{va:08X}" for va in skips), file=sys.stderr)
    if run.returncode < 0 or run.returncode > 1:
        print(nl + f"xbe harness terminated abnormally: exit {run.returncode} "
              f"(0x{run.returncode & 0xFFFFFFFF:08X})", file=sys.stderr)
        return 1
    return run.returncode


def main():
    return main_with_args(None)


if __name__ == "__main__":
    sys.exit(main())
