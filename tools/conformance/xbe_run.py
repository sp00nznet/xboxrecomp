"""Conformance against a real title: lift the game's own code and run both.

Xbox code is 32-bit x86 and this harness is a 32-bit x86 process, so the game's
machine code is not merely liftable, it is *executable*. Map the XBE where it
was linked for, call one of its functions directly, then run the lifted C over
the same arguments and compare. The oracle is the shipped binary itself.

That is only safe for functions we can show are self-contained, so candidates
are chosen mechanically rather than by hand:

  * a standard `push ebp; mov ebp, esp` prologue, and a plain `ret` -- not
    `ret imm16`, because a stdcall callee cleans a number of argument bytes we
    would have to guess, and guessing wrong corrupts the caller's stack;
  * no `call`, so nothing reaches the kernel, the CRT or an unmapped address;
  * every memory operand either esp/ebp-relative (its own frame and arguments)
    or an absolute address inside a mapped section, so it cannot dereference
    an argument we invented;
  * no `fs:` access (SEH), no string operations (they walk esi/edi as
    pointers), no privileged or I/O instructions;
  * and nothing that lifts to a bare comment or a RECOMP_UNIMPL call, since an
    unhandled instruction is a silent no-op that the comparison cannot see.

Arguments are small integers. A function that treated one as a pointer would
fault -- but the operand rule above already excludes those, and the native call
runs under an exception guard regardless.
"""

import re

from . import image

# push ebp ; mov ebp, esp -- the frame prologue MSVC emits for a function that
# is not FPO-optimised. Not every function looks like this, and that is fine:
# we need a supply of verifiable functions, not all of them.
_PROLOGUE = bytes((0x55, 0x8B, 0xEC))

# Any mention, not only a direct call: the lifter also passes a callee by
# name, as in RECOMP_ABI_CALL(0x0009748Cu, lifted_0009748C), and a callee seen
# only that way was never declared or stubbed, so the harness did not compile.
_LIFTED_RE = '\\blifted_([0-9A-Fa-f]{8})\\b'

_MAX_INSNS = 400
_MIN_BYTES, _MAX_BYTES = 12, 1200

# Instructions that make a function unsafe to call with arguments we invented,
# or that we cannot reason about. String operations are excluded because they
# walk esi/edi for a count in ecx: with a garbage count that is not a fault,
# it is a very long memcpy over whatever happens to be mapped.
_BANNED = {
    "int", "int1", "into", "in", "out", "hlt", "iret", "iretd",
    "sysenter", "sysexit", "rdmsr", "wrmsr", "lgdt", "lidt",
    "invd", "wbinvd", "ltr", "lldt", "arpl", "bound", "ud2",
}
_BANNED_PREFIX = ("rep", "lods", "stos", "movs", "scas", "cmps", "ins", "outs",
                  "loop")


def load(path):
    """(raw image bytes, [image.Section], base VA)."""
    from tools.xbe_parser.xbe_parser import XBEParser
    xbe = XBEParser(path).parse()
    sections = [
        image.Section(s.name, s.virtual_addr, max(s.virtual_size, s.raw_size),
                      s.raw_addr, s.raw_size, bool(s.flags & 0x4))
        for s in xbe.sections if s.raw_size
    ]
    if not sections:
        raise RuntimeError("no sections with raw data")
    return xbe.raw_data, sections, min(s.va for s in sections)


class Func:
    __slots__ = ("va", "end", "code", "plain_ret", "calls", "local_ok")

    def __init__(self, va, end, code, plain_ret, calls, local_ok):
        self.va, self.end, self.code = va, end, code
        self.plain_ret, self.calls, self.local_ok = plain_ret, calls, local_ok


def _mapped(sections, va):
    return any(s.va <= va < s.va + s.vsize for s in sections)


def _inspect(insns, start, end, sections):
    """(local_ok, direct call targets) for one decoded function.

    Memory through a register is allowed now: arguments include pointers into a
    scratch buffer that both sides see identically, the image is restored
    between runs so a write cannot leak into the next one, and anything that
    still goes somewhere unmapped faults and is skipped. What stays banned is
    what we cannot bound -- an indirect branch, a string operation walking a
    garbage count, a privileged instruction.
    """
    calls = []
    for insn in insns:
        m = insn.mnemonic
        if m in _BANNED or any(m.startswith(p) for p in _BANNED_PREFIX):
            return False, []
        text = insn.op_str
        if "fs:" in text or "gs:" in text:
            return False, []
        if m == "call":
            if not text.startswith("0x"):
                return False, []          # indirect: target unknown
            try:
                calls.append(int(text, 16))
            except ValueError:
                return False, []
            continue
        if m.startswith("j"):
            if not text.startswith("0x"):
                return False, []          # computed jump
            try:
                target = int(text, 16)
            except ValueError:
                return False, []
            if not (start <= target < end):
                return False, []          # leaves the function
    return True, calls


def _decode_at(data, sections, cs, va):
    """Decode one function starting at va, bounded by its first `ret`."""
    sec = next((s for s in sections
                if s.is_code and s.va <= va < s.va + s.raw_size), None)
    if sec is None:
        return None
    off = sec.raw_off + (va - sec.va)
    # Stopping at the first `ret` truncates any function with a block after it
    # -- an early return, or a loop laid out below the exit. Those then look
    # like they jump outside themselves and get rejected. Keep going while a
    # forward jump still points past where we stopped.
    insns, end, plain, furthest = [], None, None, va
    for insn in cs.disasm(data[off:off + _MAX_BYTES], va):
        insns.append(insn)
        if insn.mnemonic.startswith("j") and insn.op_str.startswith("0x"):
            try:
                furthest = max(furthest, int(insn.op_str, 16))
            except ValueError:
                pass
        if insn.mnemonic in ("ret", "retn"):
            plain = insn.op_str.strip() == ""
            end = insn.address + insn.size
            if furthest < end:
                break
            continue        # a later block is still reachable
        if len(insns) >= _MAX_INSNS:
            break
    if end is None or end - va < _MIN_BYTES or furthest >= end:
        return None
    ok, calls = _inspect(insns, va, end, sections)
    return Func(va, end, data[off:off + (end - va)], bool(plain), calls, ok)


def scan(data, sections, cs):
    """Every function we can decode, reached from a prologue or from a call.

    Seeding only from `push ebp; mov ebp, esp` finds the framed functions and
    stops there. An FPO callee has no such prologue, so its caller used to be
    dropped for calling into the unknown -- which is most of them. A direct
    `call` target is a function start by definition, so following those is both
    free and worth far more than the prologue scan alone.
    """
    seeds = []
    for sec in sections:
        if not sec.is_code or sec.name != ".text":
            continue
        blob = data[sec.raw_off:sec.raw_off + sec.raw_size]
        pos = 0
        while True:
            pos = blob.find(_PROLOGUE, pos)
            if pos < 0:
                break
            seeds.append(sec.va + pos)
            pos += 1

    funcs, todo, seen = {}, list(seeds), set(seeds)
    while todo:
        va = todo.pop()
        fn = _decode_at(data, sections, cs, va)
        if fn is None:
            continue
        funcs[va] = fn
        for target in fn.calls:
            if target not in seen:
                seen.add(target)
                todo.append(target)
    return funcs


def admissible(funcs):
    """Functions whose whole call tree we can lift.

    A function is only usable if everything it calls is too, so this is a
    fixpoint: start by assuming every locally-clean function qualifies, then
    drop any whose callee does not, and repeat until nothing changes.
    """
    ok = {va for va, f in funcs.items() if f.local_ok}
    changed = True
    while changed:
        changed = False
        for va in list(ok):
            for target in funcs[va].calls:
                if target not in ok:
                    ok.discard(va)
                    changed = True
                    break
    return ok


def find_candidates(data, sections, limit, cs):
    """(entries to call, every function to lift, entry -> its call closure)."""
    funcs = scan(data, sections, cs)
    ok = admissible(funcs)
    # Only a plain-`ret` function can be called from C: a stdcall callee cleans
    # argument bytes we would have to guess. Callees may be either -- the
    # lifter handles `ret imm16` itself.
    entries = sorted(va for va in ok if funcs[va].plain_ret)[:limit]

    closure = {}
    for va in entries:
        seen, todo = set(), [va]
        while todo:
            cur = todo.pop()
            if cur in seen:
                continue
            seen.add(cur)
            todo.extend(funcs[cur].calls)
        closure[va] = seen
    needed = sorted(set().union(*closure.values())) if closure else []

    def row(va):
        return (va, funcs[va].end - va, funcs[va].code)

    return ([row(va) for va in entries], [row(va) for va in needed], closure)


def lift(data, sections, candidates):
    """Lift each candidate through the real FunctionTranslator."""
    from tools.recomp import config
    from tools.recomp.translator import FunctionTranslator
    from .corpus_run import _CONFIG_GLOBALS

    func_db = {va: {"start": f"0x{va:08X}", "end": va + size, "_addr": va,
                    "size": size, "name": f"lifted_{va:08X}"}
               for va, size, _ in candidates}
    saved = {k: getattr(config, k) for k in _CONFIG_GLOBALS if hasattr(config, k)}
    out, rejected = [], []
    try:
        config._install(
            [config.Section(s.name, s.va, s.vsize, s.raw_off, s.raw_size,
                            s.is_code) for s in sections],
            entry_point=min(s.va for s in sections if s.is_code),
            kernel_thunk_addr=0, origin="conformance-xbe")
        tr = FunctionTranslator(data, func_db)
        for va, size, _ in candidates:
            body = tr.translate_function(va, func_db[va])
            # Untranslated instructions used to lift to a bare comment; they
            # now lift to a RECOMP_UNIMPL(...) call, which the harness does
            # not define. Either way the instruction is a no-op.
            gaps = [l.strip() for l in body.splitlines()
                    if l.strip().startswith(("/* TODO", "/* FPU:"))
                    or "RECOMP_UNIMPL(" in l]
            if gaps:
                # An unhandled instruction is a silent no-op: the comparison
                # would be meaningless, so say which one and move on.
                rejected.append((va, gaps))
                continue
            if re.search(r"\bsub_[0-9A-Fa-f]{8}\s*\(", body):
                rejected.append((va, ["calls a function outside the set"]))
                continue
            out.append((va, size, body))
    finally:
        for k, v in saved.items():
            setattr(config, k, v)
    return out, rejected


_HARNESS = '''/* generated -- a real title's own code, lifted and run against itself */
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
RECOMP_TLS uint32_t g_eflags;
RECOMP_TLS uint32_t g_seh_ebp, g_ebp;
RECOMP_TLS double g_fp_stack[8]; RECOMP_TLS int g_fp_top;
RECOMP_TLS uint16_t g_fp_control_word = 0x027F; RECOMP_TLS uint32_t g_mxcsr = 0x1F80; RECOMP_TLS int g_fp_cmp; RECOMP_TLS uint16_t g_fp_cc = 0x4000;
RECOMP_TLS RecompXmm g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7;
volatile uint32_t g_icall_trace[16]; volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count;
ptrdiff_t g_xbox_mem_offset;
void recomp_icall_fail_log(uint32_t va) { (void)va; }
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return 0; }
recomp_func_t recomp_lookup_kernel(uint32_t va) { (void)va; return 0; }

static unsigned char g_stack[256 * 1024];

/* Memory a function may be handed a pointer into. Both sides see the identical
   bytes, and it is restored before each run so one side cannot observe what
   the other wrote. */
#define SCRATCH_SIZE 0x1000
static unsigned char g_scratch[SCRATCH_SIZE];
static unsigned char g_scratch_seed[SCRATCH_SIZE];
static unsigned char g_scratch_native[SCRATCH_SIZE];

/* A pristine copy of every mapped section, so a function that writes through a
   pointer cannot leak state into the next run -- or into the other side of
   this one. */
static unsigned char *g_pristine[64];

static int g_total, g_fail, g_faulted;

static void reset_memory(void) {
    size_t i;
    for (i = 0; i < sizeof g_secs / sizeof g_secs[0]; i++)
        if (g_pristine[i])
            memcpy((void *)(uintptr_t)g_secs[i].va, g_pristine[i],
                   g_secs[i].raw_size);
    memcpy(g_scratch, g_scratch_seed, SCRATCH_SIZE);
}

/* SCRATCH_MARK + n in an argument vector means "scratch + n". The address is
   not known until run time, so the substitution happens here. */
#define SCRATCH_MARK 0x5C000000u
static uint32_t real_arg(uint32_t v) {
    if ((v & 0xFF000000u) == SCRATCH_MARK && (v & 0x00FFFFFFu) < SCRATCH_SIZE)
        return (uint32_t)(uintptr_t)(g_scratch + (v & 0x00FFFFFFu));
    return v;
}

/* The image is mapped where the XBE was linked for, so a guest address is a
   host address: the lifted code's MEM32() and the original machine code read
   the very same bytes. Executable, because the original code is *run*. */
static int commit(uintptr_t lo, uintptr_t hi) {
    /* Reserve in 64 KB granules. The low address space is not one contiguous
       free block, and adjacent XBE sections often share a granule, so a whole-
       image reservation fails while a per-granule one succeeds. */
    uintptr_t a;
    for (a = lo & ~(uintptr_t)0xFFFF; a < hi; a += 0x10000) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualAlloc((LPVOID)a, 0x10000, MEM_RESERVE | MEM_COMMIT,
                         PAGE_EXECUTE_READWRITE) == (LPVOID)a)
            continue;
        if (VirtualQuery((LPCVOID)a, &mbi, sizeof mbi)
                && mbi.State == MEM_COMMIT)
            continue;                   /* already ours from a shared granule */
        return 0;
    }
    return 1;
}

static int map_image(const char *path) {
    FILE *f = fopen(path, "rb");
    size_t i;
    int mapped = 0;
    if (!f) { printf("cannot open %s@NL@", path); return 0; }
    for (i = 0; i < sizeof g_secs / sizeof g_secs[0]; i++) {
        uintptr_t lo = g_secs[i].va;
        uintptr_t hi = (uintptr_t)g_secs[i].va + g_secs[i].raw_size;
        if (!commit(lo, hi)) {
            printf("  note: %08X..%08X unavailable, section skipped@NL@",
                   (unsigned)lo, (unsigned)hi);
            continue;
        }
        if (fseek(f, (long)g_secs[i].raw_off, SEEK_SET) != 0) continue;
        if (fread((void *)lo, 1, g_secs[i].raw_size, f) != g_secs[i].raw_size)
            continue;
        g_pristine[i] = (unsigned char *)malloc(g_secs[i].raw_size);
        if (g_pristine[i])
            memcpy(g_pristine[i], (void *)lo, g_secs[i].raw_size);
        mapped++;
    }
    fclose(f);
    if (!mapped) { printf("mapped nothing@NL@"); return 0; }
    g_xbox_mem_offset = 0;
    return 1;
}

/* Call the title's own code. pushad/popad around it because a function that
   fails to restore a callee-saved register would otherwise corrupt this
   harness rather than merely fail its comparison. */
static uint32_t g_nat_eax, g_nat_edx;

/* RECOMP_GENERATED_CODE maps the guest register names onto globals -- `eax`
   really is `g_eax` in this translation unit. That rewrite reaches into inline
   assembly too, so the real register names have to be uncovered here and put
   back afterwards for the lifted bodies. */
#undef eax
#undef ecx
#undef edx
#undef esp
#undef ebx
#undef esi
#undef edi

static void *g_fn;
static const uint32_t *g_args_p;
static double g_fp_arg;
static unsigned short g_cw = 0x027F;

/* The title's code runs below a wide gap in this thread's own stack. A
   function that indexes deeply off ebp -- with a frame size derived from an
   argument we invented -- would otherwise write straight through this
   harness's live frames, and that corruption surfaces later, outside any
   guard, as a crash with no connection to the function that caused it.
   Switching to a private stack buffer instead does not work: Windows cannot
   unwind an exception whose esp is outside the thread's stack, and the process
   dies with STATUS_BAD_STACK before the handler is reached. */
/* Kept modest: a large /STACK reservation lands in the low
   address space the XBE itself needs. */
#define NATIVE_STACK_GAP 0x8000
static uint32_t g_saved_sp;

static void call_native(void *fn, const uint32_t *args) {
    /* Both operands go through globals rather than parameters. MSVC addresses
       parameters relative to esp in a frameless function, and the four pushes
       below move esp -- so `call dword ptr fn` would dispatch through the
       wrong slot, which is a fault every time. */
    g_fn = fn;
    g_args_p = args;
    __asm {
        pushad
        mov  esi, g_args_p
        mov  g_saved_sp, esp
        /* Walk the gap a page at a time rather than jumping over it. Windows
           grows a thread stack by touching its guard page; skipping straight
           past leaves the guard unhit, and the first write below it faults in
           a way that cannot be delivered -- the process dies before any
           handler runs, which looks exactly like the lifted code crashing. */
        mov  eax, NATIVE_STACK_GAP
    probe:
        sub  esp, 4
        mov  dword ptr [esp], 0CDCDCDCDh
        sub  eax, 4
        jnz  probe
        push dword ptr [esi+12]
        push dword ptr [esi+8]
        push dword ptr [esi+4]
        push dword ptr [esi]
        /* Enter with the same register state the lifted side starts from.
           Without this a function that never writes eax -- a void one -- comes
           back holding whatever the caller left there, and compares unequal
           against a lifted side that began at zero. The call goes indirectly
           through the stack slot so that eax can be zeroed as well. */
        finit
        /* finit resets the control word to the x87 default (0x037F, extended
           precision). The model holds 0x027F, so without putting it back the
           two sides run at different precision -- and any function that reads
           the control word, as the _control87 family does, reports a different
           answer for reasons that have nothing to do with lifting. */
        fldcw word ptr g_cw
        fld  qword ptr g_fp_arg
        xor  eax, eax
        xor  ecx, ecx
        xor  edx, edx
        xor  ebx, ebx
        xor  esi, esi
        xor  edi, edi
        call dword ptr g_fn
        mov  esp, g_saved_sp
        mov  g_nat_eax, eax
        mov  g_nat_edx, edx
        popad
    }
}

#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
#define ebx g_ebx
#define esi g_esi
#define edi g_edi
'''


# Small integers only. The operand rule already rejects any function that
# dereferences an argument, so these cannot be mistaken for pointers -- and
# the native call runs guarded anyway.
# 0xSC000000 + n means "scratch buffer + n", substituted at run time. A great
# many functions take a pointer, and refusing them all was what kept the
# comparable set tiny; handing them a buffer both sides can see makes them
# testable instead.
SCRATCH = 0x5C000000

ARG_VECTORS = [
    (0, 0, 0, 0),
    (1, 2, 3, 4),
    (0xFFFFFFFF, 1, 0, 2),
    (0x7FFFFFFF, 0x80000000, 0xFF, 0x10),
    (3, 0, 0xFFFF, 0x7F),
    (0x40000000, 0x3F800000, 2, 1),   # also plausible float bit patterns
    (0x80000000, 0xFFFFFFFF, 0x8000, 0),
    (SCRATCH, SCRATCH + 0x100, 4, 1),
    (SCRATCH + 0x40, SCRATCH + 0x200, 0x10, 2),
    (SCRATCH, 1, SCRATCH + 0x80, 0),
    (SCRATCH + 0x20, SCRATCH + 0x20, 8, 0xFFFFFFFF),
]

# A value in st(0) as well. Plenty of this era's maths helpers take their
# argument on the x87 stack rather than the call stack -- and leaving the stack
# empty is not neutral: the hardware yields the indefinite value where the
# model yields 0.0, so the two sides disagree over an input neither was given.
FP_ARGS = [0.0, 1.0, -1.0, 3.5, -2.25, 1e10, 0.5,
           2.0, -0.5, 100.0, 0.25]


def harness_source(xbe_path, sections, lifted, entries):
    # VirtualAlloc places a reservation on a 64 KB boundary, and .text starts
    # at 0x11000 -- round out to the granularity or the request is refused.
    lo = min(s.va for s in sections) & ~0xFFFF
    hi = (max(s.va + s.vsize for s in sections) + 0xFFFF) & ~0xFFFF
    out = [f"#define IMAGE_LO 0x{lo:08X}u", f"#define IMAGE_HI 0x{hi:08X}u", ""]
    out.append("static const struct { unsigned va, raw_off, raw_size; } "
               "g_secs[] = {")
    for s in sections:
        out.append(f"    {{ 0x{s.va:08X}u, 0x{s.raw_off:08X}u, "
                   f"0x{s.raw_size:08X}u }},   /* {s.name} */")
    out.append("};")
    out.append(f'#define XBE_PATH {_c_string(xbe_path)}')
    out.append(_HARNESS)
    # Forward declarations before any body, stubs included: an undeclared call
    # is assumed to return int and then clashes with its own definition.
    defined = {va for va, _, _ in lifted}
    referenced = set()
    for _, _, body in lifted:
        for hit in re.findall(_LIFTED_RE, body):
            referenced.add(int(hit, 16))
    orphans = sorted(referenced - defined)
    for va in sorted(defined) + orphans:
        out.append(f"void lifted_{va:08X}(void);")
    for _, _, body in lifted:
        out.append(body)

    # A body can call something that did not lift cleanly. Those entries were
    # already excluded from the comparable set, so the stub is only here to
    # satisfy the linker -- and it is loud, because reaching one would mean the
    # exclusion failed.
    for va in orphans:
        out.append(f"void lifted_{va:08X}(void) {{ printf(\"reached "
                   f"sub_{va:08X}, which did not lift@NL@\"); g_fail++; }}")

    out.append("static const uint32_t g_funcs[] = {")
    for va, _, _ in entries:
        out.append(f"    0x{va:08X}u,")
    out.append("};")
    out.append("static recomp_func_t lookup_lifted(uint32_t va) {")
    out.append("    switch (va) {")
    for va, _, _ in lifted:
        out.append(f"    case 0x{va:08X}u: return lifted_{va:08X};")
    out.append("    default: return 0; }")
    out.append("}")
    out.append("static const uint32_t g_args[][4] = {")
    for vec in ARG_VECTORS:
        out.append("    { " + ", ".join(f"0x{v & 0xFFFFFFFF:08X}u"
                                        for v in vec) + " },")
    out.append("};")
    out.append("static const double g_fp_args[] = {"
               + ", ".join(repr(float(x)) for x in FP_ARGS) + "};")

    out.append('''
static void run_one(uint32_t va, const uint32_t *raw, int vec, int *shown) {
    uint32_t nat_eax, nat_edx, args[4];
    unsigned char *sp = g_stack + sizeof(g_stack) - 8192;
    int i, wrote;
    recomp_func_t lifted = lookup_lifted(va);

    for (i = 0; i < 4; i++) args[i] = real_arg(raw[i]);

    reset_memory();
    memset(g_stack, 0xCD, sizeof g_stack);
    g_faulted = 0;
    __try { call_native((void *)(uintptr_t)va, args); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_faulted = 1; }
    if (g_faulted) return;          /* not callable with these arguments */
    nat_eax = g_nat_eax; nat_edx = g_nat_edx;
    memcpy(g_scratch_native, g_scratch, SCRATCH_SIZE);

    reset_memory();

    g_fp_cmp = 0; g_fp_cc = 0x4000; g_fp_control_word = 0x027Fu; g_mxcsr = 0x1F80u;
    memset(g_fp_stack, 0, sizeof g_fp_stack);
    /* one value on the x87 stack, matching the native side's fld */
    g_fp_top = 7; g_fp_stack[7] = g_fp_arg;
    g_eax = g_ecx = g_edx = g_ebx = g_esi = g_edi = 0;
    g_seh_ebp = g_ebp = 0;
    for (i = 3; i >= 0; i--) { sp -= 4; memcpy(sp, &args[i], 4); }
    { uint32_t ret = 0xDEADBEEFu; sp -= 4; memcpy(sp, &ret, 4); }
    g_esp = (uint32_t)(uintptr_t)sp;

    __try { lifted(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_faulted = 2; }
    g_total++;
    if (g_faulted == 2) {
        if (!(*shown)++) printf("FAIL sub_%08X  (lifted code faulted)@NL@", va);
        g_fail++;
        return;
    }
    wrote = memcmp(g_scratch_native, g_scratch, SCRATCH_SIZE);
    if (nat_eax == g_eax && nat_edx == g_edx && !wrote) return;
    if (!(*shown)++)
        printf("FAIL sub_%08X@NL@", va);
    if (*shown <= 3) {
        printf("       vec %d  args %08X %08X %08X %08X@NL@"
               "         native eax=%08X edx=%08X   lifted eax=%08X edx=%08X@NL@",
               vec, raw[0], raw[1], raw[2], raw[3],
               nat_eax, nat_edx, g_eax, g_edx);
        if (wrote) {
            for (i = 0; i < SCRATCH_SIZE; i++)
                if (g_scratch_native[i] != g_scratch[i]) {
                    printf("         scratch+%X: native %02X, lifted %02X@NL@",
                           i, g_scratch_native[i], g_scratch[i]);
                    break;
                }
        }
    }
    g_fail++;
}

/* Addresses to leave alone this run. Executing a title's code with arguments
   it never expected cannot be made safe in-process -- a function can corrupt
   whatever it likes before any handler sees it. So the harness announces each
   function before running it and the caller re-invokes with the casualty added
   here, which converges after a few passes and costs no recompile. */
static uint32_t g_skip[256];
static int g_skips;

static int skipped(uint32_t va) {
    int i;
    for (i = 0; i < g_skips; i++) if (g_skip[i] == va) return 1;
    return 0;
}

int main(int argc, char **argv) {
    int i, v, shown;
    const char *path = argc > 1 ? argv[1] : XBE_PATH;
    if (argc > 2) {
        const char *s = argv[2];
        while (*s && g_skips < 256) {
            g_skip[g_skips++] = (uint32_t)strtoul(s, (char **)&s, 16);
            while (*s == ',') s++;
        }
    }
    if (!map_image(path)) return 2;
    /* Deterministic, and not all zero: a function that reads it should see
       something a bug could get wrong. A zero word every 64 bytes, though:
       i * 7 + 1 never has two zero bytes in a row, so a wide-string routine
       (FBoS's wcscpy at 0x000728F0) found no terminator, ran off the end of
       the scratch buffer into the harness's own buffers, and reported a
       mismatch that was the harness, not the lifter. */
    for (i = 0; i < SCRATCH_SIZE; i++)
        g_scratch_seed[i] = (i % 64) >= 62 ? 0 : (unsigned char)(i * 7 + 1);
    { unsigned short cw = 0x027F; __asm { fldcw cw } }
    for (i = 0; i < (int)(sizeof g_funcs / sizeof g_funcs[0]); i++) {
        shown = 0;
        if (skipped(g_funcs[i])) continue;
        printf("@RUN %08X@NL@", g_funcs[i]);
        fflush(stdout);
        for (v = 0; v < (int)(sizeof g_args / sizeof g_args[0]); v++) {
            g_fp_arg = g_fp_args[v];
            run_one(g_funcs[i], g_args[v], v, &shown);
        }
        if (shown > 3) printf("       ... and %d more@NL@", shown - 3);
    }
    printf("@NL@%d function vectors from the title, %d mismatches@NL@",
           g_total, g_fail);
    return g_fail != 0;
}''')
    return "\n".join(out).replace("@NL@", chr(92) + "n")


def _c_string(s):
    return '"' + s.replace(chr(92), chr(92) * 2).replace('"', chr(92) + '"') + '"'
