"""Compile a C corpus, link it, lift the machine code back, and compare.

Working from the *linked* image rather than the .obj is what lets a corpus
function use anything real. The linker has already resolved every address, so a
jump table, a float constant in .rdata and a call to a CRT helper are numbers by
the time we see them -- no relocation handling needed, and the same shape a real
XBE arrives in.

The DLL is linked at a fixed base with relocations stripped, and the harness maps
it there with VirtualAlloc, so a guest address is a host address and the lifted
code's MEM32() reads the real bytes.
"""

import re

from . import image

IMAGE_BASE = 0x30000000

# A call the generated C makes: either an address the recompiler named
# itself, or one of our lifted symbols.
_CALLEE_RE = '\\b(sub_[0-9A-Fa-f]{8}|lifted_\\w+)\\s*\\('

_SIG = {
    "ii->i": {"ctype": "int",       "arg": "int",      "push": "i", "ret": "eax"},
    "uu->u": {"ctype": "unsigned",  "arg": "unsigned", "push": "i", "ret": "eax"},
    "ll->l": {"ctype": "long long", "arg": "long long","push": "l", "ret": "edx:eax"},
    "dd->d": {"ctype": "double",    "arg": "double",   "push": "d", "ret": "st0"},
    "ff->f": {"ctype": "float",     "arg": "float",    "push": "f", "ret": "st0"},
}

_PUSH = {
    "i": "    { uint32_t _v = (uint32_t)a{n}; sp -= 4; memcpy(sp, &_v, 4); }",
    "l": "    { uint64_t _v = (uint64_t)a{n}; sp -= 8; memcpy(sp, &_v, 8); }",
    "d": "    { double   _v = a{n};           sp -= 8; memcpy(sp, &_v, 8); }",
    "f": "    { float    _v = a{n};           sp -= 4; memcpy(sp, &_v, 4); }",
}

_READ = {
    "eax":     "(int)g_eax",
    "edx:eax": "(long long)(((uint64_t)g_edx << 32) | g_eax)",
    "st0":     "g_fp_stack[g_fp_top & 7]",
}


def lift_all(dll_bytes, map_text, wanted):
    """Lift each wanted function, following calls into whatever they reach.

    A corpus function that calls a CRT helper needs that helper lifted too --
    __allmul, __ftol2 and friends are just more code in .text, and lifting them
    is exactly what a real port does rather than a special case here.

    Returns (ordered [(symbol, C)], sections, image_base).
    """
    from tools.recomp import config
    from tools.recomp.translator import FunctionTranslator

    base, symbols = image.parse_map(map_text)
    pe_base, sections = image.parse_pe(dll_bytes)
    if base != pe_base:
        raise RuntimeError(f"map base {base:#x} != PE base {pe_base:#x}")

    # Every function the linker placed, so a call lifts to a named callee
    # rather than an address the harness has never heard of.
    func_db, extents = {}, {}
    for sym in symbols:
        try:
            start, end, sec = image.function_extent(symbols, sections, sym.name)
        except RuntimeError:
            continue
        if not sec.is_code:
            continue
        code = image.trim_padding(image.section_bytes(dll_bytes, sec, start, end))
        if not code:
            continue
        extents[sym.name] = (start, start + len(code), code)
        func_db[start] = {"start": f"0x{start:08X}", "end": start + len(code),
                          "_addr": start, "size": len(code),
                          "name": _c_name(sym.name)}

    saved = {k: getattr(config, k) for k in _CONFIG_GLOBALS if hasattr(config, k)}
    try:
        config._install(
            [config.Section(s.name, s.va, s.vsize, s.raw_off, s.raw_size,
                            s.is_code) for s in sections],
            entry_point=base, kernel_thunk_addr=base, origin="conformance-corpus")
        translator = FunctionTranslator(dll_bytes, func_db)

        todo = [n for n in wanted if n in extents]
        missing = [n for n in wanted if n not in extents]
        done, order, unlifted = set(), [], {}
        while todo:
            name = todo.pop(0)
            if name in done:
                continue
            done.add(name)
            start, end, code = extents[name]
            body = translator.translate_function(start, func_db[start])
            order.append((name, body))
            # An instruction the lifter did not handle becomes a bare comment.
            # That is a silent no-op: the comparison can still pass while the
            # instruction does nothing, so it has to be reported separately.
            for line in body.splitlines():
                s = line.strip()
                if s.startswith("/* TODO") or s.startswith("/* FPU:"):
                    unlifted.setdefault(name, []).append(s)
            # follow the calls this function makes
            for target in _call_targets(code, start):
                sym = next((s.name for s in symbols if s.va == target), None)
                if sym and sym in extents and sym not in done:
                    todo.append(sym)
    finally:
        for k, v in saved.items():
            setattr(config, k, v)
    return (order, sections, base, missing,
            {n: e[0] for n, e in extents.items()}, unlifted)


def _call_targets(code, start):
    """Where a block of code transfers control to, by a direct call or a jump.

    Tail jumps count: the CRT helpers reach each other that way (__ftol2_sse
    ends in a jump to _ftoi2), and following only `call` leaves the callee
    unlifted and the harness with an unresolved symbol.
    """
    from tools.recomp.disasm import Disassembler
    d = Disassembler()
    out = []
    for insn in d._cs.disasm(code, start):
        if insn.mnemonic in ("call", "jmp") and insn.op_str.startswith("0x"):
            try:
                out.append(int(insn.op_str, 16))
            except ValueError:
                pass
    return out


def _c_name(sym):
    """A C identifier for a linker symbol (`_t_mul64` -> `lifted_t_mul64`)."""
    return "lifted_" + re.sub(r"[^A-Za-z0-9_]", "_", sym.lstrip("_"))


_CONFIG_GLOBALS = (
    "_SECTIONS", "SECTIONS", "_configured_from",
    "TEXT_VA_START", "TEXT_VA_END", "RDATA_VA_START", "RDATA_VA_END",
    "DATA_VA_START", "DATA_VA_END", "KERNEL_THUNK_ADDR", "ENTRY_POINT",
)


_PREAMBLE = """/* generated -- whole-function corpus: compiled C vs lifted C */
#define RECOMP_GENERATED_CODE 1
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <windows.h>
#include "recomp_types.h"

RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
RECOMP_TLS uint32_t g_seh_ebp, g_ebp;
RECOMP_TLS int g_df;   /* EFLAGS.DF: the direction the string ops walk */
RECOMP_TLS uint32_t g_eflags;
RECOMP_TLS double g_fp_stack[8]; RECOMP_TLS int g_fp_top;
RECOMP_TLS uint16_t g_fp_control_word = 0x027F; RECOMP_TLS uint32_t g_mxcsr = 0x1F80; RECOMP_TLS int g_fp_cmp; RECOMP_TLS uint16_t g_fp_cc = 0x4000;
RECOMP_TLS RecompXmm g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7;
RECOMP_TLS RecompMmx g_mm0,g_mm1,g_mm2,g_mm3,g_mm4,g_mm5,g_mm6,g_mm7;
volatile uint32_t g_icall_trace[16]; volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count;
ptrdiff_t g_xbox_mem_offset;
void recomp_icall_fail_log(uint32_t va) { (void)va; }

"""

_PREAMBLE_TAIL = """/* The corpus image is mapped at the address it was linked for, so a guest
   address is a host address and g_xbox_mem_offset stays 0. That is what lets
   the lifted code read a float constant out of .rdata, or index a jump table,
   without any relocation handling anywhere in the pipeline. */
static void map_guest_image(void) {
    void *want = (void *)(uintptr_t)GUEST_IMAGE_BASE;
    void *got = VirtualAlloc(want, GUEST_IMAGE_SPAN,
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (got != want) {
        printf("could not map the guest image at %p (got %p)@NL@", want, got);
        exit(2);
    }
    load_guest_sections();
    g_xbox_mem_offset = 0;
}

/* The guest stack. Also a host address, for the same reason. */
static unsigned char g_stack[256 * 1024];
#define GUEST_SP_TOP (g_stack + sizeof(g_stack) - 4096)

static int g_total, g_fail;

/* A lifted function is void(void) and takes its arguments off the guest stack,
   right to left, under a return address -- __cdecl, which is what the
   recompiler's own generated code assumes. Setting that up here is the point:
   it exercises the real calling path rather than a special one. */
static void enter(void) {
    g_fp_top = 0; g_fp_cmp = 0; g_fp_cc = 0x4000; g_fp_control_word = 0x027Fu; g_mxcsr = 0x1F80u;
    memset(g_fp_stack, 0, sizeof g_fp_stack);
    g_eax = g_ecx = g_edx = g_ebx = g_esi = g_edi = 0;
    g_seh_ebp = g_ebp = 0;
    g_df = 0;   /* the ABI hands every function DF clear */
}
"""

_REPORT = """
static void fail(const char *name, const char *why, int *shown, int vec) {
    if (!*shown) printf("FAIL %s  (%s)@NL@", name, why);
    (*shown)++;
    g_fail++;
    (void)vec;
}

static int same_d(double a, double b, double tol) {
    if (memcmp(&a, &b, sizeof a) == 0) return 1;
    if (isnan(a) && isnan(b)) return 1;
    if (tol > 0.0 && !isnan(a) && !isnan(b)) {
        double d = fabs(a - b), s = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
        return d <= tol * (s > 1.0 ? s : 1.0);
    }
    return 0;
}
"""


def _section_blob(dll_bytes, sections, base):
    """The guest image, as C arrays placed at their virtual addresses."""
    span = max(s.va + s.vsize for s in sections) - base
    out = [f"#define GUEST_IMAGE_BASE 0x{base:08X}u",
           f"#define GUEST_IMAGE_SPAN 0x{span:X}u", ""]
    loads = []
    for i, s in enumerate(sections):
        raw = dll_bytes[s.raw_off:s.raw_off + s.raw_size]
        if not raw:
            continue
        rows = ",".join(str(b) for b in raw)
        out.append(f"/* {s.name} */")
        out.append(f"static const unsigned char g_sec{i}[] = {{{rows}}};")
        loads.append(f"    memcpy((void *)(uintptr_t)0x{s.va:08X}u, g_sec{i}, "
                     f"sizeof g_sec{i});")
    out.append("")
    out.append("static void load_guest_sections(void) {")
    out.extend(loads)
    out.append("}")
    return chr(10).join(out)


def harness_source(entries, lifted, dll_bytes, sections, base, addr_of):
    """entries: corpus Fn dicts under test. lifted: ordered [(symbol, C)]."""
    # includes first, then the image arrays, then the code that uses them
    out = [_PREAMBLE, _section_blob(dll_bytes, sections, base),
           _PREAMBLE_TAIL]
    for fn in entries:
        s = _SIG[fn["sig"]]
        out.append(f'extern {s["ctype"]} __cdecl {fn["name"]}'
                   f'({s["arg"]}, {s["arg"]});')
    # Forward declarations, before any body: a lifted function may call one
    # defined below it, and an undeclared call is assumed to return int, which
    # then clashes with its own definition further down.
    defined = {_c_name(sym) for sym, _ in lifted}
    referenced = set()
    for _, body in lifted:
        referenced.update(re.findall(_CALLEE_RE, body))
    orphans = sorted(referenced - defined)
    for name in sorted(defined) + orphans:
        out.append(f"void {name}(void);")
    out.append("")
    for _, body in lifted:
        out.append(body)
    # The generated code dispatches an unresolved indirect branch through
    # recomp_lookup*. A resolved jump table never reaches it, so a hit here
    # means the lift got something wrong -- say so loudly rather than
    # returning NULL and letting the run continue with a silently skipped
    # call, which is how the real runtime's stub behaves.
    out.append("typedef void (*recomp_func_t)(void);")
    out.append("static const struct { uint32_t va; recomp_func_t fn; } "
               "g_dispatch[] = {")
    for sym, _ in lifted:
        out.append(f"    {{ 0x{addr_of[sym]:08X}u, {_c_name(sym)} }},")
    out.append("};")
    out.append("""
recomp_func_t recomp_lookup(uint32_t va) {
    size_t i;
    for (i = 0; i < sizeof g_dispatch / sizeof g_dispatch[0]; i++)
        if (g_dispatch[i].va == va) return g_dispatch[i].fn;
    printf("indirect branch to %08X, which is not a lifted function@NL@", va);
    g_fail++;
    return 0;
}
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return 0; }
recomp_func_t recomp_lookup_kernel(uint32_t va) { (void)va; return 0; }
""")
    # A lifted body can name a callee nothing defines -- an address decoded out
    # of padding past a function's real end, or a helper the call-follower did
    # not reach. Stub it rather than fail the link, and make the stub loud: a
    # silent no-op here would let the comparison pass while a call went
    # nowhere, which is the failure this whole tool exists to catch.
    for name in orphans:
        out.append(f"void {name}(void) {{ printf(\"called {name}, which was "
                   f"never lifted@NL@\"); g_fail++; }}")

    out.append(_REPORT)
    prepared = [(fn, None) for fn in entries]

    for fn, _ in prepared:
        s = _SIG[fn["sig"]]
        pushes = "\n".join(_PUSH[s["push"]].replace("{n}", str(i))
                           for i in (1, 0))
        out.append(f"""
static void run_{fn['name']}({s['arg']} a0, {s['arg']} a1, int vec, int *shown) {{
    {s['ctype']} want, got;
    unsigned char *sp = GUEST_SP_TOP;
    want = {fn['name']}(a0, a1);
    enter();
{pushes}
    {{ uint32_t _ret = 0xDEADBEEFu; sp -= 4; memcpy(sp, &_ret, 4); }}
    g_esp = (uint32_t)(uintptr_t)sp;
    lifted_{fn['name']}();
    got = ({s['ctype']}){_READ[s['ret']]};
    g_total++;
    if ({_agree(fn, s)}) return;
    fail("{fn['name']}", "{fn['why']}", shown, vec);
    if (*shown <= 3) {_printf(fn, s)}
}}""")

    out.append("int main(void) {\n    int shown;")
    out.append("    map_guest_image();")
    out.append("    /* Match the model: it holds the x87 stack as C doubles, so\n"
               "       the hardware is put in double-precision mode too. */\n"
               "    { unsigned short cw = 0x027F; __asm { fldcw cw } }")
    for fn, _ in prepared:
        out.append(f"    shown = 0;   /* {fn['name']} */")
        for vec, (a, b) in enumerate(fn["args"]):
            out.append(f"    run_{fn['name']}({_lit(a, fn['sig'])}, "
                       f"{_lit(b, fn['sig'])}, {vec}, &shown);")
        out.append('    if (shown > 3) printf("       ... and %d more@NL@", '
                   'shown - 3);')
    out.append('    printf("@NL@%d function vectors, %d mismatches@NL@", '
               'g_total, g_fail);\n    return g_fail != 0;\n}')
    return "\n".join(out).replace("@NL@", chr(92) + "n")


def _agree(fn, s):
    if s["ret"] == "st0":
        return f"same_d((double)want, (double)got, {fn['tol']!r})"
    return "want == got"


def _printf(fn, s):
    if s["ret"] == "st0":
        return ('printf("       vec %-3d native=%.17g  lifted=%.17g@NL@",'
                ' vec, (double)want, (double)got);')
    if s["ret"] == "edx:eax":
        return ('printf("       vec %-3d native=%lld  lifted=%lld@NL@",'
                ' vec, (long long)want, (long long)got);')
    return ('printf("       vec %-3d native=%d (%08X)  lifted=%d (%08X)@NL@",'
            ' vec, (int)want, (unsigned)want, (int)got, (unsigned)got);')


def _lit(v, sig):
    if sig in ("dd->d",):
        return repr(float(v))
    if sig in ("ff->f",):
        return repr(float(v)) + "f"
    if sig == "ll->l":
        return f"{v}LL"
    if sig == "uu->u":
        return f"0x{v & 0xFFFFFFFF:08X}u"
    return f"({v})" if v < 0 else str(v)
