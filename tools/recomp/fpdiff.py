"""Differential FP/SIMD checks: lifted C against the same bytes on the CPU.

    python -m tools.recomp.fpdiff --out <scratch-dir> [-k substring]
        [--include <dir> --prelude <file>]

Every case is a synthetic snippet. 32-bit MSVC assembles it once; the native
side runs those instructions and the lifted side runs the C that
FunctionTranslator emits for the same bytes, from identical inputs. eax, the
x87 stack, every XMM lane and the scratch buffer are compared exactly, except
that two NaNs always match and the transcendentals carry a tolerance. No game
bytes or addresses are involved.

By default the lifted side builds against templates/runtime. A project with
its own recomp_types.h passes that directory as --include, and a --prelude
that includes it and supplies the register globals harness._PREAMBLE would
otherwise declare, so the header the game really compiles is the one tested.
"""

import argparse
import math
import random
import re
import struct
import subprocess
from pathlib import Path

from tools.conformance import __main__ as runner
from tools.conformance import harness
from tools.conformance.cases import Case
from . import config
from .translator import FunctionTranslator

BASE = 0x00100000
CC16 = ("a", "ae", "b", "be", "e", "ne", "p", "np",
        "s", "ns", "o", "no", "l", "ge", "le", "g")


def _jump(cc):
    # Result in ecx so the flag setter and the jcc stay adjacent, as in game
    # code. The label is the last instruction, so the jump lands on the end.
    return ["mov ecx, 1", f"j{cc} done", "xor ecx, ecx", "done: mov eax, ecx"]


def _set(cc):
    return [f"set{cc} al", "movzx eax, al"]


def _cmov(cc):
    return ["mov ecx, 1", "mov eax, 0", f"cmov{cc} eax, ecx"]


def cases():
    out = []

    def add(name, asm, kind="fpu", tol=0.0, domain="1"):
        # domain: C condition on the fpu inputs a and b. Vectors outside it
        # are skipped, for instructions whose result Intel leaves undefined.
        # Integer-only snippets compare bit for bit: their results can look
        # like NaNs, and the NaN-payload allowance would hide differences.
        exact = not any(re.match(r"(f|cvt|u?comis|rcp|rsqrt|sqrt)|.*(ss|sd|ps|pd)$",
                                 insn.split()[0]) for insn in asm)
        out.append(dict(Case(name, name, asm, [], kind, tol), domain=domain,
                        exact=int(exact)))

    def status(name, asm, mask=0x4500, tol=0.0, domain="1"):
        # AL holds exception flags the model does not keep, and C1 is only
        # meaningful after fxam/fprem, so compare the bits code tests.
        add(name, asm + ["fnstsw ax", f"and eax, {mask:#x}"], tol=tol, domain=domain)

    # Scratch layout: [0] double a, [8] float a, [12] int a,
    # [16] double b, [24] float b, [28] int b, [32..63] output.
    two = ["fld qword ptr [eax+16]", "fld qword ptr [eax]"]            # a b
    three = ["fld dword ptr [eax+24]"] + two                           # a b bf
    six = ["fld1", "fldpi", "fld dword ptr [eax+24]",
           "fld dword ptr [eax+8]"] + two                              # a b af bf pi 1

    for op in ("fadd", "fsub", "fsubr", "fmul", "fdiv", "fdivr"):
        add(f"{op}_m32", two + [f"{op} dword ptr [eax+24]"])
        add(f"{op}_m64", two + [f"{op} qword ptr [eax+16]"])
        for i in range(6):
            add(f"{op}_st0_st{i}", six + [f"{op} st, st({i})"])
            add(f"{op}_to_st{i}", six + [f"{op} st({i}), st"])
            if i:
                add(f"{op}p_st{i}", six + [f"{op}p st({i}), st"])
        fi = "fi" + op[1:]
        add(f"{fi}_m16", two + [f"{fi} word ptr [eax+28]"])
        add(f"{fi}_m32", two + [f"{fi} dword ptr [eax+28]"])

    add("fld_m32", ["fld dword ptr [eax+8]"])
    add("fld_m64", ["fld qword ptr [eax]"])
    add("fld_m80", ["fld qword ptr [eax]", "fstp tbyte ptr [eax+32]",
                    "fld tbyte ptr [eax+32]"])
    for op in ("fst", "fstp"):
        add(f"{op}_m32", two + [f"{op} dword ptr [eax+32]"])
        add(f"{op}_m64", two + [f"{op} qword ptr [eax+32]"])
        for i in range(6):
            add(f"{op}_st{i}", six + [f"{op} st({i})"])
    for i in range(6):
        add(f"fld_st{i}", six + [f"fld st({i})"])
    for i in range(1, 6):
        add(f"fxch_st{i}", six + [f"fxch st({i})"])
    add("fild_m16", ["fild word ptr [eax+28]"])
    add("fild_m32", ["fild dword ptr [eax+28]"])
    add("fild_m64", ["fild qword ptr [eax]"])
    for op in ("fldz", "fld1", "fldpi", "fldl2e", "fldl2t", "fldlg2", "fldln2"):
        add(op, [op])
    for op in ("fchs", "fabs", "fsqrt"):
        add(op, two + [op])

    for rc in range(4):
        cw = [f"mov word ptr [eax+48], {0x27f | rc << 10:#x}",
              "fldcw word ptr [eax+48]"]
        for op, sizes in (("fist", ("word", "dword")),
                          ("fistp", ("word", "dword", "qword")),
                          ("fisttp", ("word", "dword", "qword"))):
            for size in sizes:
                add(f"{op}_{size}_rc{rc}", cw + two + [f"{op} {size} ptr [eax+32]"])
        add(f"frndint_rc{rc}", cw + two + ["frndint"])
    # Precision control 24 (the arithmetic rounds to float) and, as a known
    # model limit, directed rounding of arithmetic.
    for pc, rc in ((0, 0), (0, 1), (2, 2), (3, 3)):
        cw = [f"mov word ptr [eax+48], {0x7f | pc << 8 | rc << 10:#x}",
              "fldcw word ptr [eax+48]"]
        for op in ("faddp st(1), st", "fsubp st(1), st", "fmulp st(1), st",
                   "fdivp st(1), st", "fsqrt", "fadd dword ptr [eax+24]"):
            add(f"cw_pc{pc}_rc{rc}_{op.split()[0]}", cw + two + [op])

    # f2xm1 and fyl2xp1 are defined only on these ranges.
    for op, tol, domain in (("fsin", 1e-9, "1"), ("fcos", 1e-9, "1"),
                            ("fsincos", 1e-9, "1"), ("fptan", 1e-9, "1"),
                            ("f2xm1", 1e-13, "fabs(a) <= 1")):
        add(op, two + [op], tol=tol, domain=domain)
        status(f"{op}_sw", two + [op], 0x0400, tol, domain)
    for op, tol, domain in (("fpatan", 1e-13, "1"), ("fyl2x", 1e-13, "1"),
                            ("fyl2xp1", 1e-13, "fabs(a) < 0.29"), ("fscale", 0.0, "1")):
        add(op, two + [op], tol=tol, domain=domain)
    # One partial step reduces by an implementation-chosen 32..63 bits, so
    # single steps are compared only where one step completes; the loops
    # (what fmod and the CRT's trig reduction run) cover the rest.
    one_step = "!(isfinite(a) && isfinite(b) && b != 0 && fabs(a) >= ldexp(fabs(b), 62))"
    for op in ("fprem", "fprem1"):
        add(op, two + [op], domain=one_step)
        status(f"{op}_sw", two + [op], 0x4700, domain=one_step)
        add(f"{op}_loop", two + [f"again: {op}", "fnstsw ax", "sahf", "jp again",
                                 "and eax, 0x4700"])
        add(f"{op}_loop_wait", two + [f"again: {op}", "fwait", "fnstsw ax",
                                      "sahf", "jp again", "fstp st(1)",
                                      "and eax, 0x4700"])
    status("fxam_c1_sw", two + ["fxam"], 0x4700)
    add("fnstsw_m16", three + ["fcomp st(1)", "fnstsw word ptr [eax+32]",
                               "and word ptr [eax+32], 0x4500"])

    compares = {
        "fcom_m32": ["fcom dword ptr [eax+24]"],
        "fcomp_m32": ["fcomp dword ptr [eax+24]"],
        "fcom_m64": ["fcom qword ptr [eax+16]"],
        "fcomp_m64": ["fcomp qword ptr [eax+16]"],
        "fcom_st1": ["fcom st(1)"], "fcomp_st1": ["fcomp st(1)"],
        "fcomp_st2": ["fcomp st(2)"], "fcompp": ["fcompp"],
        "fucom_st1": ["fucom st(1)"], "fucomp_st1": ["fucomp st(1)"],
        "fucompp": ["fucompp"],
        "ficom_m32": ["ficom dword ptr [eax+28]"],
        "ficomp_m16": ["ficomp word ptr [eax+28]"],
        "ftst": ["ftst"], "fxam": ["fxam"],
    }
    for name, cmp in compares.items():
        pre = three + cmp
        status(f"{name}_sw", pre)
        for mask in (0x01, 0x04, 0x05, 0x40, 0x41, 0x44, 0x45):
            for cc in ("e", "ne", "p", "np"):
                add(f"{name}_t{mask:02x}_j{cc}",
                    pre + ["fnstsw ax", f"test ah, {mask:#x}"] + _jump(cc))
                if name in ("fcomp_m32", "fcompp"):
                    add(f"{name}_t{mask:02x}_set{cc}",
                        pre + ["fnstsw ax", f"test ah, {mask:#x}"] + _set(cc))
        if name in ("fcomp_m32", "fcom_st1", "fcompp", "fucompp", "ftst"):
            for cc in CC16:
                add(f"{name}_sahf_j{cc}", pre + ["fnstsw ax", "sahf"] + _jump(cc))
                add(f"{name}_sahf_set{cc}", pre + ["fnstsw ax", "sahf"] + _set(cc))
    # Shapes MSVC emits between the compare and the branch.
    add("seq_fstp_between", three + ["fcomp st(1)", "fnstsw ax", "fstp st(0)",
                                     "test ah, 0x41"] + _jump("ne"))
    add("seq_fld_between", three + ["fcomp st(1)", "fld1", "fnstsw ax",
                                    "test ah, 0x41"] + _jump("e"))
    add("seq_fst_between", three + ["fcom st(1)", "fst dword ptr [eax+32]",
                                    "fnstsw ax", "test ah, 5"] + _jump("p"))
    add("seq_mov_after_test", three + ["fcomp st(1)", "fnstsw ax",
                                       "test ah, 0x44", "mov edx, 7", "jp done",
                                       "mov edx, 3", "done: mov eax, edx"])
    add("seq_and_cmp_e", three + ["fcomp st(1)", "fnstsw ax", "and ah, 0x45",
                                  "cmp ah, 0x40"] + _jump("e"))
    add("seq_and_cmp_ne", three + ["fcompp", "fnstsw ax", "and ah, 0x41",
                                   "cmp ah, 1"] + _jump("ne"))
    add("seq_two_tests", three + ["fcom st(1)", "fnstsw ax", "mov ecx, 1",
                                  "test ah, 0x41", "jne done", "mov ecx, 2",
                                  "test ah, 5", "jp done", "mov ecx, 3",
                                  "done: mov eax, ecx"])
    add("seq_sahf_mov_jae", three + ["fcompp", "fnstsw ax", "sahf",
                                     "mov eax, 0", "jae done", "mov eax, 1",
                                     "done: mov ecx, eax"])
    add("seq_wait_sahf_jne", three + ["fcompp", "fwait", "fnstsw ax", "sahf"] + _jump("ne"))
    add("seq_fptan_sahf_jp", two + ["fptan", "fwait", "fnstsw ax", "sahf"] + _jump("p"))

    for cmp in ("fcomi", "fcomip", "fucomi", "fucomip"):
        pre = two + [f"{cmp} st, st(1)"]
        for cc in CC16:
            add(f"{cmp}_j{cc}", pre + _jump(cc))
            add(f"{cmp}_set{cc}", pre + _set(cc))
            add(f"{cmp}_cmov{cc}", pre + _cmov(cc))
    add("fcomi_then_ftst", two + ["fcomi st, st(1)", "ftst"] + _set("a"))

    # SSE: [0..15] four floats a, [16..31] four floats b, [32..63] output.
    sse = ["movups xmm0, xmmword ptr [eax]", "movups xmm1, xmmword ptr [eax+16]"]
    for op in ("add", "sub", "mul", "div", "min", "max", "sqrt", "rcp", "rsqrt"):
        for w in ("ss", "ps"):
            mem = ("dword" if w == "ss" else "xmmword") + " ptr [eax+16]"
            add(f"{op}{w}_reg", sse + [f"{op}{w} xmm0, xmm1"], "sse")
            add(f"{op}{w}_mem", sse + [f"{op}{w} xmm0, {mem}"], "sse")
    for op in ("andps", "andnps", "orps", "xorps", "unpcklps", "unpckhps"):
        add(f"{op}_reg", sse + [f"{op} xmm0, xmm1"], "sse")
        add(f"{op}_mem", sse + [f"{op} xmm0, xmmword ptr [eax+16]"], "sse")
    for op in ("movlhps", "movhlps"):
        add(op, sse + [f"{op} xmm0, xmm1"], "sse")
    add("xorps_self", sse + ["xorps xmm0, xmm0"], "sse")
    for imm in range(256):
        add(f"shufps_{imm}", sse + [f"shufps xmm0, xmm1, {imm}"], "sse")
    add("shufps_mem", sse + ["shufps xmm0, xmmword ptr [eax+16], 0x1b"], "sse")
    add("shufps_self", sse + ["shufps xmm0, xmm0, 0x4e"], "sse")
    for w in ("ss", "ps"):
        for pred in range(8):
            add(f"cmp{w}_{pred}", sse + [f"cmp{w} xmm0, xmm1, {pred}",
                                         "movmskps eax, xmm0"], "sse")
    add("cmpneqps_mem", sse + ["cmpneqps xmm0, xmmword ptr [eax+16]",
                               "movmskps eax, xmm0"], "sse")
    for name, asm in (
            ("movaps_load", ["movaps xmm2, xmmword ptr [eax+16]"]),
            ("movaps_store", sse + ["movaps xmmword ptr [eax+32], xmm1"]),
            ("movaps_reg", sse + ["movaps xmm0, xmm1"]),
            ("movups_store", sse + ["movups xmmword ptr [eax+32], xmm1"]),
            ("movntps_store", sse + ["movntps xmmword ptr [eax+32], xmm1"]),
            ("movss_load", sse + ["movss xmm0, dword ptr [eax+20]"]),
            ("movss_reg", sse + ["movss xmm0, xmm1"]),
            ("movss_store", sse + ["movss dword ptr [eax+32], xmm1"]),
            ("movlps_load", sse + ["movlps xmm0, qword ptr [eax+16]"]),
            ("movhps_load", sse + ["movhps xmm0, qword ptr [eax+16]"]),
            ("movlps_store", sse + ["movlps qword ptr [eax+32], xmm1"]),
            ("movhps_store", sse + ["movhps qword ptr [eax+32], xmm1"]),
            ("cvtsi2ss", sse + ["cvtsi2ss xmm0, dword ptr [eax+16]"]),
            ("cvtsi2ss_reg", sse + ["mov ecx, dword ptr [eax+16]", "cvtsi2ss xmm0, ecx"]),
            ("cvtss2si", sse + ["cvtss2si ecx, xmm1", "mov eax, ecx"]),
            ("cvttss2si", sse + ["cvttss2si ecx, xmm1", "mov eax, ecx"]),
            ("cvtss2si_mem", sse + ["cvtss2si ecx, dword ptr [eax+16]", "mov eax, ecx"]),
            ("cvtps2dq", sse + ["cvtps2dq xmm0, xmm1"]),
            ("cvttps2dq", sse + ["cvttps2dq xmm0, xmm1"]),
            ("cvtdq2ps", sse + ["cvtdq2ps xmm0, xmm1"]),
            ("cvtsi2sd", sse + ["cvtsi2sd xmm0, dword ptr [eax+16]"]),
            ("cvtsd2si", sse + ["cvtsd2si ecx, xmm1", "mov eax, ecx"]),
            ("cvttsd2si", sse + ["cvttsd2si ecx, xmm1", "mov eax, ecx"]),
            ("cvtsd2ss", sse + ["cvtsd2ss xmm0, xmm1"]),
            ("cvtss2sd", sse + ["cvtss2sd xmm0, xmm1"]),
            ("cvtpi2ps_mem", sse + ["cvtpi2ps xmm0, qword ptr [eax+16]"]),
            ("cvtpi2ps_reg", sse + ["movq mm1, qword ptr [eax+16]",
                                    "cvtpi2ps xmm0, mm1", "emms"]),
            ("cvtps2pi", sse + ["cvtps2pi mm0, xmm1",
                                "movq qword ptr [eax+32], mm0", "emms"]),
            ("cvttps2pi", sse + ["cvttps2pi mm0, xmm1",
                                 "movq qword ptr [eax+32], mm0", "emms"])):
        add(name, asm, "sse")
    # SSE2 scalar doubles (absent from Xbox code; the Pentium III lacks SSE2).
    for op in ("add", "sub", "mul", "div", "min", "max", "sqrt"):
        add(f"{op}sd", sse + [f"{op}sd xmm0, xmm1"], "sse")
    for cmp in ("comisd", "ucomisd"):
        for cc in ("a", "b", "e", "ne", "p", "np", "le", "g"):
            add(f"{cmp}_set{cc}", sse + [f"{cmp} xmm0, xmm1"] + _set(cc), "sse")
    for cmp in ("comiss", "ucomiss"):
        for cc in CC16:
            add(f"{cmp}_set{cc}", sse + [f"{cmp} xmm0, xmm1"] + _set(cc), "sse")
            add(f"{cmp}_j{cc}", sse + [f"{cmp} xmm0, xmm1"] + _jump(cc), "sse")
        add(f"{cmp}_mem_jb", sse + [f"{cmp} xmm0, dword ptr [eax+16]"] + _jump("b"), "sse")

    # MMX (SSE-integer forms included): mm0 = [0], mm1 = [16], out = [32].
    mmx = ["movq mm0, qword ptr [eax]", "movq mm1, qword ptr [eax+16]"]
    keep = ["movq qword ptr [eax+32], mm0", "emms"]
    for op in ("paddb", "paddw", "paddd", "paddsw", "paddusb", "paddusw",
               "psubb", "psubw", "psubd", "psubsw", "psubusb", "psubusw",
               "pmulhw", "pmullw", "pmaddwd", "pmulhuw", "packuswb", "packsswb",
               "packssdw", "punpcklbw", "punpcklwd", "punpckldq", "punpckhbw",
               "punpckhwd", "punpckhdq", "pavgb", "pavgw", "pcmpgtb", "pcmpgtw",
               "pcmpgtd", "pcmpeqb", "pcmpeqw", "pcmpeqd", "pand", "pandn", "por",
               "pxor", "pmaxsw", "pminsw", "pmaxub", "pminub", "psadbw"):
        add(f"{op}_reg", mmx + [f"{op} mm0, mm1"] + keep, "sse")
        add(f"{op}_mem", mmx + [f"{op} mm0, qword ptr [eax+16]"] + keep, "sse")
    for op in ("punpcklbw", "punpcklwd"):
        add(f"{op}_m32", mmx + [f"{op} mm0, dword ptr [eax+16]"] + keep, "sse")
    for op in ("psraw", "psrad", "psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq"):
        for count in (0, 1, 7, 15, 16, 31, 32, 63, 64, 255):
            add(f"{op}_{count}", mmx + [f"{op} mm0, {count}"] + keep, "sse")
        add(f"{op}_mm", mmx + [f"{op} mm0, mm1"] + keep, "sse")
    for imm in range(4):
        add(f"pextrw_{imm}", mmx + [f"pextrw ecx, mm0, {imm}", "mov eax, ecx", "emms"], "sse")
    for imm in (0x1b, 0x4e, 0xe4, 0x00):
        add(f"pshufw_{imm}", mmx + [f"pshufw mm0, mm1, {imm}"] + keep, "sse")
    add("movd_store", mmx + ["movd dword ptr [eax+32], mm1", "emms"], "sse")
    add("movd_load", mmx + ["movd mm0, dword ptr [eax+20]"] + keep, "sse")
    add("movd_gpr", mmx + ["movd ecx, mm1", "mov eax, ecx", "emms"], "sse")
    add("movq_reg", mmx + ["movq mm0, mm1"] + keep, "sse")
    add("movntq_store", mmx + ["movntq qword ptr [eax+32], mm1", "emms"], "sse")
    return out


def _f32(x):
    try:
        return struct.unpack("<f", struct.pack("<f", x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def inputs(kind, random_count=96):
    """64-byte scratch images, the same for every case of a kind."""
    rng = random.Random(0xF9D1FF)
    if kind == "fpu":
        edges = [0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 1.5, 2.5, -2.5, 3.0, 0.1,
                 -7.75, 10.5, 1e-310, -1e-310, 2.0 ** -1074, 2.0 ** -1022,
                 1e-40, 2.0 ** 24 + 1, 2.0 ** 31 - 0.5, -2.0 ** 31 - 0.5,
                 2.0 ** 31, -2.0 ** 31, 2.0 ** 63, -2.0 ** 63, 1e300, -1e300,
                 3.4e38, math.inf, -math.inf, math.nan]
        pairs = [(a, b) for a in edges for b in edges]
        for _ in range(random_count):
            a = _f32(rng.uniform(-100, 100))
            pairs += [(a, rng.uniform(-100, 100)), (a, a), (a, _f32(-a))]
        ints = [0, 1, -1, 2, 7, -3, 32767, -32768, 65535, 2 ** 31 - 1,
                -2 ** 31, 100000, -1234567]
        rows = []
        for n, (a, b) in enumerate(pairs):
            ia = ints[n % len(ints)]
            ib = ints[(n // len(ints)) % len(ints)] if n % 3 else rng.getrandbits(31)
            rows.append(struct.pack("<dfidfi", a, _f32(a), ia, b, _f32(b), ib)
                        + bytes(32))
        return rows
    edges = [0, 0x80000000, 0x3F800000, 0xBF800000, 0x3F000000, 0x3FC00000,
             0x40200000, 0xC0200000, 1, 0x80000001, 0x007FFFFF, 0x00800000,
             0x7F7FFFFF, 0x4B000000, 0x4F000000, 0xCF000000, 0xCF000001,
             0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7FA00001,
             0x7FFF8000, 0x80017FFF, 0x00FF00FF, 0xFFFFFFFF]
    pairs = [([a, b, a, b], [b, a, b, a]) for a in edges for b in edges]
    pairs += [([rng.getrandbits(32) for _ in range(4)],
               [rng.getrandbits(32) for _ in range(4)]) for _ in range(random_count)]
    pairs += [([f, f, g, g], [g, f, f, g]) for f, g in
              (struct.unpack("<2I", struct.pack("<2f", rng.uniform(-9, 9),
                                                rng.uniform(-9, 9)))
               for _ in range(random_count))]
    return [struct.pack("<8I", *a, *b) + bytes(32) for a, b in pairs]


def translate(code, name):
    """C for the snippet, through the same translator recomp uses."""
    image = bytes(code) + b"\xC3"   # ret
    config._install([config.Section(".text", BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin="fpdiff")
    db = {BASE: {"start": hex(BASE), "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    text = FunctionTranslator(image, db).translate_function(BASE, db[BASE])
    return text.replace(f"sub_{BASE:08X}", f"fn_{name}")


_SUPPORT = r"""
static unsigned char saved[64], native_mem[64];
static int unsupported, g_exact;
void recomp_unimpl(const char *text, uint32_t va) { (void)text; (void)va; unsupported++; }

static int same_d(double a, double b, double tol) {
    if (memcmp(&a, &b, sizeof a) == 0 || (a != a && b != b)) return 1;
    if (tol > 0.0 && a == a && b == b) {
        double d = fabs(a - b), s = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
        return d <= tol * (s > 1.0 ? s : 1.0);
    }
    return 0;
}
static int same_f(const unsigned char *x, const unsigned char *y) {
    float a, b;
    if (memcmp(x, y, 4) == 0) return 1;
    memcpy(&a, x, 4); memcpy(&b, y, 4);
    return !g_exact && a != a && b != b;
}
/* Byte offset of the first scratch difference, or -1. NaN payloads match. */
static int diff_mem(void) {
    int i;
    for (i = 0; i < 64; i += 4) {
        double a, b; int j = i & ~7;
        if (same_f(native_mem + i, g_scratch + i)) continue;
        memcpy(&a, native_mem + j, 8); memcpy(&b, g_scratch + j, 8);
        if (!g_exact && a != a && b != b) continue;
        return i;
    }
    return -1;
}
static int g_total, g_fail, g_unsup;
static void show_in(int fpu) {
    if (fpu) {
        double a, b; memcpy(&a, saved, 8); memcpy(&b, saved + 16, 8);
        printf("       in a=%.17g b=%.17g ia=%d ib=%d\n", a, b,
               *(int *)(saved + 12), *(int *)(saved + 28));
    } else {
        unsigned int *w = (unsigned int *)saved;
        printf("       in %08X %08X %08X %08X | %08X %08X %08X %08X\n",
               w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
}
static int check(const char *name, int vec, int fpu, double tol, int *shown) {
    char why[160] = "";
    int i, m;
    if (g_out_eax != l_eax)
        sprintf(why, "eax native=%08X lifted=%08X", g_out_eax, l_eax);
    else if (fpu && n_depth != l_depth)
        sprintf(why, "depth native=%d lifted=%d", n_depth, l_depth);
    if (!why[0] && fpu)
        for (i = 0; i < n_depth; i++)
            if (!same_d(n_st[i], l_st[i], tol)) {
                sprintf(why, "st(%d) native=%.17g lifted=%.17g", i, n_st[i], l_st[i]);
                break;
            }
    if (!why[0] && !fpu)
        for (i = 0; i < 32; i++)
            if (!same_f(g_out_xmm + i * 4, l_xmm + i * 4)) {
                sprintf(why, "xmm%d[%d] native=%08X lifted=%08X", i / 4, i % 4,
                        *(unsigned int *)(g_out_xmm + i * 4), *(unsigned int *)(l_xmm + i * 4));
                break;
            }
    if (!why[0] && (m = diff_mem()) >= 0)
        sprintf(why, "mem[%d] native=%08X lifted=%08X", m,
                *(unsigned int *)(native_mem + m), *(unsigned int *)(g_scratch + m));
    if (!why[0]) return 0;
    if (++*shown <= 3) { printf("  %s vec %d: %s\n", name, vec, why); show_in(fpu); }
    return 1;
}
"""


def source(prepared, prelude=None):
    preamble = harness._PREAMBLE
    if prelude:
        preamble = prelude + preamble[preamble.index("\nextern unsigned int g_in_a"):]
    out = [preamble, _SUPPORT]
    for c, _, _ in prepared:
        out.append(f"void nat_{c['name']}(void);")
    for c, text, _ in prepared:
        kind = c["kind"]
        pro = harness._LIFTED_PROLOGUE[kind].replace("@FPMACROS@", "")
        epi = harness._LIFTED_EPILOGUE[kind].replace("@FPUNDEFS@", "")
        if kind == "fpu":
            pro = pro.replace("g_fp_cc = 0x4000", "g_fp_cc = 0")
        out.append(text)
        out.append(f"static void lif_{c['name']}(void) {{\n{pro}\n"
                   "    g_esp = (uint32_t)(uintptr_t)(g_guest_stack + sizeof(g_guest_stack) / 2);\n"
                   f"    fn_{c['name']}();\n{epi}\n}}")
    for kind in ("fpu", "sse"):
        rows = ",\n".join("{" + ",".join(f"0x{v:02x}" for v in row) + "}"
                          for row in inputs(kind))
        out.append(f"static const unsigned char input_{kind}[][64] = {{\n{rows}\n}};")
    body = ["int main(void) {", "    int vec, shown, fails, runs; g_scratch_ptr = g_scratch;"]
    for c, _, unsup in prepared:
        name, kind, fpu = c["name"], c["kind"], int(c["kind"] == "fpu")
        body.append(f"""    shown = 0; fails = 0; unsupported = 0; runs = 0; g_exact = {c['exact']};
    for (vec = 0; vec < (int)(sizeof input_{kind} / 64); ++vec) {{
        double a, b;
        memcpy(saved, input_{kind}[vec], 64); memcpy(g_scratch, saved, 64);
        memcpy(&a, saved, 8); memcpy(&b, saved + 16, 8);
        if (!({c['domain']})) continue;
        runs++;
        _mm_setcsr(0x1f80); nat_{name}(); memcpy(native_mem, g_scratch, 64);
        {"n_depth = (8 - ((g_out_sw >> 11) & 7)) & 7; memcpy(n_st, g_out_st, sizeof n_st);" if fpu else ""}
        _mm_setcsr(0x1f80); memcpy(g_scratch, saved, 64); lif_{name}(); g_total++;
        fails += check("{name}", vec, {fpu}, {c['tol']!r}, &shown);
    }}
    g_fail += fails; if ({unsup} || unsupported) g_unsup++;
    printf("RESULT {name} %d %d%s\\n", fails, runs, ({unsup} || unsupported) ? " UNSUPPORTED" : "");""")
    body.append('    printf("%d vectors, %d mismatches, %d unsupported cases\\n", g_total, g_fail, g_unsup);')
    body.append("    return g_fail != 0 || g_unsup != 0;\n}")
    out.append("\n".join(body))
    return "\n".join(out)


_UNSUPPORTED = ("RECOMP_UNIMPL", "/* FPU:", "unhandled", "unsupported")


def run(out, corpus, include=None, prelude=None):
    vcvars = runner._find_vcvars()
    if vcvars is None:
        raise RuntimeError("32-bit MSVC is required (same as tools.conformance)")
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / "native.c").write_text(harness.native_source(corpus), newline="\n")
    built = runner._cl(vcvars, str(out), "/c /FAc /Fanative.cod native.c")
    if built.returncode:
        raise RuntimeError(built.stdout + built.stderr)
    listing = (out / "native.cod").read_text()
    prepared = []
    for c in corpus:
        text = translate(runner._bytes_from_listing(listing, c["name"]), c["name"])
        prepared.append((c, text, int(any(s in text for s in _UNSUPPORTED))))
    (out / "lifted.c").write_text(source(prepared, prelude), newline="\n")
    include = (Path(include).resolve() if include
               else Path(__file__).resolve().parents[2] / "templates" / "runtime")
    built = runner._cl(vcvars, str(out),
                       f'/O2 /fp:strict /arch:SSE2 /I"{include}" lifted.c native.obj /Fecompare.exe')
    (out / "compile.txt").write_text(built.stdout + built.stderr)
    if built.returncode:
        raise RuntimeError(built.stdout + built.stderr)
    checked = subprocess.run([str(out / "compare.exe")], capture_output=True,
                             text=True, timeout=600)
    (out / "results.txt").write_text(checked.stdout + checked.stderr)
    return checked


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("-k", default="", help="only cases whose name contains this")
    ap.add_argument("--include", type=Path, help="directory with recomp_types.h")
    ap.add_argument("--prelude", type=Path, help="C replacing the harness register globals")
    args = ap.parse_args()
    corpus = [c for c in cases() if args.k in c["name"]]
    prelude = args.prelude.read_text() if args.prelude else None
    result = run(args.out, corpus, args.include, prelude)
    print("\n".join(l for l in result.stdout.splitlines()
                    if not l.startswith("RESULT") or " 0 " not in l
                    or l.endswith("UNSUPPORTED")))
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
