"""
x86 → C instruction lifter.

Translates individual x86 instructions (and common multi-instruction
patterns like cmp+jcc) into C statements using the recomp_types.h macros.

Register model:
  - eax, ebx, ecx, edx, esi, edi, ebp: uint32_t locals
  - esp: uint32_t local (stack pointer)
  - FPU: shared double g_fp_stack[8] with g_fp_top index

Memory model:
  - MEM8/MEM16/MEM32 macros for memory access at flat addresses
  - Xbox data sections mapped at original VAs
"""

import re
import struct

from .disasm import Instruction, Operand
from .config import is_code_address, is_data_address, va_to_file_offset


# ── Operand formatting ──────────────────────────────────────

def _fmt_reg(name, size=4):
    """Format a register name as a C expression."""
    if not name:
        return "0"

    # Segment registers → constants
    if name in ("fs", "gs", "cs", "ds", "es", "ss"):
        return f"0 /* seg:{name} */"

    # Map sub-registers to expressions on 32-bit locals
    SUB_REGS = {
        "al": "LO8(eax)", "ah": "HI8(eax)", "ax": "LO16(eax)",
        "bl": "LO8(ebx)", "bh": "HI8(ebx)", "bx": "LO16(ebx)",
        "cl": "LO8(ecx)", "ch": "HI8(ecx)", "cx": "LO16(ecx)",
        "dl": "LO8(edx)", "dh": "HI8(edx)", "dx": "LO16(edx)",
        "si": "LO16(esi)", "di": "LO16(edi)",
        "bp": "LO16(ebp)", "sp": "LO16(esp)",
    }
    if name in SUB_REGS:
        return SUB_REGS[name]
    return name


def _fmt_set_reg(name, value_expr):
    """Format assignment to a register, handling sub-register writes."""
    # Segment registers → no-op
    if name in ("fs", "gs", "cs", "ds", "es", "ss"):
        return f"/* mov {name}, {value_expr} - segment register */;"

    SET_MAP = {
        "al": f"SET_LO8(eax, {value_expr})",
        "ah": f"SET_HI8(eax, {value_expr})",
        "ax": f"SET_LO16(eax, {value_expr})",
        "bl": f"SET_LO8(ebx, {value_expr})",
        "bh": f"SET_HI8(ebx, {value_expr})",
        "bx": f"SET_LO16(ebx, {value_expr})",
        "cl": f"SET_LO8(ecx, {value_expr})",
        "ch": f"SET_HI8(ecx, {value_expr})",
        "cx": f"SET_LO16(ecx, {value_expr})",
        "dl": f"SET_LO8(edx, {value_expr})",
        "dh": f"SET_HI8(edx, {value_expr})",
        "dx": f"SET_LO16(edx, {value_expr})",
        "si": f"SET_LO16(esi, {value_expr})",
        "di": f"SET_LO16(edi, {value_expr})",
        "bp": f"SET_LO16(ebp, {value_expr})",
        "sp": f"SET_LO16(esp, {value_expr})",
    }
    if name in SET_MAP:
        return SET_MAP[name] + ";"
    return f"{name} = {value_expr};"


def _fmt_imm(val):
    """Format an immediate value as a C hex literal."""
    if val == 0:
        return "0"
    if val <= 9:
        return str(val)
    if val > 0x7FFFFFFF:
        return f"0x{val:08X}u"
    return f"0x{val:X}"


def _mem_accessor(size):
    """Return the MEM macro name for a given operand size."""
    return {1: "MEM8", 2: "MEM16", 4: "MEM32"}.get(size, "MEM32")


def _smem_accessor(size):
    """Return the signed MEM macro for a given operand size."""
    return {
        1: "SMEM8", 2: "SMEM16", 4: "SMEM32", 8: "SMEM64",
    }.get(size, "SMEM32")


def _fmt_fpu_operand(op):
    """Format an x87 memory or stack-register operand for reading."""
    if op.type == "mem":
        accessor = "MEMD" if op.mem_size == 8 else "MEMF"
        return f"{accessor}({_fmt_mem(op)})"
    if op.type == "reg":
        name = op.reg or ""
        if name == "st":
            return "fp_st(0)"
        if name.startswith("st(") and name.endswith(")"):
            return f"fp_st({name[3:-1]})"
    return None


def _fmt_mem(op):
    """Format a memory operand as a C expression (the address computation).

    An fs-relative operand is offset by XBOX_FS_BASE. The prefix used to be
    dropped, which put the TIB at guest address 0 -- the same address a null
    pointer dereferences. Two things went wrong there and both were silent: a
    null check written as `cmp byte [ecx], 0` read the TIB's first byte (0xFF,
    the end-of-chain marker) and decided the pointer was fine, and a store
    through a null pointer overwrote the exception-chain head instead of
    faulting. With the TIB moved off page zero, that page can be left
    unmapped and both become immediate, locatable faults.
    """
    parts = []
    if getattr(op, "mem_seg", None) == "fs":
        parts.append("XBOX_FS_BASE")
    if op.mem_base:
        parts.append(_fmt_reg(op.mem_base))
    if op.mem_index:
        idx = _fmt_reg(op.mem_index)
        if op.mem_scale and op.mem_scale > 1:
            parts.append(f"{idx} * {op.mem_scale}")
        else:
            parts.append(idx)
    if op.mem_disp:
        if op.mem_disp < 0:
            # Negative displacement - but we stored unsigned, check sign
            if op.mem_disp > 0x80000000:
                # Actually negative (two's complement)
                signed_disp = op.mem_disp - 0x100000000
                if parts:
                    parts.append(f"- {_fmt_imm(-signed_disp)}")
                else:
                    parts.append(_fmt_imm(op.mem_disp))
            else:
                parts.append(_fmt_imm(op.mem_disp))
        else:
            parts.append(_fmt_imm(op.mem_disp))
    if not parts:
        return "0"
    return " + ".join(parts)


def _fmt_mem_read(op):
    """Format reading from a memory operand."""
    accessor = _mem_accessor(op.mem_size)
    addr = _fmt_mem(op)
    return f"{accessor}({addr})"


def _fmt_mem_write(op, value_expr):
    """Format writing to a memory operand."""
    accessor = _mem_accessor(op.mem_size)
    addr = _fmt_mem(op)
    return f"{accessor}({addr}) = {value_expr};"


_REG_WIDTH = {
    "al": 1, "ah": 1, "bl": 1, "bh": 1, "cl": 1, "ch": 1, "dl": 1, "dh": 1,
    "ax": 2, "bx": 2, "cx": 2, "dx": 2, "si": 2, "di": 2, "bp": 2, "sp": 2,
}


def _operand_width(op):
    """Byte width of an operand, or None when it carries no width of its own.

    Memory operands know their size; registers imply it by name; immediates do
    not have one and take it from the other operand. Getting this wrong makes a
    narrow compare test a widened value - "cmp word ptr [x], -1" against
    0xFFFFFFFF never matches, because a 16-bit -1 is 0xFFFF.
    """
    if op.type == "mem":
        return getattr(op, "mem_size", None) or 4
    if op.type == "reg":
        return _REG_WIDTH.get(str(op.reg).lower(), 4)
    return None


def _fmt_operand_read(op):
    """Format reading any operand type."""
    if op.type == "reg":
        return _fmt_reg(op.reg)
    elif op.type == "imm":
        return _fmt_imm(op.imm)
    elif op.type == "mem":
        return _fmt_mem_read(op)
    return "/* unknown operand */"


def _fmt_operand_write(op, value_expr):
    """Format writing to any operand type. Returns a C statement."""
    if op.type == "reg":
        return _fmt_set_reg(op.reg, value_expr)
    elif op.type == "mem":
        return _fmt_mem_write(op, value_expr)
    return f"/* cannot write to {op.type} */;"


# ── Condition code mapping ───────────────────────────────────

# Maps jcc mnemonic → (cmp_macro, test_macro, description)
# cmp_macro takes (lhs, rhs), test_macro takes (lhs, rhs)
COND_MAP = {
    "je":   ("CMP_EQ",  "TEST_Z",  "equal / zero"),
    "jz":   ("CMP_EQ",  "TEST_Z",  "zero"),
    "jne":  ("CMP_NE",  "TEST_NZ", "not equal / not zero"),
    "jnz":  ("CMP_NE",  "TEST_NZ", "not zero"),
    "jb":   ("CMP_B",   None,      "below (unsigned <)"),
    "jnae": ("CMP_B",   None,      "below"),
    "jae":  ("CMP_AE",  None,      "above or equal (unsigned >=)"),
    "jnb":  ("CMP_AE",  None,      "above or equal"),
    "jbe":  ("CMP_BE",  None,      "below or equal (unsigned <=)"),
    "jna":  ("CMP_BE",  None,      "below or equal"),
    "ja":   ("CMP_A",   None,      "above (unsigned >)"),
    "jl":   ("CMP_L",   "TEST_S",  "less (signed <)"),
    "jge":  ("CMP_GE",  None,      "greater or equal (signed >=)"),
    "jle":  ("CMP_LE",  None,      "less or equal (signed <=)"),
    "jg":   ("CMP_G",   None,      "greater (signed >)"),
    "js":   (None,       "TEST_S",  "sign (negative)"),
    "jns":  (None,       None,      "not sign (positive)"),
    "jo":   (None,       None,      "overflow"),
    "jno":  (None,       None,      "not overflow"),
    "jp":   (None,       None,      "parity"),
    "jnp":  (None,       None,      "not parity"),
    "jecxz": (None,      None,      "ecx is zero"),
    "jcxz":  (None,      None,      "cx is zero"),
}

# Instructions that set arithmetic flags (primary set, fully handled)
FLAG_SETTERS = frozenset({
    "cmp", "test", "sub", "add", "and", "or", "xor",
    "inc", "dec", "neg", "shl", "shr", "sar", "imul", "adc", "sbb",
    "comiss", "comisd", "ucomiss", "ucomisd",  # SSE float compare
})

# Arithmetic whose carry-out the lifter computes into _cf next to the write.
#
# A jb/jae reading CF after one of these is exact, which matters because the
# generic fallback is a _flags variable nothing ever assigns -- the condition
# came out always-false. MSVC's bit-oriented decoders are built entirely from
# this shape: "add reg, reg" to shift the top bit into CF, then jae on it.
CF_TRACKED = frozenset({
    "add", "sub", "adc", "sbb", "shl", "shr", "sar",
})

# Additional instructions that modify EFLAGS (tracked but handled as generic)
_EFLAGS_SETTERS = frozenset({
    "shld", "shrd", "rol", "ror", "rcl", "rcr",  # Shifts/rotates set CF
    "bsf", "bsr",       # Bit scan sets ZF
    "bt", "bts", "btr", "btc",  # Bit test sets CF
    "cmpxchg",           # Compare-and-exchange sets ZF
    "xadd",              # Exchange-and-add sets flags
})

# Instructions with undefined/unpredictable flags (clear tracking)
_FLAGS_UNDEFINED = frozenset({
    "mul", "div", "idiv",  # Flags partially undefined
    "rdtsc", "cpuid",      # Special instructions
    "lock xadd",           # Lock prefix - complex flag behavior
})

# Instructions that do NOT modify EFLAGS (preserve flag tracking)
_EFLAGS_PRESERVE = frozenset({
    # General-purpose data movement / stack
    "mov", "lea", "push", "pop", "nop", "leave", "ret",
    "movzx", "movsx", "xchg", "bswap",
    "cdq", "cwde", "cbw", "cwd",
    "lahf",
    "not",  # NOT does not modify flags
    "call",
    "int3", "int", "wait",
    "cld", "std", "cli", "sti",
    "pushfd", "popfd", "pushal",
    "sgdt", "ljmp", "sfence",
    # SSE scalar float
    "movss", "movsd",
    "addss", "subss", "mulss", "divss",
    "minss", "maxss", "sqrtss", "rsqrtss", "rcpss",
    "addsd", "subsd", "mulsd", "divsd",
    "minsd", "maxsd", "sqrtsd",
    "cvtsi2ss", "cvtss2si", "cvttss2si",
    "cvtsi2sd", "cvtsd2si", "cvttsd2si",
    "cvtss2sd", "cvtsd2ss",
    "cmpss", "cmpsd",
    "cmpltss", "cmpeqss", "cmpleps", "cmpneqss",
    # SSE packed float
    "movaps", "movups", "movlps", "movhps", "movlhps", "movhlps",
    "addps", "subps", "mulps", "divps",
    "minps", "maxps", "sqrtps", "rsqrtps", "rcpps",
    "shufps", "unpcklps", "unpckhps",
    "andps", "orps", "xorps", "andnps",
    "cmpps", "cmpneqps",
    "movmskps",
    # SSE2 packed double
    "movapd", "movupd",
    "addpd", "subpd", "mulpd", "divpd",
    # SSE/MMX integer
    "movd", "movq", "movntq",
    "emms",
    "paddb", "paddw", "paddd", "paddq",
    "psubb", "psubw", "psubd",
    "pmullw", "pmulhw", "pmulhuw", "pmaddwd",
    "pand", "pandn", "por", "pxor",
    "pcmpeqb", "pcmpeqw", "pcmpeqd",
    "pcmpgtb", "pcmpgtw", "pcmpgtd",
    "psllw", "pslld", "psllq",
    "psrlw", "psrld", "psrlq",
    "psraw", "psrad",
    "pshufw", "pshufd", "pshufhw", "pshuflw",
    "punpcklbw", "punpcklwd", "punpckldq", "punpcklqdq",
    "punpckhbw", "punpckhwd", "punpckhdq", "punpckhqdq",
    "packsswb", "packssdw", "packuswb",
    "pmovmskb",
    # String operations (without rep prefix)
    "stosb", "stosw", "stosd",
    "movsb", "movsw", "movsd",
    "lodsb", "lodsw", "lodsd",
    # Prefetch hints
    "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2",
})


def _make_condition(jcc, flag_setter, flag_ops):
    """
    Generate a C condition expression for a jcc based on what set the flags.
    Returns (cond_expr, description) or None.
    """
    cond_info = COND_MAP.get(jcc)
    if not cond_info:
        return None
    cmp_macro, test_macro, desc = cond_info

    # A cmp/test that is not fused with its jcc snapshots its operands into
    # _fa/_fb (zero-extended) and _fas/_fbs (sign-extended) at the point the
    # comparison happens. Use those rather than re-reading registers that may
    # since have changed.
    SIGNED = {"CMP_L", "CMP_LE", "CMP_G", "CMP_GE", "TEST_S"}
    if flag_setter in ("cmp", "test", "bsf", "bsr") and len(flag_ops) >= 2:
        signed = (cmp_macro in SIGNED) or (test_macro in SIGNED)
        lhs, rhs = ("_fas", "_fbs") if signed else ("_fa", "_fb")
    elif len(flag_ops) >= 2:
        lhs = _fmt_operand_read(flag_ops[0])
        rhs = _fmt_operand_read(flag_ops[1])
    elif len(flag_ops) == 1:
        lhs = _fmt_operand_read(flag_ops[0])
        rhs = None
    else:
        lhs = None
        rhs = None

    # ── FPU compare-to-EFLAGS and sahf: no standard operands ──
    if flag_setter in ("fcompi", "fcomip", "fucomi", "fucompi",
                        "fucomip", "fcomi", "sahf"):
        fpu_cmp_map = {
            "ja": ">", "jnbe": ">",
            "jae": ">=", "jnb": ">=", "jnc": ">=",
            "jb": "<", "jnae": "<", "jc": "<",
            "jbe": "<=", "jna": "<=",
            "je": "==", "jz": "==",
            "jne": "!=", "jnz": "!=",
        }
        op = fpu_cmp_map.get(jcc)
        if op:
            return f"(g_fp_cmp {op} 0) /* {flag_setter} */", desc
        if jcc == "jp":
            return "0 /* fpu: unordered/NaN */", desc
        if jcc == "jnp":
            return "1 /* fpu: ordered */", desc
        return None

    # If no operands available for other flag-setters, can't generate condition
    if lhs is None:
        return None

    # ── comiss/ucomiss: float comparison, sets CF/ZF/PF ──
    if flag_setter in ("comiss", "comisd", "ucomiss", "ucomisd"):
        def _sse_op(op):
            if op.type == "reg" and op.reg and op.reg.startswith("xmm"):
                return f"{op.reg}.f[0]"
            elif op.type == "reg":
                return op.reg
            elif op.type == "mem":
                if op.mem_size == 8:
                    return f"MEMD({_fmt_mem(op)})"
                return f"MEMF({_fmt_mem(op)})"
            return _fmt_operand_read(op)
        # Read the snapshot the compare left rather than the operands, which
        # may since have been overwritten. _sse_op stays in use for the
        # description only.
        (void_a, void_b) = (
            _sse_op(flag_ops[0]) if len(flag_ops) >= 1 else "0.0f",
            _sse_op(flag_ops[1]) if len(flag_ops) >= 2 else "0.0f",
        )
        desc = f"{desc} ({void_a} vs {void_b})" if desc else desc
        a, b = "_fca", "_fcb"
        # comiss uses unsigned condition codes (CF, ZF)
        if jcc in ("ja", "jnbe"):
            return f"({a} > {b})", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"({a} >= {b})", desc
        if jcc in ("jb", "jnae", "jc"):
            return f"({a} < {b})", desc
        if jcc in ("jbe", "jna"):
            return f"({a} <= {b})", desc
        if jcc in ("je", "jz"):
            return f"({a} == {b})", desc
        if jcc in ("jne", "jnz"):
            return f"({a} != {b})", desc
        if jcc == "jp":
            return f"0 /* {jcc}: unordered/NaN */", desc
        if jcc == "jnp":
            return f"1 /* {jcc}: ordered */", desc
        return None

    # SF is the sign bit of the result at the OPERAND's width, not at 32 bits.
    # `test dl, dl; jns` asks about bit 7; evaluating the zero-extended byte as
    # an int32 makes 0x80..0xFF look positive and the branch always goes the
    # same way. Same defect the signed compares had before the width-aware
    # CMP_L/CMP_G landed -- js/jns were simply missed at the time.
    _sf_width = _operand_width(flag_ops[0]) if flag_ops else None
    if _sf_width is None and len(flag_ops) > 1:
        _sf_width = _operand_width(flag_ops[1])
    _sf_cast = {1: "(int8_t)", 2: "(int16_t)"}.get(_sf_width, "(int32_t)")

    # ── bsf/bsr: ZF is the only flag they define ──
    #
    # ZF is set when the SOURCE was zero, not from any subtraction, and the
    # lifter snapshots that source into _fa with _fb = 0. Everything else --
    # CF, SF, OF, PF -- is architecturally undefined here, so refuse rather
    # than invent a condition for it.
    if flag_setter in ("bsf", "bsr"):
        if jcc in ("je", "jz", "sete", "setz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz", "setne", "setnz"):
            return f"({lhs} != 0)", desc
        return None

    # ── cmp: flags from (a - b), operands unchanged ──
    if flag_setter == "cmp":
        if cmp_macro:
            return f"{cmp_macro}({lhs}, {rhs})", desc
        if jcc == "js":
            return f"({_sf_cast}(({lhs}) - ({rhs})) < 0)", desc
        if jcc == "jns":
            return f"({_sf_cast}(({lhs}) - ({rhs})) >= 0)", desc
        if jcc == "jp":
            return f"RECOMP_PARITY8(({lhs}) - ({rhs}))", desc
        if jcc == "jnp":
            return f"(!RECOMP_PARITY8(({lhs}) - ({rhs})))", desc
        return None

    # ── test: flags from (a & b), operands unchanged ──
    if flag_setter == "test":
        if test_macro:
            return f"{test_macro}({lhs}, {rhs})", desc
        if cmp_macro:
            return f"{cmp_macro}({lhs} & {rhs}, 0)", desc
        if jcc == "js":
            return f"({_sf_cast}(({lhs}) & ({rhs})) < 0)", desc
        if jcc == "jns":
            return f"({_sf_cast}(({lhs}) & ({rhs})) >= 0)", desc
        if jcc == "jo":
            return "0", desc  # OF=0 after test
        if jcc == "jno":
            return "1", desc
        if jcc == "jp":
            # PF from (a & b) low byte. This is the x87 float branch idiom
            # `fnstsw ax; test ah, mask; jp/jnp` (fnstsw put the compare bits
            # into ah). jp jumps on even parity (PF=1).
            return f"RECOMP_PARITY8(({lhs}) & ({rhs}))", desc
        if jcc == "jnp":
            return f"(!RECOMP_PARITY8(({lhs}) & ({rhs})))", desc
        return None

    # ── carry conditions, from the _cf the arithmetic already produced ──
    #
    # Ahead of the per-mnemonic rules below, which reconstruct CF from the
    # operands after the write: that reconstruction is wrong whenever the
    # destination is also the source, because both sides then read the result.
    # "add edx, edx" -- the way MSVC shifts a bit into the carry -- turned into
    # "edx < edx", always false. _cf is computed before the write, so it holds
    # for every operand shape, and the translator declares it exactly when a
    # branch like this one is going to read it.
    if flag_setter in CF_TRACKED:
        if jcc in ("jb", "jnae", "jc"):
            return "_cf", desc
        if jcc in ("jae", "jnb", "jnc"):
            return "!_cf", desc
        # ZF as well: after these the destination holds the result.
        if jcc in ("jbe", "jna"):
            return f"(_cf || {lhs} == 0)", desc
        if jcc in ("ja", "jnbe"):
            return f"(!_cf && {lhs} != 0)", desc

    # ── sub: a = a - b, flags from (a_orig - b) ──
    if flag_setter == "sub":
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        # Ordered: reconstruct original a = result + b
        if cmp_macro and rhs:
            return f"{cmp_macro}((uint32_t){lhs} + (uint32_t){rhs}, (uint32_t){rhs})", desc
        if jcc in ("jb", "jnae"):
            return f"((uint32_t){lhs} + (uint32_t){rhs} < (uint32_t){rhs})", desc
        if jcc in ("jae", "jnb"):
            return f"((uint32_t){lhs} + (uint32_t){rhs} >= (uint32_t){rhs})", desc
        if jcc in ("jl", "jnge"):
            return f"((int32_t){lhs} < 0)", desc
        if jcc in ("jge", "jnl"):
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jle", "jng"):
            return f"((int32_t){lhs} <= 0)", desc
        if jcc in ("jg", "jnle"):
            return f"((int32_t){lhs} > 0)", desc
        return None

    # ── add: a = a + b, flags from result ──
    if flag_setter == "add":
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jb", "jnae", "jc"):
            return f"({lhs} < (uint32_t){rhs})", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"({lhs} >= (uint32_t){rhs})", desc
        if jcc in ("jl", "jnge"):
            return f"((int32_t){lhs} < 0)", desc
        if jcc in ("jge", "jnl"):
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jle", "jng"):
            return f"((int32_t){lhs} <= 0)", desc
        if jcc in ("jg", "jnle"):
            return f"((int32_t){lhs} > 0)", desc
        return None

    # ── adc/sbb: result-based (like add/sub but with carry) ──
    if flag_setter in ("adc", "sbb"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        return None

    # ── and/or/xor: result-based, CF=0, OF=0 ──
    if flag_setter in ("and", "or", "xor"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc in ("js", "jl"):
            return f"((int32_t){lhs} < 0)", desc
        if jcc in ("jns", "jge"):
            return f"((int32_t){lhs} >= 0)", desc
        if jcc == "jle":
            return f"((int32_t){lhs} <= 0)", desc
        if jcc == "jg":
            return f"((int32_t){lhs} > 0)", desc
        if jcc in ("jb", "jnae", "jbe", "jna"):
            return "0", desc  # CF=0 after and/or/xor
        if jcc in ("jae", "jnb", "ja", "jnbe"):
            return "1", desc
        return None

    # ── dec/inc: result-based, CF unchanged ──
    if flag_setter in ("dec", "inc"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jl", "jle", "jg", "jge"):
            cast = "(int32_t)" + lhs
            op = {"jl": "<", "jle": "<=", "jg": ">", "jge": ">="}[jcc]
            return f"({cast} {op} 0)", desc
        return None

    # ── neg: flags from (0 - a_orig), result is -a ──
    if flag_setter == "neg":
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc in ("jb", "jnae", "jc"):
            # CF=1 unless original was 0
            return f"({lhs} != 0)", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"({lhs} == 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jg", "jnle"):
            return f"((int32_t){lhs} > 0)", desc
        if jcc in ("jge", "jnl"):
            return f"((int32_t){lhs} >= 0)", desc
        if jcc in ("jl", "jnge"):
            return f"((int32_t){lhs} < 0)", desc
        if jcc in ("jle", "jng"):
            return f"((int32_t){lhs} <= 0)", desc
        return None

    # ── shift: result-based ──
    if flag_setter in ("shl", "shr", "sar"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        return None

    # ── shld/shrd: double-precision shift, result-based ──
    if flag_setter in ("shld", "shrd"):
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        if jcc == "js":
            return f"((int32_t){lhs} < 0)", desc
        if jcc == "jns":
            return f"((int32_t){lhs} >= 0)", desc
        return None

    # ── rol/ror/rcl/rcr: rotation, only CF/OF affected ──
    if flag_setter in ("rol", "ror", "rcl", "rcr"):
        # ZF/SF not modified by rotations - can't resolve most conditions
        return None

    # ── bsf/bsr: bit scan, ZF set if source is zero ──
    # ── bt/bts/btr/btc: bit test, sets CF ──
    if flag_setter in ("bt", "bts", "btr", "btc"):
        if rhs is None:
            return None
        # Same bit-string rule as the lifter above: a memory bit base with a
        # register offset addresses a string, so the dword is chosen by
        # offset/32 and only then is the bit offset%32. This is the half that
        # does the testing -- MSVC's strpbrk loop is "bt [esp], eax" fused with
        # the jae that follows it.
        if (len(flag_ops) >= 2 and flag_ops[0].type == "mem"
                and flag_ops[1].type != "imm"):
            base = _fmt_mem(flag_ops[0])
            off = _fmt_operand_read(flag_ops[1])
            bit = (f"((MEM32(({base}) + (((int32_t)({off}) >> 5) * 4))"
                   f" >> (({off}) & 31)) & 1)")
            if jcc in ("jb", "jnae", "jc"):
                return bit, desc
            if jcc in ("jae", "jnb", "jnc"):
                return f"!{bit}", desc
            return None
        if jcc in ("jb", "jnae", "jc"):
            return f"(({lhs} >> ({rhs} & 31)) & 1)", desc
        if jcc in ("jae", "jnb", "jnc"):
            return f"!(({lhs} >> ({rhs} & 31)) & 1)", desc
        return None

    # ── cmpxchg: compares accumulator with dest, sets ZF on match ──
    if flag_setter == "cmpxchg":
        # ZF comes from the compare the instruction already did, and the lift
        # snapshots it into _fa/_fb. Re-reading eax here would test a value
        # cmpxchg may have just replaced.
        if jcc in ("je", "jz"):
            return "(_fa == _fb)", desc
        if jcc in ("jne", "jnz"):
            return "(_fa != _fb)", desc
        return None

    # ── xadd: exchange and add, flags from addition ──
    if flag_setter == "xadd":
        if jcc in ("je", "jz"):
            return f"({lhs} == 0)", desc
        if jcc in ("jne", "jnz"):
            return f"({lhs} != 0)", desc
        return None

    # ── repe cmpsb / repne scasb: string comparison ──
    if "cmps" in flag_setter or "scas" in flag_setter:
        if jcc in ("je", "jz"):
            return "(_flags != 0)", desc
        if jcc in ("jne", "jnz"):
            return "(_flags == 0)", desc
        return None

    return None


def _make_setcc_value(setcc_mnemonic, flag_setter, flag_ops):
    """Generate the condition expression for a SETcc instruction."""
    cc = setcc_mnemonic[3:]
    jcc = "j" + cc
    result = _make_condition(jcc, flag_setter, flag_ops)
    if result:
        return result[0]
    return None


def _make_cmovcc_cond(cmov_mnemonic, flag_setter, flag_ops):
    """Generate the condition expression for a CMOVcc instruction."""
    cc = cmov_mnemonic[4:]
    jcc = "j" + cc
    result = _make_condition(jcc, flag_setter, flag_ops)
    if result:
        return result[0]
    return None


# ── Pattern matching for flag-setter + jcc ────────────────────

def _emit_cond_goto(cond_expr, jcc, desc, target, lifter):
    """Emit a conditional goto or call for a jump target."""
    if target is None:
        return f"if ({cond_expr}) {{ /* {jcc}: {desc} - indirect */ }}"
    if lifter and lifter._is_external_target(target):
        # Conditional tail call: same frame bridge as the unconditional tail
        # jmp in _lift_jmp, applied only on the taken path.
        if target in lifter.manual_functions:
            return (f"if ({cond_expr}) {{ g_seh_ebp = ebp; "
                    f"RECOMP_ITAIL(0x{target:08X}u); return; }}"
                    f" /* {jcc}: {desc}, manual tail */")
        name = lifter._call_target_name(target)
        return (f"if ({cond_expr}) {{ g_seh_ebp = ebp; {name}(); return; }}"
                f" /* {jcc}: {desc} */")
    return f"if ({cond_expr}) goto loc_{target:08X}; /* {jcc}: {desc} */"


def try_match_cmp_jcc(insns, idx, lifter=None):
    """
    Try to match a cmp/test + jcc pattern starting at insns[idx].
    Returns (c_statement, num_consumed) or None.
    """
    if idx + 1 >= len(insns):
        return None

    first = insns[idx]
    second = insns[idx + 1]

    if first.mnemonic not in ("cmp", "test") or not second.is_cond_jump:
        return None

    if len(first.operands) < 2:
        return None

    result = _make_condition(second.mnemonic, first.mnemonic, first.operands)
    if not result:
        return None

    cond_expr, desc = result
    target = second.jump_target
    stmt = _emit_cond_goto(cond_expr, second.mnemonic, desc, target, lifter)
    return (stmt, 2)


# ── Single instruction lifting ───────────────────────────────

# MSVC's __SEH_prolog establishes the caller's frame pointer, so the lifter has
# to know which function it is. The address is per-title, and hardcoding it
# meant every other game silently got no frame set up after the call: ebp kept
# whatever stale value it had, and the first ebp-relative local access read
# through it. In Halo that surfaced as a read of 0xFFFFFFFC (ebp=0, [ebp-4]).
#
# Both helpers are compiler boilerplate with distinctive bodies, so detect them
# rather than asking every project to look them up by hand.
#
#   __SEH_prolog   mov eax, fs:[0]        64 A1 00 00 00 00
#                  lea ebp, [esp+0x10]    8D 6C 24 10
#   __SEH_epilog   mov fs:[0], ecx        64 89 0D 00 00 00 00
#                  leave; push ecx; ret   C9 51 C3
_SEH_PROLOG_MARKERS = (b"\x64\xa1\x00\x00\x00\x00", b"\x8d\x6c\x24\x10")
_SEH_EPILOG_MARKERS = (b"\x64\x89\x0d\x00\x00\x00\x00", b"\xc9\x51\xc3")

# Both are tiny; a large match is something else that happens to touch fs:[0].
_SEH_PROLOG_MAX_SIZE = 128
_SEH_EPILOG_MAX_SIZE = 64


def detect_seh_helpers(func_db, xbe_data, verbose=False):
    """Locate __SEH_prolog / __SEH_epilog in the target binary.

    Returns (prolog_addr, epilog_addr); either may be None if not found, which
    is normal for a title whose CRT does not use them.
    """
    from .config import va_to_file_offset

    prolog = epilog = None

    def _size_of(info):
        # "end" is a hex string in functions.json but BatchTranslator rewrites
        # it to an int in place, so accept either.
        try:
            size = int(info.get("size") or 0)
        except (TypeError, ValueError):
            size = 0
        if size:
            return size
        end = info.get("end")
        if isinstance(end, str):
            try:
                end = int(end, 16)
            except ValueError:
                return 0
        return (end - addr) if isinstance(end, int) else 0

    for addr in sorted(func_db):
        info = func_db[addr]
        size = _size_of(info)
        if size <= 0 or size > _SEH_PROLOG_MAX_SIZE:
            continue

        offset = va_to_file_offset(addr)
        if offset is None or xbe_data is None or offset + size > len(xbe_data):
            continue
        body = xbe_data[offset:offset + size]

        if (prolog is None and size <= _SEH_PROLOG_MAX_SIZE
                and all(m in body for m in _SEH_PROLOG_MARKERS)):
            prolog = addr
        elif (epilog is None and size <= _SEH_EPILOG_MAX_SIZE
                and all(m in body for m in _SEH_EPILOG_MARKERS)):
            epilog = addr

        if prolog is not None and epilog is not None:
            break

    if verbose:
        import sys
        fmt = lambda a: f"0x{a:08X}" if a else "not found"
        print(f"  SEH helpers: __SEH_prolog {fmt(prolog)}, "
              f"__SEH_epilog {fmt(epilog)}", file=sys.stderr)

    return prolog, epilog


# MSVC's setjmp/longjmp pair, found by the "VC20" cookie the CRT stamps into
# every jmp_buf. setjmp stores it, longjmp compares against it, so the two are
# told apart by the opcode carrying the constant rather than by the constant
# itself -- which appears in both.
#
#   setjmp    mov dword ptr [edx+20h], 56433230h    C7 42 20 30 32 43 56
#   longjmp   cmp eax, 56433230h                    3D 30 32 43 56
#
# Both markers are unique across a whole title's image, so a hit is the
# function and no size bound is needed.
_SETJMP_MARKER  = bytes.fromhex("c7422030324356")
_LONGJMP_MARKER = bytes.fromhex("3d30324356")


def detect_setjmp_helpers(func_db, xbe_data, verbose=False):
    """Locate the CRT's setjmp and longjmp in the target binary.

    Returns (setjmp_addr, longjmp_addr); either may be None. A title whose CRT
    has neither is normal -- nothing in it uses non-local jumps.
    """
    from .config import va_to_file_offset

    if not xbe_data:
        return None, None

    found = {}
    for marker, key in ((_SETJMP_MARKER, "setjmp"),
                        (_LONGJMP_MARKER, "longjmp")):
        for addr in sorted(func_db):
            info = func_db[addr]
            end = info.get("end")
            if isinstance(end, str):
                try:
                    end = int(end, 16)
                except ValueError:
                    continue
            size = (end - addr) if isinstance(end, int) else 0
            if size <= 0 or size > 512:
                continue
            offset = va_to_file_offset(addr)
            if offset is None or offset + size > len(xbe_data):
                continue
            if marker in xbe_data[offset:offset + size]:
                found[key] = addr
                break

    if verbose:
        import sys
        fmt = lambda a: f"0x{a:08X}" if a else "not found"
        print(f"  CRT non-local jumps: setjmp {fmt(found.get('setjmp'))}, "
              f"longjmp {fmt(found.get('longjmp'))}", file=sys.stderr)

    return found.get("setjmp"), found.get("longjmp")


class Lifter:
    """Translates x86 instructions to C statements."""

    def __init__(self, func_db=None, label_db=None, abi_db=None, xbe_data=None,
                 seh_prolog=None, seh_epilog=None,
                 setjmp_fn=None, longjmp_fn=None, manual_functions=None):
        """
        func_db: dict of func_addr → func_info (for naming call targets)
        label_db: dict of addr → name (for kernel imports, etc.)
        abi_db: dict of addr → ABI info (for calling conventions)
        xbe_data: raw XBE file bytes (for reading jump tables)
        seh_prolog/seh_epilog: override the detected __SEH_prolog/__SEH_epilog
        manual_functions: addresses replaced through recomp_lookup_manual
        """
        self.func_db = func_db or {}
        self.label_db = label_db or {}
        self.abi_db = abi_db or {}
        self.xbe_data = xbe_data
        self.manual_functions = set(manual_functions or ())
        self._fp_top = 0  # FPU stack top index
        self.func_start = 0  # Set per-function by translator
        self.func_end = 0
        self.needs_cf = False  # Set per-function by translator (has adc/sbb)
        self.publishes_ebp = False  # Set per-function: has a real frame
        self.trace_exit_name = None  # Set per-function when traced
        # Every direct call target we emit a name for, as {addr: name}. The
        # batch translator diffs this against the functions it actually defined
        # so it can stub out the remainder (see translate_batch_split).
        self.referenced_calls = {}
        # Mnemonics that fell through to the TODO comment, as
        # {mnemonic: [addr, ...]}. An unimplemented instruction becomes a
        # comment, which is a silent no-op in the generated C: Wreckless spent
        # a whole bring-up dying inside RtlAllocateHeap because `bsf eax, ecx`
        # was a comment, so the heap's free-list bitmap scan returned its own
        # input and the allocator handed out the address of an empty list head.
        # The translator reports this at the end of a run, so the next one
        # costs a line of output instead of an afternoon.
        self.unimplemented = {}

        # Detect if either is missing, so overriding one does not silently
        # leave the other unset -- that is the bug this whole path fixes.
        if (seh_prolog is None or seh_epilog is None) and self.func_db:
            found_prolog, found_epilog = detect_seh_helpers(self.func_db, xbe_data)
            seh_prolog = seh_prolog if seh_prolog is not None else found_prolog
            seh_epilog = seh_epilog if seh_epilog is not None else found_epilog
        self.SEH_PROLOG = seh_prolog
        self.SEH_EPILOG = seh_epilog
        self.SETJMP_FN = setjmp_fn
        self.LONGJMP_FN = longjmp_fn
        self.jump_table_targets = {}

    def _call_target_name(self, addr):
        """Get the name for a call target address.

        func_db wins over label_db. The function definition is emitted from
        func_db, so consulting labels first meant a renamed function was
        *defined* as cseries__sub_0008DB80 but *called* as sub_0008DB80 -- the
        disassembler's generic auto-label -- and the link failed on every
        function any naming pass had touched. Labels still cover call targets
        that are not known function starts.
        """
        if addr in self.func_db:
            name = self.func_db[addr].get("name", f"sub_{addr:08X}")
        elif addr in self.label_db:
            name = self.label_db[addr]
        else:
            name = f"sub_{addr:08X}"
        self.referenced_calls[addr] = name
        return name

    def lift_instruction(self, insn):
        """
        Translate a single x86 instruction to one or more C statements.
        Returns a list of C statement strings.
        """
        m = insn.mnemonic
        ops = insn.operands
        nops = len(ops)

        # ── NOP ──
        if m == "nop" or (m == "lea" and nops == 2 and
                          ops[0].type == "reg" and ops[1].type == "mem" and
                          ops[1].mem_base == ops[0].reg and
                          not ops[1].mem_index and ops[1].mem_disp == 0):
            return [f"/* nop */"]

        # ── Data movement ──
        if m == "mov":
            return self._lift_mov(insn, ops)
        if m == "movzx":
            return self._lift_movzx(insn, ops)
        if m == "movsx":
            return self._lift_movsx(insn, ops)
        if m == "lea":
            return self._lift_lea(insn, ops)
        if m == "xchg":
            return self._lift_xchg(insn, ops)

        # ── Stack ──
        if m == "push":
            return self._lift_push(insn, ops)
        if m == "pop":
            return self._lift_pop(insn, ops)

        # ── Arithmetic ──
        if m in ("add", "sub", "and", "or", "xor"):
            return self._lift_alu_binop(insn, ops, m)
        if m in ("inc", "dec"):
            return self._lift_inc_dec(insn, ops, m)
        if m == "neg":
            return self._lift_neg(insn, ops)
        if m == "not":
            return self._lift_not(insn, ops)
        if m == "imul":
            return self._lift_imul(insn, ops)
        if m in ("mul", "div", "idiv"):
            return self._lift_muldiv(insn, ops, m)
        if m == "sbb":
            return self._lift_sbb(insn, ops)
        if m == "adc":
            return self._lift_adc(insn, ops)
        if m in ("shl", "sal"):
            return self._lift_shift(insn, ops, "<<")
        if m == "shr":
            return self._lift_shift(insn, ops, ">>")
        if m == "sar":
            return self._lift_sar(insn, ops)
        if m in ("rol", "ror"):
            return self._lift_rotate(insn, ops, m)

        # ── Comparison / test (standalone, not part of cmp+jcc pattern) ──
        if m == "cmp":
            return self._lift_cmp(insn, ops)
        if m == "test":
            return self._lift_test(insn, ops)

        # ── Control flow ──
        if m == "call":
            return self._lift_call(insn, ops)
        if m in ("ret", "retn", "retf"):
            return self._lift_ret(insn, ops)
        if m == "jmp":
            return self._lift_jmp(insn, ops)
        if insn.is_cond_jump:
            return self._lift_jcc(insn)

        # ── String operations ──
        if m.startswith("rep ") or m.startswith("repe ") or m.startswith("repne "):
            return self._lift_rep_string(insn, m)
        if m in ("movsb", "movsd", "movsw", "stosb", "stosd", "stosw",
                 "lodsb", "lodsd", "lodsw"):
            return self._lift_string_op(insn, m)
        if m == "wait":
            return ["/* wait - FPU sync */"]

        # ── Misc ──
        if m == "cdq":
            return ["edx = ((int32_t)eax < 0) ? 0xFFFFFFFF : 0; /* cdq */"]
        if m == "cwde":
            return ["eax = SX16(eax); /* cwde */"]
        if m == "cbw":
            return ["SET_LO16(eax, SX8(eax)); /* cbw */"]
        if m == "bswap" and nops >= 1 and ops[0].type == "reg":
            r = _fmt_reg(ops[0].reg)
            return [f"{r} = BSWAP32({r}); /* bswap */"]
        # ── Bit test and modify ──
        # 386 instructions, so real Xbox code has them. The CRT's float-to-int
        # helper uses `btr` to clear a rounding-control bit of the x87 control
        # word, which is exactly the _control87 shape. Unhandled, they lifted to
        # a comment and the bit was silently left alone.
        if m in ("bt", "btr", "bts", "btc") and nops >= 2:
            dst, bit = ops[0], ops[1]

            # A memory bit base with a *register* offset is a bit string, not a
            # dword: the operand addresses the byte holding bit 0, and the
            # offset then runs over the whole string, so the hardware reads the
            # dword at base + (offset/32)*4 and takes bit offset%32. Masking the
            # offset to 31 instead -- which is right only for a register bit
            # base, where the offset really is taken modulo the operand size --
            # folds the entire string onto its first dword.
            #
            # MSVC builds strpbrk, strspn and strcspn out of exactly this: eight
            # zero dwords pushed as a 256-bit character map, "bts [esp], eax"
            # per character of the set, "bt [esp], eax" per character of the
            # string. Folded onto one dword the map aliases mod 32, so '?'
            # (0x3F) sets the same bit '_' (0x5F) tests. Half-Life 2 stats every
            # file for wildcards before opening it, and its archives are
            # zip0_xbox.xzp and zip0_xbox_english.xzp -- every path with an
            # underscore came back "contains a wildcard" and the engine loaded
            # no content at all.
            #
            # An immediate offset is genuinely limited to 0..31 within the
            # addressed dword, so it keeps the simple form.
            if dst.type == "mem" and bit.type != "imm":
                base = _fmt_mem(dst)
                off = _fmt_operand_read(bit)
                word = f"MEM32(({base}) + (((int32_t)({off}) >> 5) * 4))"
                index = f"(({off}) & 31)"
                out = []
                if self.needs_cf:
                    out.append(f"_cf = (int)(({word} >> {index}) & 1u);"
                               f" /* {m}: CF = bit */")
                update = {"btr": f"{word} & ~(1u << {index})",
                          "bts": f"{word} | (1u << {index})",
                          "btc": f"{word} ^ (1u << {index})"}.get(m)
                if update:
                    out.append(f"{word} = ({update}); /* {m} */")
                elif not out:
                    out.append(f"/* bt {insn.op_str}: no CF consumer */")
                return out

            index = (f"({_fmt_imm(bit.imm)})" if bit.type == "imm"
                     else f"({_fmt_operand_read(bit)} & 31)")
            value = _fmt_operand_read(dst)
            out = []
            if self.needs_cf:
                out.append(f"_cf = (int)(({value} >> {index}) & 1u);"
                           f" /* {m}: CF = bit */")
            update = {"btr": f"{value} & ~(1u << {index})",
                      "bts": f"{value} | (1u << {index})",
                      "btc": f"{value} ^ (1u << {index})"}.get(m)
            if update:
                out.append(_fmt_operand_write(dst, f"({update})")
                           + f" /* {m} */")
            elif not out:
                out.append(f"/* bt {insn.op_str}: no CF consumer */")
            return out

        # INT 2D is the Xbox kernel debug trap: eax picks the service, ecx
        # carries its argument, and service 1 prints the ANSI_STRING ecx points
        # at. It is always followed by an int3 that the kernel skips over.
        #
        # Emitting nothing for that int3 is not laziness about breakpoints: a
        # __debugbreak() there terminates the process with STATUS_BREAKPOINT,
        # which is exit code 3 and no message at all. Wreckless hit it the
        # first time it tried to print a debug line, after a full boot.
        if m == "int" and ops and ops[0].type == "imm" and ops[0].imm == 0x2D:
            return ["recomp_debug_service(eax, ecx); /* int 0x2d */"]
        if m == "int3":
            return ["/* int3: debug-trap slide byte, stepped over */"]
        if m in ("leave",):
            return ["esp = ebp;", "POP32(esp, ebp); /* leave */"]
        if m in ("cld", "std"):
            return [f"g_df = {0 if m == 'cld' else 1}; /* {m} */"]
        if m == "lahf":
            return ["/* lahf - load AH from flags (used in FPU compare idiom) */"]
        if m == "sahf":
            return ["/* sahf - store AH to flags */"]
        if m == "shld":
            return self._lift_shld(insn, ops)
        if m == "shrd":
            return self._lift_shrd(insn, ops)
        if m == "bt":
            if len(ops) >= 2:
                return [f"/* bt {_fmt_operand_read(ops[0])}, {_fmt_operand_read(ops[1])} - bit test */"]
            return [f"/* bt {insn.op_str} */"]
        if m == "emms":
            # A statement rather than a comment: emms genuinely has no
            # effect here (mm/x87 aliasing is not modelled), and a
            # comment-only lift is how the conformance suite reports a
            # silently dropped instruction.
            return ["(void)0; /* emms - empty MMX state */"]
        if m in ("xlat", "xlatb"):
            # AL indexes the byte table at EBX. With an address-size override,
            # the effective offset is calculated and wrapped at 16 bits.
            raw_bytes = bytes.fromhex(insn.bytes_hex)
            if 0x67 in raw_bytes[:-1]:
                address = "(uint16_t)(LO16(ebx) + LO8(eax))"
            else:
                address = "ebx + LO8(eax)"
            return [f"SET_LO8(eax, MEM8({address})); /* xlatb */"]
        if m in ("sete", "setne", "setb", "setae", "setbe", "seta",
                 "setl", "setge", "setle", "setg", "sets", "setns"):
            return self._lift_setcc(insn, ops, m)
        if m in ("cmove", "cmovne", "cmovb", "cmovae", "cmovbe", "cmova",
                 "cmovl", "cmovge", "cmovle", "cmovg", "cmovs", "cmovns"):
            return self._lift_cmovcc(insn, ops, m)

        # ── SSE (scalar float) ──
        # Non-temporal SSE stores differ from the ordinary ones only in
        # cache behaviour, which nothing here models -- but they are
        # stores, and dropping them loses the data. Wreckless writes 50
        # of them in its texture upload path.
        if m in ("movntps", "movntpd", "movntdq"):
            m = "movaps"

        # ── MMX ──
        # Dispatched on the operands rather than the mnemonic, because the
        # integer SIMD names are shared with SSE: `paddw mm0, mm1` and
        # `paddw xmm0, xmm1` differ only in register file.
        if any(op.type == "reg" and op.reg and op.reg.startswith("mm")
               and not op.reg.startswith("xmm") for op in ops) or m in (
                   "emms", "femms"):
            return self._lift_mmx(insn, m, ops)

        if m in ("movss", "movsd", "movaps", "movups", "movlps", "movhps",
                 "movlhps", "movhlps", "movapd", "movupd",
                 "addss", "subss", "mulss", "divss", "sqrtss",
                 "addsd", "subsd", "mulsd", "divsd", "sqrtsd",
                 "minss", "maxss", "minsd", "maxsd",
                 "comiss", "comisd", "ucomiss", "ucomisd",
                 "cvtsi2ss", "cvtss2si", "cvttss2si",
                 "cvtsi2sd", "cvtsd2si", "cvttsd2si",
                 "cvtss2sd", "cvtsd2ss",
                 "xorps", "xorpd", "andps", "orps", "andnps",
                 "movd", "movq",
                 "shufps", "unpcklps", "unpckhps",
                 "addps", "subps", "mulps", "divps",
                 "minps", "maxps", "rsqrtss", "rcpss",
                 "sqrtps", "rsqrtps", "rcpps",
                 "cmpneqps", "cmpeqps", "cmpltps", "cmpleps",
                 "movmskps",
                 "pand", "pandn", "por", "pxor", "pcmpgtd"):
            return self._lift_sse(insn, m, ops)

        # ── FPU ──
        if m.startswith("f"):
            return self._lift_fpu(insn, m, ops)

        # ── Explicit carry-flag manipulation ──
        # MSVC emits these around the multi-word arithmetic helpers, and around
        # the "return a bool in CF" idiom. They were unhandled, so the flag the
        # following adc/sbb reads kept whatever the last arithmetic left in it.
        # Only worth emitting when something downstream consumes CF -- that is
        # also the only time the enclosing function declares _cf.
        # Cache hints and store fences: nothing to model on a single-threaded
        # interpreter over coherent host memory. Named so they stop showing up
        # in the unimplemented report as if they were missing work.
        if m.startswith("prefetch") or m in ("sfence", "lfence", "mfence"):
            return [f"(void)0; /* {m}: cache/ordering hint, nothing to model */"]

        if m in ("stc", "clc", "cmc"):
            if not self.needs_cf:
                return [f"/* {m}: no adc/sbb in this function consumes CF */"]
            expr = {"stc": "1", "clc": "0", "cmc": "!_cf"}[m]
            return [f"_cf = {expr}; /* {m} */"]

        # ── Time stamp counter ──
        #
        # The guest reads wall-clock time through it. Xbox's
        # QueryPerformanceCounter is literally `rdtsc`, and its
        # QueryPerformanceFrequency returns the CPU clock as a constant --
        # Half-Life 2's is 0x2BB5C755 (733,333,333 Hz) at 0x0059C6C7. So a
        # frame timer computes seconds as counter / 733333333, and an rdtsc
        # that does nothing leaves the counter fixed: every "now - last" is
        # zero, and a loop waiting for time to pass never finishes. HL2 spins
        # 300 million times in sub_0040F4E0 doing exactly that.
        #
        # The runtime scales the host's counter to the console's clock rate
        # rather than returning the host TSC, so the guest's own division by
        # its hardcoded frequency yields real seconds.
        if m == "rdtsc":
            return [
                "{ uint64_t _tsc = xbox_ReadTimeStampCounter();",
                "  eax = (uint32_t)_tsc; edx = (uint32_t)(_tsc >> 32); }"
                "  /* rdtsc */",
            ]

        # ── Bit scan ──
        # Index of the lowest (bsf) or highest (bsr) set bit. When the source
        # is zero the destination is left untouched and ZF is set; that is the
        # whole contract, and callers branch on ZF to tell the cases apart.
        #
        # The CRT heap leans on this: RtlAllocateHeap picks a size bucket by
        # bsf-ing its free-list-in-use bitmap, so a bsf that does nothing hands
        # back the address of an empty list head and the heap eats itself.
        #
        # ZF is published through the same _fa/_fb snapshot a `cmp src, 0`
        # would leave, which is what _make_condition reads for a following jcc.
        # The other flags are architecturally undefined here, so a jcc that
        # reads them was already meaningless.
        if m in ("bsf", "bsr"):
            if len(ops) < 2:
                self.unimplemented.setdefault(m, []).append(insn.address)
                return [f"/* TODO: {m} {insn.op_str} */"]
            dst = _fmt_operand_read(ops[0])
            src = _fmt_operand_read(ops[1])
            width = (_operand_width(ops[1]) or 4) * 8
            if m == "bsr":
                init, cond, step = f"{width - 1}", "_bs_i >= 0", "_bs_i--"
            else:
                init, cond, step = "0", f"_bs_i < {width}", "_bs_i++"
            return [
                f"{{ uint32_t _bs_v = (uint32_t)({src}); int _bs_i;",
                f"  _fa = _bs_v; _fb = 0; _fas = (int32_t)_fa; _fbs = 0;"
                f" /* {m}: ZF = src == 0 */",
                "  if (_bs_v) {",
                f"    for (_bs_i = {init}; {cond}; {step})",
                "      if (_bs_v & (1u << _bs_i)) break;",
                "    " + _fmt_operand_write(ops[0], "(uint32_t)_bs_i") + ";",
                f"  }} else {{ (void)({dst}); }} }}",
            ]

        # ── Atomic read-modify-write ──
        #
        # "lock xadd" and "lock cmpxchg" are what InterlockedIncrement and
        # InterlockedCompareExchange compile to, so they carry a title's
        # reference counts and its lock-free lists. Both used to fall through
        # to the TODO below: a refcount that never moved and a compare-and-swap
        # that never swapped. Harmless while every guest thread ran
        # synchronously, and not once the runtime began spawning real ones.
        #
        # Emitted through RECOMP_ATOMIC_*, which are genuinely atomic. A
        # read-modify-write that races is the exact bug these instructions are
        # there to prevent, so lowering them to a plain sequence would swap one
        # silent race for another.
        # Capstone keeps the prefix in the mnemonic, so the locked and
        # unlocked spellings both arrive here. Both lift atomically: an
        # unlocked xadd on memory is single-CPU-safe on the console and
        # costs nothing extra to make safe here.
        atomic_m = m[5:] if m.startswith("lock ") else m
        if atomic_m in ("xadd", "cmpxchg") and len(ops) >= 2:
            width = _operand_width(ops[0]) or 4
            if width == 4 and ops[0].type == "mem":
                addr = _fmt_mem(ops[0])
                src = _fmt_operand_read(ops[1])
                if atomic_m == "xadd":
                    # dst = dst + src, and src receives dst's old value.
                    return [
                        "{ uint32_t _old = RECOMP_ATOMIC_ADD32("
                        f"XBOX_PTR({addr}), {src});",
                        "  " + _fmt_operand_write(ops[1], "_old") + " }"
                        f"  /* {m} */",
                    ]
                # cmpxchg: compare eax with dst; on a match store src, else
                # load dst into eax. ZF says which happened, and it is
                # snapshotted rather than recomputed -- eax may have just been
                # overwritten, so re-reading it to decide the branch would test
                # the wrong pair.
                return [
                    "{ uint32_t _cmp = eax;",
                    "  uint32_t _old = RECOMP_ATOMIC_CAS32("
                    f"XBOX_PTR({addr}), _cmp, {src});",
                    "  _fa = _old; _fb = _cmp;",
                    "  _fas = (int32_t)_fa; _fbs = (int32_t)_fb;",
                    "  if (_old != _cmp) eax = _old; }"
                    f"  /* {m} */",
                ]

        # ── Unhandled ──
        #
        # Recorded, not merely commented -- see self.unimplemented.
        self.unimplemented.setdefault(m, []).append(insn.address)
        return [f"/* TODO: {m} {insn.op_str} */"]

    # ── MOV family ──

    def _lift_mov(self, insn, ops):
        if nops := len(ops) < 2:
            return [f"/* mov: bad operands */"]
        src = _fmt_operand_read(ops[1])
        out = [_fmt_operand_write(ops[0], src)]
        # `mov ebp, esp` establishes this function's frame. Publish it, because
        # ebp is a per-function local and a callee with no prologue of its own
        # reads its caller's frame through ebp -- MSVC emits those routinely for
        # shared tails (Halo's CRT float formatting is one). Without the
        # publish, such a callee starts from an uninitialised ebp and its
        # [ebp-N] stores land wherever that garbage points.
        if (ops[0].type == "reg" and ops[0].reg == "ebp" and
                ops[1].type == "reg" and ops[1].reg == "esp"):
            out.append("g_ebp = ebp; /* publish frame for frameless callees */")
            # g_seh_ebp too. A callee with no prologue inherits through
            # g_seh_ebp, not g_ebp, but only tail jumps and the SEH helpers
            # ever wrote it -- so one reached by an ordinary call read whatever
            # frame the last tail jump had left behind. Wreckless hit this in
            # setjmp: it saved that stale ebp into the jmp_buf, and the longjmp
            # that should have resumed the catch restored a frame two calls
            # dead, so the exception unwound past every handler and off the top
            # of the stack.
            out.append("g_seh_ebp = ebp;")
        return out

    def _lift_movzx(self, insn, ops):
        if len(ops) < 2:
            return [f"/* movzx: bad operands */"]
        src = _fmt_operand_read(ops[1])
        if ops[1].type == "mem":
            if ops[1].mem_size == 1:
                src = f"ZX8({src})"
            elif ops[1].mem_size == 2:
                src = f"ZX16({src})"
        elif ops[1].type == "reg":
            r = ops[1].reg
            if r in ("al", "bl", "cl", "dl", "ah", "bh", "ch", "dh"):
                src = f"ZX8({src})"
            elif r in ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp"):
                src = f"ZX16({src})"
        return [_fmt_operand_write(ops[0], src)]

    def _lift_movsx(self, insn, ops):
        if len(ops) < 2:
            return [f"/* movsx: bad operands */"]
        src = _fmt_operand_read(ops[1])
        if ops[1].type == "mem":
            accessor = _smem_accessor(ops[1].mem_size)
            addr = _fmt_mem(ops[1])
            src = f"(uint32_t)(int32_t){accessor}({addr})"
        elif ops[1].type == "reg":
            r = ops[1].reg
            if r in ("al", "bl", "cl", "dl", "ah", "bh", "ch", "dh"):
                src = f"SX8({src})"
            elif r in ("ax", "bx", "cx", "dx", "si", "di"):
                src = f"SX16({src})"
        return [_fmt_operand_write(ops[0], src)]

    def _lift_lea(self, insn, ops):
        if len(ops) < 2 or ops[1].type != "mem":
            return [f"/* lea: unexpected operands */"]
        addr_expr = _fmt_mem(ops[1])
        return [_fmt_operand_write(ops[0], addr_expr)]

    def _lift_xchg(self, insn, ops):
        if len(ops) < 2:
            return [f"/* xchg: bad operands */"]
        a = _fmt_operand_read(ops[0])
        b = _fmt_operand_read(ops[1])
        return [
            f"{{ uint32_t _tmp = {a};",
            _fmt_operand_write(ops[0], b),
            _fmt_operand_write(ops[1], "_tmp") + " }",
        ]

    # ── Stack ──

    def _lift_push(self, insn, ops):
        if len(ops) < 1:
            return ["/* push: no operand */"]
        val = _fmt_operand_read(ops[0])
        return [f"PUSH32(esp, {val});"]

    def _lift_pop(self, insn, ops):
        if len(ops) < 1:
            return ["/* pop: no operand */"]
        if ops[0].type == "reg":
            r = ops[0].reg
            # Segment register pop → discard from stack
            if r in ("fs", "gs", "cs", "ds", "es", "ss"):
                return [f"{{ uint32_t _tmp; POP32(esp, _tmp); }} /* pop {r} - segment register */"]
            # Sample esp at each pop in a traced function. An epilogue that ends
            # `mov esp, ebp` restores esp unconditionally, so any drift inside
            # the function is erased before a return-time trace can see it --
            # yet the pops run *before* that restore, so drift is exactly what
            # breaks them. This is the only point the drift is observable.
            if self.trace_exit_name:
                return [f'RECOMP_TRACE_ESP("{self.trace_exit_name}", "pop {r}");',
                        f"POP32(esp, {r});"]
            return [f"POP32(esp, {r});"]
        else:
            return [f"{{ uint32_t _tmp; POP32(esp, _tmp); {_fmt_operand_write(ops[0], '_tmp')} }}"]

    # ── ALU binary operations ──

    def _lift_alu_binop(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        c_op = {"add": "+", "sub": "-", "and": "&", "or": "|", "xor": "^"}[m]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        # XOR reg, reg → zero
        if m == "xor" and ops[0].type == "reg" and ops[1].type == "reg" and ops[0].reg == ops[1].reg:
            return [_fmt_operand_write(ops[0], "0") + " /* xor self */"]
        expr = f"{dst} {c_op} {src}"
        out = []
        if self.needs_cf:
            # CF must be computed from the pre-write operands.
            if m == "add":
                w = _operand_width(ops[0]) or 4
                out.append(f"_cf = (int)((((uint64_t)({dst}) + (uint64_t)({src})) >> {w * 8}) & 1);")
            elif m == "sub":
                out.append(f"_cf = (int)((uint32_t)({dst}) < (uint32_t)({src}));")
            else:
                out.append("_cf = 0; /* logical op clears CF */")
        out.append(_fmt_operand_write(ops[0], expr))
        return out

    def _lift_inc_dec(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        val = _fmt_operand_read(ops[0])
        delta = "1"
        op_char = "+" if m == "inc" else "-"
        # For sub-registers (al, cl, etc.), use the SET macro instead of ++
        if ops[0].type == "reg" and ops[0].reg in (
                "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"):
            return [f"{val}{'++' if m == 'inc' else '--'};"]
        else:
            return [_fmt_operand_write(ops[0], f"{val} {op_char} {delta}")]

    def _lift_neg(self, insn, ops, preserve_carry=False):
        if len(ops) < 1:
            return ["/* neg: no operand */"]
        val = _fmt_operand_read(ops[0])
        out = []
        if preserve_carry or self.needs_cf:
            # neg sets CF iff the operand was non-zero (neg/sbb sign-extract).
            out.append(f"_cf = (int)(({val}) != 0);")
        out.append(_fmt_operand_write(ops[0], f"(uint32_t)(-(int32_t){val})"))
        return out

    def _lift_not(self, insn, ops):
        if len(ops) < 1:
            return ["/* not: no operand */"]
        val = _fmt_operand_read(ops[0])
        return [_fmt_operand_write(ops[0], f"~{val}")]

    def _lift_sbb(self, insn, ops):
        """SBB: subtract with borrow. Common idiom: sbb reg, reg → -CF (0 or -1)."""
        if len(ops) < 2:
            return ["/* sbb: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        # sbb reg, reg is a common idiom: result is 0 or 0xFFFFFFFF depending on CF
        if ops[0].type == "reg" and ops[1].type == "reg" and ops[0].reg == ops[1].reg:
            return [_fmt_operand_write(ops[0], "_cf ? 0xFFFFFFFF : 0") + " /* sbb self (CF extend) */"]
        w = (_operand_width(ops[0]) or 4) * 8
        return ["{ uint64_t _t = (uint64_t)(%s) - (uint64_t)(%s) - (uint64_t)_cf;"
                " _cf = (int)((_t >> %d) & 1); %s }  /* sbb */"
                % (dst, src, w, _fmt_operand_write(ops[0], "(uint32_t)_t"))]

    def _lift_adc(self, insn, ops):
        """ADC: add with carry."""
        if len(ops) < 2:
            return ["/* adc: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        w = (_operand_width(ops[0]) or 4) * 8
        return ["{ uint64_t _t = (uint64_t)(%s) + (uint64_t)(%s) + (uint64_t)_cf;"
                " _cf = (int)((_t >> %d) & 1); %s }  /* adc */"
                % (dst, src, w, _fmt_operand_write(ops[0], "(uint32_t)_t"))]

    def _lift_shld(self, insn, ops):
        """SHLD: double-precision shift left."""
        if len(ops) < 3:
            return [f"/* shld: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        cnt = _fmt_operand_read(ops[2])
        return [_fmt_operand_write(ops[0],
            f"({dst} << {cnt}) | ({src} >> (32 - {cnt}))") + " /* shld */"]

    def _lift_shrd(self, insn, ops):
        """SHRD: double-precision shift right."""
        if len(ops) < 3:
            return [f"/* shrd: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        src = _fmt_operand_read(ops[1])
        cnt = _fmt_operand_read(ops[2])
        return [_fmt_operand_write(ops[0],
            f"({dst} >> {cnt}) | ({src} << (32 - {cnt}))") + " /* shrd */"]

    def _lift_imul(self, insn, ops):
        nops = len(ops)
        if nops == 1:
            # One operand: edx:eax = eax * ops[0]
            src = _fmt_operand_read(ops[0])
            return [
                f"{{ int64_t _r = (int64_t)(int32_t)eax * (int64_t)(int32_t){src};",
                f"  eax = (uint32_t)_r; edx = (uint32_t)(_r >> 32); }}"
            ]
        elif nops == 2:
            # Two operand: dst = dst * src
            dst = _fmt_operand_read(ops[0])
            src = _fmt_operand_read(ops[1])
            return [_fmt_operand_write(ops[0], f"(uint32_t)((int32_t){dst} * (int32_t){src})")]
        elif nops == 3:
            # Three operand: dst = src1 * imm
            src = _fmt_operand_read(ops[1])
            imm = _fmt_operand_read(ops[2])
            return [_fmt_operand_write(ops[0], f"(uint32_t)((int32_t){src} * (int32_t){imm})")]
        return ["/* imul: unexpected form */"]

    def _lift_muldiv(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        src = _fmt_operand_read(ops[0])
        if m == "mul":
            return [
                f"{{ uint64_t _r = (uint64_t)eax * (uint64_t){src};",
                f"  eax = (uint32_t)_r; edx = (uint32_t)(_r >> 32); }}"
            ]
        elif m == "div":
            return [
                f"{{ uint64_t _dividend = ((uint64_t)edx << 32) | eax;",
                f"  eax = (uint32_t)(_dividend / (uint32_t){src});",
                f"  edx = (uint32_t)(_dividend % (uint32_t){src}); }}"
            ]
        elif m == "idiv":
            return [
                f"{{ int64_t _dividend = ((int64_t)(int32_t)edx << 32) | eax;",
                f"  eax = (uint32_t)((int32_t)(_dividend / (int32_t){src}));",
                f"  edx = (uint32_t)((int32_t)(_dividend % (int32_t){src})); }}"
            ]
        return [f"/* {m}: unhandled */"]

    def _lift_shift(self, insn, ops, c_op):
        if len(ops) < 2:
            return [f"/* shift: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        out = []
        if self.needs_cf:
            w = (_operand_width(ops[0]) or 4) * 8
            # CF is the last bit shifted out; a zero count leaves CF alone.
            bit = f"({cnt}) - 1" if c_op == ">>" else f"{w} - ({cnt})"
            out.append(f"if ({cnt}) _cf = (int)((({dst}) >> ({bit})) & 1);")
        out.append(_fmt_operand_write(ops[0], f"{dst} {c_op} {cnt}"))
        return out

    def _lift_sar(self, insn, ops):
        if len(ops) < 2:
            return ["/* sar: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        out = []
        if self.needs_cf:
            out.append(f"if ({cnt}) _cf = (int)((({dst}) >> (({cnt}) - 1)) & 1);")
        out.append(_fmt_operand_write(ops[0], f"(uint32_t)((int32_t){dst} >> {cnt})"))
        return out

    def _lift_rotate(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        dst = _fmt_operand_read(ops[0])
        cnt = _fmt_operand_read(ops[1])
        func = "ROL32" if m == "rol" else "ROR32"
        return [_fmt_operand_write(ops[0], f"{func}({dst}, {cnt})")]

    # ── Compare / Test (standalone) ──

    # Widths for the flag snapshot below.
    _SNAP_MASK = {1: "0xFFu", 2: "0xFFFFu", 4: "0xFFFFFFFFu"}
    _SNAP_SX = {1: "(int8_t)", 2: "(int16_t)", 4: "(int32_t)"}

    def _snapshot_flags(self, insn, ops, kind):
        """Capture a cmp/test's operands where the comparison happens.

        The flags are consumed later by a jcc, which used to re-read the
        operands at that point. Two things go wrong with that. Anything
        between the two can change them - "cmp [esi+ecx*2], -1 / lea esi,
        [esi+ecx*2] / jne" re-read through the already-advanced esi and
        tested the wrong slot. And the comparison lost its width, so a
        16-bit "cmp word ptr [..], -1" became a compare against
        0xFFFFFFFF, which a 16-bit -1 (0xFFFF) never equals - the branch
        then went the same way every time.

        Snapshotting fixes both: operands are read once, at the right
        width, in both a zero- and a sign-extended form so the jcc can
        pick whichever its condition needs.
        """
        size = _operand_width(ops[0])
        if size is None:
            size = _operand_width(ops[1])          # e.g. cmp imm, reg
        if size not in self._SNAP_MASK:
            size = 4
        mask = self._SNAP_MASK[size]
        sx = self._SNAP_SX[size]
        lhs = _fmt_operand_read(ops[0])
        rhs = _fmt_operand_read(ops[1])
        out = [
            f"_fa = (uint32_t)({lhs}) & {mask}; _fb = (uint32_t)({rhs}) & {mask};",
            f"_fas = (int32_t){sx}(_fa); _fbs = (int32_t){sx}(_fb);"
            f" /* {kind} {lhs}, {rhs} ({size*8}-bit) */",
        ]
        # A cmp sets the carry flag too, and sbb/adc/setc/rcl read it directly
        # rather than through _fa/_fb. Leaving CF alone here let those pick up
        # whatever an earlier instruction had left in it.
        #
        # MSVC's branchless tolower, in the _stricmp every Source string
        # compare goes through, is exactly that shape:
        #
        #     sub al, 0x41      ; CF = al < 'A'
        #     cmp al, 0x1A      ; CF = "is a letter"  <- the one sbb wants
        #     sbb cl, cl
        #     and cl, 0x20
        #     add al, cl        ; fold to lowercase
        #
        # With CF still coming from the sub, no uppercase letter was ever
        # folded and _stricmp quietly became strcmp.
        #
        # _fa and _fb are already masked to the operand width, so their
        # unsigned comparison is CF at that width.
        if self.needs_cf:
            if kind == "cmp":
                out.append("_cf = (int)(_fa < _fb);")
            else:
                out.append("_cf = 0; /* test/cmp-logical clears CF */")
        return out

    def _lift_cmp(self, insn, ops):
        if len(ops) < 2:
            return ["/* cmp: bad operands */"]
        return self._snapshot_flags(insn, ops, "cmp")

    def _lift_test(self, insn, ops):
        if len(ops) < 2:
            return ["/* test: bad operands */"]
        return self._snapshot_flags(insn, ops, "test")

    # ── Control flow ──

    def _build_call_args(self, target_addr):
        """Build argument list for a function call based on ABI data."""
        abi_info = self.abi_db.get(target_addr, {})
        cc = abi_info.get("calling_convention", "cdecl")
        num_params = abi_info.get("estimated_params", 0)

        args = []
        if cc in ("thiscall", "thiscall_cdecl"):
            args.append("(void*)(uintptr_t)ecx")
        for i in range(num_params):
            args.append(f"0 /* a{i+1} */")
        return ", ".join(args)

    # SEH prolog/epilog addresses - these functions modify ebp for their
    # caller.  After calling __SEH_prolog, the caller must read back ebp
    # from g_seh_ebp.  Before returning, __SEH_prolog writes g_seh_ebp.
    #
    # Per-title addresses, detected from the binary by detect_seh_helpers()
    # and assigned to the instance. The class values are only a fallback for
    # callers that construct a Lifter without a function database.
    SEH_PROLOG = None
    SEH_EPILOG = None

    # The CRT's setjmp/longjmp, detected by detect_setjmp_helpers().
    SETJMP_FN = None
    LONGJMP_FN = None

    def _lift_call(self, insn, ops):
        # x86 'call' pushes the address of the following instruction, then jumps.
        # Push that real guest address, not a placeholder: the value is visible
        # to the callee, and plenty of x86 code reads it. __SEH_prolog locates
        # its scope table through [esp], _alloca probes walk back to it, and the
        # `mov eax, [esp]` / `pop eax` idiom for "where was I called from" shows
        # up in any CRT. A zero there is silently wrong until something reads it.
        #
        # 'ret' still only does esp += 4 and returns -- it never consumes the
        # value -- so writing the true address costs nothing at the return side.
        ret_va = insn.end_address
        if insn.call_target:
            name = self._call_target_name(insn.call_target)
            lines = []
            # Re-publish this function's frame before every call, not just once
            # at `mov ebp, esp`. g_ebp is "the last frame established anywhere",
            # so a callee that sets up its own frame overwrites it and leaves it
            # stale on return. A frameless helper called afterwards then
            # inherits the wrong frame -- in Halo, sub_001E1BA0 called one
            # function, returned, then called the frameless sub_001DEC07, which
            # inherited a long-dead frame of ~0xA6 and wrote [ebp-0xa2] and
            # [ebp-0xa0] onto Xbox VA 4 and 6: exactly the fs:[4] corruption.
            if self.publishes_ebp:
                lines.append("g_ebp = ebp; /* frame stays current across calls */")
                lines.append("g_seh_ebp = ebp;")
            # Non-local jumps have to move the native stack, not just the
            # guest one. Every recompiled function is a real C function, so a
            # guest longjmp that only rewrites esp leaves the abandoned frames
            # sitting on the native stack: the resume point runs, returns, and
            # C unwinds straight back into frames the guest has already left,
            # which then keep executing against a stack pointer that moved. In
            # Wreckless that turned a correctly caught image-loader exception
            # into the decoder being re-entered with a garbage context, and
            # then an endless drain loop.
            #
            # So each guest jmp_buf gets a native one taken here, in the frame
            # that calls setjmp -- the only place a native setjmp is valid --
            # and the guest longjmp becomes a native longjmp back to it. The
            # frames unwind for real and execution resumes exactly where the
            # guest buffer says.
            #
            # Both keep the guest call as a fallback: a buffer armed by a
            # setjmp that was reached some other way has no native counterpart,
            # and the old behaviour is still better than ignoring the jump.
            if insn.call_target == self.SETJMP_FN:
                lines.append(
                    "{ int _sjv = setjmp(*recomp_setjmp_slot(MEM32(esp)));"
                    " if (_sjv) { ebp = g_seh_ebp; eax = (uint32_t)_sjv; }"
                    f" else {{ PUSH32(esp, 0x{ret_va:08X}u); {name}(); }} }}"
                    f" /* setjmp 0x{insn.call_target:08X} */")
            elif insn.call_target == self.LONGJMP_FN:
                lines.append(
                    "if (!recomp_guest_longjmp(MEM32(esp), MEM32(esp + 4)))"
                    f" {{ PUSH32(esp, 0x{ret_va:08X}u); {name}(); }}"
                    f" /* longjmp 0x{insn.call_target:08X} */")
            elif insn.call_target in self.manual_functions:
                # A function the project replaces by hand. recomp_lookup_manual
                # is consulted on indirect calls, and without this a direct
                # caller went straight to the generated body and bypassed the
                # replacement silently. Contributed in #15.
                lines.append(
                    f"PUSH32(esp, 0x{ret_va:08X}u); "
                    f"RECOMP_ICALL_SAFE(0x{insn.call_target:08X}u, "
                    "_icall_esp); "
                    f"/* manual call 0x{insn.call_target:08X} */")
            else:
                # Routed through RECOMP_ABI_CALL so -DRECOMP_ABI_CHECK covers
                # direct calls too. Without it the check sees only indirect
                # ones, and CRT and static-init paths -- where callee-saved
                # clobbers actually bite -- are almost entirely direct. Expands
                # to a plain call when the flag is off.
                lines.append(
                    f"PUSH32(esp, 0x{ret_va:08X}u); "
                    f"RECOMP_ABI_CALL(0x{insn.call_target:08X}u, {name}); "
                    f"/* call 0x{insn.call_target:08X} */")
            # esp immediately after the callee returns. A per-call delta is the
            # only way to attribute a leak to one callee rather than to the
            # function containing them all.
            if self.trace_exit_name:
                lines.append(
                    f'RECOMP_TRACE_ESP("{self.trace_exit_name}", '
                    f'"after call 0x{insn.call_target:08X}");')
            # The SEH helpers exchange the frame pointer with their caller
            # through g_seh_ebp, because ebp is a C local rather than a global.
            #
            # Publishing before the call is as necessary as reading back after.
            # __SEH_prolog stores the caller's ebp into the new frame, and
            # __SEH_epilog restores esp from it before popping ebx/esi/edi. With
            # only the read-back, g_seh_ebp still held whatever the last *nested*
            # SEH function left there, so the epilog unwound to the wrong frame
            # and restored the callee-saved registers from the wrong stack slots.
            #
            # That corrupts ebx/esi/edi across any SEH function that calls
            # another - which is most of them. In Halo it left esi holding a
            # stack address where the caller had just zeroed it, so an
            # "if (status < 0)" test against esi failed and XapiInitProcess
            # bailed to the dashboard.
            if insn.call_target in (self.SEH_PROLOG, self.SEH_EPILOG):
                lines.insert(0, "g_seh_ebp = ebp; /* publish frame to SEH helper */")
                lines.append("ebp = g_seh_ebp; /* read back frame from SEH helper */")
            return lines
        elif len(ops) >= 1:
            target = _fmt_operand_read(ops[0])
            # Mark indirect calls for post-processing by _fixup_icall_esp_save
            #
            # The target is read into a local BEFORE the return-address push.
            # On real x86 the memory operand is computed before the push, so
            # emitting the push first shifts esp by four and every esp-relative
            # target -- `call dword ptr [esp+8]`, the shape MSVC gives a
            # callback invoked through a stack argument -- is read four bytes
            # low and dispatches through the wrong slot.
            return [f"{{ uint32_t _icall_target = {target}; "
                    f"PUSH32(esp, 0x{ret_va:08X}u); "
                    "RECOMP_ICALL_SAFE(_icall_target, _icall_esp); }"
                    " /* indirect call */"]
        return ["/* call: no target */"]

    def _lift_ret(self, insn, ops):
        # x86 'ret' pops return address from stack.
        # 'ret N' also pops N extra bytes (stdcall cleanup).
        # If this function IS __SEH_prolog or __SEH_epilog, bridge ebp
        # so the caller can read back the frame pointer.
        prefix = ""
        if self.func_start in (self.SEH_PROLOG, self.SEH_EPILOG):
            prefix = "g_seh_ebp = ebp; "
        # Exit trace, for functions that return with a register the caller
        # relied on holding something else. Entry tracing alone cannot show
        # that: it tells you what went in, never what came back. Emitted at the
        # ret, after the epilogue's pops, so the values are what the caller
        # actually receives.
        if self.trace_exit_name:
            prefix = (f'RECOMP_TRACE_EXIT("{self.trace_exit_name}", '
                      f'0x{self.func_start:08X}); ') + prefix
        if len(ops) >= 1 and ops[0].type == "imm":
            n = ops[0].imm
            return [f"{prefix}esp += {4 + n}; return; /* ret {n} */"]
        return [f"{prefix}esp += 4; return; /* ret */"]

    def _is_external_target(self, addr):
        """Check if a jump target is outside the current function."""
        return not (self.func_start <= addr < self.func_end)

    def _read_jump_table(self, table_va, max_entries=256):
        """Read 32-bit jump table entries from the XBE at a given VA.
        Returns list of target addresses. Stops when an entry is not a
        valid code address or max_entries is reached."""
        if not self.xbe_data:
            return []
        offset = va_to_file_offset(table_va)
        if offset is None:
            return []
        targets = []
        for i in range(max_entries):
            o = offset + i * 4
            if o + 4 > len(self.xbe_data):
                break
            val = struct.unpack_from('<I', self.xbe_data, o)[0]
            if not is_code_address(val):
                break
            targets.append(val)
        return targets

    def _analyze_switch_table(self, ops):
        """Detect if an indirect jmp operand is an intra-function switch table.
        Pattern: jmp [reg*scale + table_base] or jmp [reg + table_base]
        Returns (targets: list[int]) if ALL table entries are within the current
        function, else empty list."""
        if not ops or ops[0].type != "mem":
            return []
        op = ops[0]
        # Need a table base (displacement) and an index register
        if not op.mem_disp or not (op.mem_index or op.mem_base):
            return []
        table_va = op.mem_disp
        targets = self.jump_table_targets.get(table_va)
        if targets is None:
            targets = self._read_jump_table(table_va)
        if not targets:
            return []
        # Truncate at the first entry outside the function rather than
        # demanding that every entry be inside it.
        #
        # _read_jump_table stops at the first value that is not a plausible
        # code address, but a switch table is followed by ordinary code, and
        # the next function's bytes routinely read as a valid .text address --
        # so the read overruns the real table and picks up garbage. Requiring
        # ALL entries to be in range then threw the whole switch away because
        # of entries that were never part of it.
        #
        # A switch table's arms all land inside their own function, so the
        # first entry that does not is exactly where the table ends. On
        # Half-Life 2's CRT format parser (sub_005B9EB0) the table at
        # 0x005BA617 has 8 real arms followed by code; the old rule resolved
        # none of them, the indexed jump became an unresolvable indirect call,
        # and sprintf silently produced the wrong string.
        inside = []
        for target in targets:
            if not (self.func_start <= target < self.func_end):
                break
            inside.append(target)
        # Two arms is the smallest thing worth calling a switch; one is more
        # likely a coincidence than a jump table.
        if len(inside) >= 2:
            return inside
        return []

    def _lift_jmp(self, insn, ops):
        if insn.jump_target:
            if self._is_external_target(insn.jump_target):
                # Tail call - no return address push (reuses current frame's)
                # Bridge ebp so the target function can inherit our frame pointer.
                if insn.jump_target in self.manual_functions:
                    tail = (
                        f"g_seh_ebp = ebp; "
                        f"RECOMP_ITAIL(0x{insn.jump_target:08X}u); return; "
                        f"/* manual tail jmp 0x{insn.jump_target:08X} */"
                    )
                    if self.trace_exit_name:
                        return [
                            f'RECOMP_TRACE_ESP("{self.trace_exit_name}", '
                            f'"tail 0x{insn.jump_target:08X}");',
                            tail,
                        ]
                    return [tail]
                name = self._call_target_name(insn.jump_target)
                tail = (f"g_seh_ebp = ebp; {name}(); return; "
                        f"/* tail jmp 0x{insn.jump_target:08X} */")
                if self.trace_exit_name:
                    # Tag the exit so a trace says which one was taken. A
                    # function whose paths all look individually balanced can
                    # still leak, and knowing the path is the difference
                    # between measuring and guessing.
                    return [f'RECOMP_TRACE_ESP("{self.trace_exit_name}", '
                            f'"tail 0x{insn.jump_target:08X}");', tail]
                return [tail]
            return [f"goto loc_{insn.jump_target:08X};"]
        elif len(ops) >= 1:
            # Detect intra-function switch tables (computed gotos)
            switch_targets = self._analyze_switch_table(ops)
            if switch_targets:
                target_expr = _fmt_operand_read(ops[0])
                unique_targets = sorted(set(switch_targets))
                lines = [f"{{ uint32_t _jt = {target_expr}; /* switch: {len(switch_targets)} entries, {len(unique_targets)} targets */"]
                for t in unique_targets:
                    lines.append(f"if (_jt == 0x{t:08X}u) goto loc_{t:08X};")
                lines.append(f"g_seh_ebp = ebp; RECOMP_ITAIL(_jt); return; }}")
                return lines
            target = _fmt_operand_read(ops[0])
            return [f"g_seh_ebp = ebp; RECOMP_ITAIL({target}); return; /* indirect tail jmp */"]
        return ["/* jmp: no target */"]

    def _lift_jcc(self, insn):
        """Standalone conditional jump (no flag-setter tracked)."""
        target = insn.jump_target
        jcc = insn.mnemonic

        # jecxz/jcxz: jump if ecx/cx is zero (not flag-based)
        if jcc in ("jecxz", "jcxz"):
            cond = "ecx == 0" if jcc == "jecxz" else "LO16(ecx) == 0"
            if target:
                if self._is_external_target(target):
                    name = self._call_target_name(target)
                    return [f"if ({cond}) {{ g_seh_ebp = ebp; {name}(); return; }} /* {jcc} */"]
                return [f"if ({cond}) goto loc_{target:08X}; /* {jcc} */"]
            return [f"/* {jcc} - no target */"]

        cond_info = COND_MAP.get(jcc)
        desc = cond_info[2] if cond_info else jcc

        # Flag tracking resets at a block boundary, because which predecessor
        # arrives is not known here. _cf survives it: it is a real variable
        # holding the carry, so a jb/jae landing on a label still reads the
        # right bit while the generic _flags fallback -- which nothing ever
        # assigns -- is silently always false. The XCompress bit reader jumps
        # into the middle of its refill exactly this way.
        cond = "_flags"
        if self.needs_cf:
            if jcc in ("jb", "jnae", "jc"):
                cond = "_cf"
            elif jcc in ("jae", "jnb", "jnc"):
                cond = "!_cf"

        if target:
            if self._is_external_target(target):
                name = self._call_target_name(target)
                return [f"if ({cond} /* {jcc}: {desc} */) {{ g_seh_ebp = ebp; {name}(); return; }}"]
            return [f"if ({cond} /* {jcc}: {desc} */) goto loc_{target:08X};"]
        return [f"/* {jcc}: {desc} - no target */"]

    # ── SETcc / CMOVcc ──

    def _lift_setcc(self, insn, ops, m):
        if len(ops) < 1:
            return [f"/* {m}: no operand */"]
        return [_fmt_operand_write(ops[0], f"_flags /* {m} */")]

    def _lift_cmovcc(self, insn, ops, m):
        if len(ops) < 2:
            return [f"/* {m}: bad operands */"]
        src = _fmt_operand_read(ops[1])
        return [f"if (_flags /* {m} */) {_fmt_operand_write(ops[0], src)}"]

    # ── String operations ──

    def _lift_rep_string(self, insn, m):
        # Every one of these steps by RECOMP_DF_STEP(size) rather than a
        # literal, because EFLAGS.DF decides the direction and the block
        # forms (memcpy/memset) are only valid forwards. See g_df in
        # recomp_types.h for what a missing direction flag actually costs.
        # Forward "rep movs" is NOT memcpy. The hardware copies one element at
        # a time, so when the ranges overlap with the destination ahead of the
        # source the copy reads bytes it has already written and the pattern
        # propagates -- which is exactly how every LZ decompressor emits a run:
        # a match of distance 1 and length N repeats one byte N times. memcpy
        # is undefined on overlap, and a vectorised one reads ahead and writes
        # the pre-copy bytes, so runs come out wrong while everything else
        # looks fine.
        #
        # Half-Life 2's disc archives decompressed to exactly the right length
        # with 165,448 wrong bytes in them, spread over 352 of 26,530 blocks:
        # every difference a zero where a repeated byte belonged. The backward
        # (DF=1) path was already an explicit loop and was already correct;
        # only the common direction took the shortcut.
        #
        # memcpy is still used when the ranges provably do not overlap, which
        # is the overwhelming majority of calls.
        if "movsb" in m:
            return ["if (!g_df) { uint8_t *_d = (uint8_t*)XBOX_PTR(edi),"
                    " *_s = (uint8_t*)XBOX_PTR(esi); uint32_t _n = ecx;",
                    "  if (_d + _n <= _s || _s + _n <= _d) memcpy(_d, _s, _n);",
                    "  else { uint32_t _i; for (_i = 0; _i < _n; _i++) _d[_i] = _s[_i]; }",
                    "  esi += ecx; edi += ecx; }",
                    "else { uint32_t _i; for (_i = 0; _i < ecx; _i++)"
                    " MEM8(edi - _i) = MEM8(esi - _i); esi -= ecx; edi -= ecx; }",
                    "ecx = 0; /* rep movsb */"]
        if "movsd" in m:
            return ["if (!g_df) { uint8_t *_d = (uint8_t*)XBOX_PTR(edi),"
                    " *_s = (uint8_t*)XBOX_PTR(esi); uint32_t _n = ecx * 4;",
                    "  if (_d + _n <= _s || _s + _n <= _d) memcpy(_d, _s, _n);",
                    "  else { uint32_t _i; for (_i = 0; _i < ecx; _i++)"
                    " MEM32(edi + _i*4) = MEM32(esi + _i*4); }",
                    "  esi += ecx * 4; edi += ecx * 4; }",
                    "else { uint32_t _i; for (_i = 0; _i < ecx; _i++)"
                    " MEM32(edi - _i*4) = MEM32(esi - _i*4); esi -= ecx * 4; edi -= ecx * 4; }",
                    "ecx = 0; /* rep movsd */"]
        if "movsw" in m:
            return ["if (!g_df) { uint8_t *_d = (uint8_t*)XBOX_PTR(edi),"
                    " *_s = (uint8_t*)XBOX_PTR(esi); uint32_t _n = ecx * 2;",
                    "  if (_d + _n <= _s || _s + _n <= _d) memcpy(_d, _s, _n);",
                    "  else { uint32_t _i; for (_i = 0; _i < ecx; _i++)"
                    " MEM16(edi + _i*2) = MEM16(esi + _i*2); }",
                    "  esi += ecx * 2; edi += ecx * 2; }",
                    "else { uint32_t _i; for (_i = 0; _i < ecx; _i++)"
                    " MEM16(edi - _i*2) = MEM16(esi - _i*2); esi -= ecx * 2; edi -= ecx * 2; }",
                    "ecx = 0; /* rep movsw */"]
        if "stosb" in m:
            return ["if (!g_df) { memset((void*)XBOX_PTR(edi), (uint8_t)eax, ecx); edi += ecx; }",
                    "else { uint32_t _i; for (_i = 0; _i < ecx; _i++)"
                    " MEM8(edi - _i) = LO8(eax); edi -= ecx; }",
                    "ecx = 0; /* rep stosb */"]
        if "stosd" in m:
            return [
                "{ uint32_t _i; int32_t _st = RECOMP_DF_STEP(4);"
                " for (_i = 0; _i < ecx; _i++) MEM32(edi + _i*_st) = eax;"
                " edi += ecx * _st; }",
                "ecx = 0; /* rep stosd */"
            ]
        if "stosw" in m:
            return [
                "{ uint32_t _i; int32_t _st = RECOMP_DF_STEP(2);"
                " for (_i = 0; _i < ecx; _i++) MEM16(edi + _i*_st) = LO16(eax);"
                " edi += ecx * _st; }",
                "ecx = 0; /* rep stosw */"
            ]
        if "cmpsb" in m:
            continue_on_equal = "repne" not in m and "repnz" not in m
            stop_condition = "!_flags" if continue_on_equal else "_flags"
            return [
                "{ int32_t _st = RECOMP_DF_STEP(1);",
                "while (ecx != 0) {",
                "    _flags = (MEM8(esi) == MEM8(edi));",
                "    esi += _st; edi += _st; ecx--;",
                f"    if ({stop_condition}) break;",
                f"}} }} /* {m} */",
            ]
        if "scasb" in m:
            continue_on_equal = "repne" not in m and "repnz" not in m
            stop_condition = "!_flags" if continue_on_equal else "_flags"
            return [
                "{ int32_t _st = RECOMP_DF_STEP(1);",
                "while (ecx != 0) {",
                "    _flags = (LO8(eax) == MEM8(edi));",
                "    edi += _st; ecx--;",
                f"    if ({stop_condition}) break;",
                f"}} }} /* {m} */",
            ]
        # The word and dword forms, same shape as the byte forms above. They
        # used to be a bare comment: nothing compared, esi/edi never advanced,
        # and the flags kept whatever the previous instruction left, so the jcc
        # reading them went wherever that pointed. D3DX compares two 1 KB
        # palettes with `repe cmpsd` before deciding whether a surface copy is
        # legal, and as a no-op that always answered "identical".
        if "cmpsw" in m or "cmpsd" in m:
            wide = "cmpsd" in m
            step, acc = (4, "MEM32") if wide else (2, "MEM16")
            continue_on_equal = "repne" not in m and "repnz" not in m
            stop_condition = "!_flags" if continue_on_equal else "_flags"
            return [
                f"{{ int32_t _st = RECOMP_DF_STEP({step});",
                "while (ecx != 0) {",
                f"    _flags = ({acc}(esi) == {acc}(edi));",
                "    esi += _st; edi += _st; ecx--;",
                f"    if ({stop_condition}) break;",
                f"}} }} /* {m} */",
            ]
        if "scasw" in m or "scasd" in m:
            wide = "scasd" in m
            step, acc = (4, "MEM32") if wide else (2, "MEM16")
            value = "eax" if wide else "LO16(eax)"
            continue_on_equal = "repne" not in m and "repnz" not in m
            stop_condition = "!_flags" if continue_on_equal else "_flags"
            return [
                f"{{ int32_t _st = RECOMP_DF_STEP({step});",
                "while (ecx != 0) {",
                f"    _flags = ({value} == {acc}(edi));",
                "    edi += _st; ecx--;",
                f"    if ({stop_condition}) break;",
                f"}} }} /* {m} */",
            ]
        return [f"/* {m} */"]

    def _lift_string_op(self, insn, m):
        # Unprefixed forms; direction still comes from EFLAGS.DF.
        if m == "movsb":
            return ["MEM8(edi) = MEM8(esi); esi += RECOMP_DF_STEP(1);"
                    " edi += RECOMP_DF_STEP(1); /* movsb */"]
        if m == "movsd":
            return ["MEM32(edi) = MEM32(esi); esi += RECOMP_DF_STEP(4);"
                    " edi += RECOMP_DF_STEP(4); /* movsd */"]
        if m == "stosb":
            return ["MEM8(edi) = LO8(eax); edi += RECOMP_DF_STEP(1); /* stosb */"]
        if m == "stosd":
            return ["MEM32(edi) = eax; edi += RECOMP_DF_STEP(4); /* stosd */"]
        if m == "lodsb":
            return ["SET_LO8(eax, MEM8(esi)); esi += RECOMP_DF_STEP(1); /* lodsb */"]
        if m == "lodsd":
            return ["eax = MEM32(esi); esi += RECOMP_DF_STEP(4); /* lodsd */"]
        if m == "movsw":
            return ["MEM16(edi) = MEM16(esi); esi += RECOMP_DF_STEP(2);"
                    " edi += RECOMP_DF_STEP(2); /* movsw */"]
        if m == "stosw":
            return ["MEM16(edi) = LO16(eax); edi += RECOMP_DF_STEP(2); /* stosw */"]
        if m == "lodsw":
            return ["SET_LO16(eax, MEM16(esi)); esi += RECOMP_DF_STEP(2); /* lodsw */"]
        return [f"/* {m} */"]

    # ── FPU (x87) ──

    # ── SSE (scalar/packed float) ──


    # ── MMX ──────────────────────────────────────────────────────
    #
    # Pentium III, so MMX is fair game and every XDK codec uses it. These were
    # all TODO comments, which is a silent no-op leaving the destination
    # holding its previous value -- a decoder written this way emits the last
    # frame, or garbage, and never says why.
    #
    # mm0..mm7 alias the x87 stack on hardware; they do not here. A title that
    # mixes the two issues emms between, and emms is a no-op for us, so the
    # aliasing buys nothing and would cost the separate x87 model.
    _MMX_BINARY = {
        "paddb": "MMX_PADDB", "paddw": "MMX_PADDW", "paddd": "MMX_PADDD",
        "psubb": "MMX_PSUBB", "psubw": "MMX_PSUBW", "psubd": "MMX_PSUBD",
        "paddsb": "MMX_PADDSB", "paddsw": "MMX_PADDSW",
        "psubsb": "MMX_PSUBSB", "psubsw": "MMX_PSUBSW",
        "paddusb": "MMX_PADDUSB", "psubusb": "MMX_PSUBUSB",
        "pmullw": "MMX_PMULLW", "pmulhw": "MMX_PMULHW",
        "pmaddwd": "MMX_PMADDWD",
        "pavgb": "MMX_PAVGB", "pavgw": "MMX_PAVGW",
        "pminsw": "MMX_PMINSW", "pmaxsw": "MMX_PMAXSW",
        "pcmpeqb": "MMX_PCMPEQB", "pcmpeqw": "MMX_PCMPEQW",
        "pcmpeqd": "MMX_PCMPEQD",
        "pcmpgtb": "MMX_PCMPGTB", "pcmpgtw": "MMX_PCMPGTW",
        "pcmpgtd": "MMX_PCMPGTD",
        "punpcklbw": "MMX_PUNPCKLBW", "punpckhbw": "MMX_PUNPCKHBW",
        "punpcklwd": "MMX_PUNPCKLWD", "punpckhwd": "MMX_PUNPCKHWD",
        "punpckldq": "MMX_PUNPCKLDQ", "punpckhdq": "MMX_PUNPCKHDQ",
        "packsswb": "MMX_PACKSSWB", "packuswb": "MMX_PACKUSWB",
        "packssdw": "MMX_PACKSSDW",
        "psadbw": "MMX_PSADBW",
        "pand": "MMX_PAND", "pandn": "MMX_PANDN",
        "por": "MMX_POR", "pxor": "MMX_PXOR",
    }
    _MMX_SHIFT = {
        "psllw": "MMX_PSLLW", "psrlw": "MMX_PSRLW", "psraw": "MMX_PSRAW",
        "pslld": "MMX_PSLLD", "psrld": "MMX_PSRLD", "psrad": "MMX_PSRAD",
        "psllq": "MMX_PSLLQ", "psrlq": "MMX_PSRLQ",
    }

    def _lift_mmx(self, insn, m, ops):
        """MMX, when at least one operand is an mm register."""
        def is_mm(op):
            return (op.type == "reg" and op.reg
                    and op.reg.startswith("mm") and not op.reg.startswith("xmm"))

        def src(op):
            if is_mm(op):
                return op.reg
            if op.type == "mem":
                return f"MMX_MEM({_fmt_mem(op)})"
            if op.type == "reg":
                return f"MMX_FROM32({op.reg})"
            return None

        if not ops:
            return [f"/* {m}: no operands */"]

        # emms/femms: only the x87 tag word, which is not modelled.
        if m in ("emms", "femms"):
            # A real statement, not a comment: this instruction genuinely has
            # no effect in this model, and the conformance suite treats a
            # comment-only lift as a silently dropped instruction -- which is
            # exactly the signal that must stay meaningful for the ones that
            # really are missing.
            return [f"(void)0; /* {m}: mm/x87 aliasing is not modelled */"]

        dst = ops[0]

        if m in self._MMX_BINARY and len(ops) >= 2:
            a, b = src(dst), src(ops[1])
            if a is None or b is None:
                return [f"/* TODO: {m} {insn.op_str} */"]
            if is_mm(dst):
                return [f"{dst.reg} = {self._MMX_BINARY[m]}({a}, {b}); /* {m} */"]
            return [f"/* TODO: {m} {insn.op_str} (dst not mm) */"]

        if m in self._MMX_SHIFT and len(ops) >= 2 and is_mm(dst):
            count = ops[1]
            if count.type == "imm":
                cnt = f"{count.imm & 0xFF}u"
            elif is_mm(count):
                cnt = f"{count.reg}.q"
            elif count.type == "mem":
                cnt = f"MMX_MEM({_fmt_mem(count)}).q"
            else:
                return [f"/* TODO: {m} {insn.op_str} */"]
            return [f"{dst.reg} = {self._MMX_SHIFT[m]}({dst.reg}, {cnt}); /* {m} */"]

        # cvtps2pi / cvttps2pi: two singles in, two dwords out. The source is
        # an xmm register or a 64-bit memory operand -- never an mm register,
        # so it does not go through src() above.
        if m in ("cvtps2pi", "cvttps2pi") and len(ops) >= 2 and is_mm(dst):
            trunc = "1" if m == "cvttps2pi" else "0"
            s_op = ops[1]
            if s_op.type == "reg" and s_op.reg and s_op.reg.startswith("xmm"):
                lo, hi = f"{s_op.reg}.f[0]", f"{s_op.reg}.f[1]"
            elif s_op.type == "mem":
                addr = _fmt_mem(s_op)
                lo, hi = f"MEMF({addr})", f"MEMF(({addr}) + 4)"
            else:
                return [f"/* TODO: {m} {insn.op_str} */"]
            return [f"{dst.reg} = MMX_FROM_PS({lo}, {hi}, {trunc}); /* {m} */"]

        if m == "pshufw" and len(ops) >= 3 and is_mm(dst):
            a = src(ops[1])
            if a is None:
                return [f"/* TODO: {m} {insn.op_str} */"]
            return [f"{dst.reg} = MMX_PSHUFW({a}, {ops[2].imm & 0xFF}u); /* pshufw */"]

        if m == "pinsrw" and len(ops) >= 3 and is_mm(dst):
            v = (f"{ops[1].reg}" if ops[1].type == "reg"
                 else f"MEM16({_fmt_mem(ops[1])})" if ops[1].type == "mem" else None)
            if v is None:
                return [f"/* TODO: {m} {insn.op_str} */"]
            return [f"{dst.reg} = MMX_PINSRW({dst.reg}, {v}, "
                    f"{ops[2].imm & 0xFF}u); /* pinsrw */"]

        if m == "pextrw" and len(ops) >= 3 and is_mm(ops[1]):
            return [f"{dst.reg} = MMX_PEXTRW({ops[1].reg}, "
                    f"{ops[2].imm & 0xFF}u); /* pextrw */"]

        if m == "pmovmskb" and len(ops) >= 2 and is_mm(ops[1]):
            return [f"{dst.reg} = MMX_PMOVMSKB({ops[1].reg}); /* pmovmskb */"]

        # movq / movd between mm, memory and GPRs.
        if m in ("movq", "movd") and len(ops) >= 2:
            wide = (m == "movq")
            if is_mm(dst):
                if is_mm(ops[1]):
                    return [f"{dst.reg} = {ops[1].reg}; /* {m} */"]
                if ops[1].type == "mem":
                    return ([f"{dst.reg} = MMX_MEM({_fmt_mem(ops[1])}); /* movq */"]
                            if wide else
                            [f"{dst.reg} = MMX_FROM32(MEM32({_fmt_mem(ops[1])}));"
                             " /* movd */"])
                if ops[1].type == "reg":
                    return [f"{dst.reg} = MMX_FROM32({ops[1].reg}); /* movd */"]
            if is_mm(ops[1]):
                if dst.type == "mem":
                    return ([f"MMX_STORE({_fmt_mem(dst)}, {ops[1].reg}); /* movq */"]
                            if wide else
                            [f"MEM32({_fmt_mem(dst)}) = {ops[1].reg}.ud[0];"
                             " /* movd */"])
                if dst.type == "reg":
                    return [f"{dst.reg} = {ops[1].reg}.ud[0]; /* movd */"]
            return [f"/* TODO: {m} {insn.op_str} */"]

        # movntq: a non-temporal store. The hint is irrelevant; the store is not.
        if m == "movntq" and len(ops) >= 2 and dst.type == "mem" and is_mm(ops[1]):
            return [f"MMX_STORE({_fmt_mem(dst)}, {ops[1].reg}); /* movntq */"]

        self.unimplemented.setdefault(m, []).append(insn.address)
        return [f"/* TODO: {m} {insn.op_str} */"]

    def _lift_sse(self, insn, m, ops):
        """Translate SSE instructions to C float operations."""
        nops = len(ops)
        if nops < 1:
            return [f"/* {m}: no operands */"]

        def _is_xmm(op):
            return op.type == "reg" and op.reg and op.reg.startswith("xmm")

        def _is_mmx(op):
            return (op.type == "reg" and op.reg and op.reg.startswith("mm")
                    and not op.reg.startswith("xmm"))

        # ── Scalar (lane 0) access ──
        # movss/addss/... genuinely operate on one 32-bit value, so they read
        # and write lane 0 explicitly rather than the whole register.
        def _sse_read(op):
            if _is_xmm(op):
                return f"{op.reg}.f[0]"
            elif op.type == "reg":
                return op.reg
            elif op.type == "mem":
                if op.mem_size == 8:
                    return f"MEMD({_fmt_mem(op)})"
                return f"MEMF({_fmt_mem(op)})"
            elif op.type == "imm":
                return _fmt_imm(op.imm)
            return f"/* sse_read? */"

        def _sse_write(op, val):
            if _is_xmm(op):
                return f"{op.reg}.f[0] = {val};"
            elif op.type == "reg":
                return f"{op.reg} = {val};"
            elif op.type == "mem":
                if op.mem_size == 8:
                    return f"MEMD({_fmt_mem(op)}) = {val};"
                return f"MEMF({_fmt_mem(op)}) = {val};"
            return f"/* sse_write? */;"

        # ── Packed (128-bit) access ──
        # A whole-register read yields a RecompXmm value; a whole-register
        # write is a statement. Memory goes through the guest translation.
        def _packed_read(op):
            if _is_xmm(op):
                return op.reg
            if op.type == "mem":
                return f"XMM_MEM({_fmt_mem(op)})"
            return None

        def _packed_write(op, val):
            if _is_xmm(op):
                return f"{op.reg} = {val};"
            if op.type == "mem":
                return f"XMM_STORE({_fmt_mem(op)}, {val});"
            return None

        def _packed_binary(helper):
            """dst = helper(dst, src) for a two-operand packed op."""
            if nops < 2:
                return None
            a = _packed_read(ops[0])
            b = _packed_read(ops[1])
            if a is None or b is None:
                return None
            written = _packed_write(ops[0], f"{helper}({a}, {b})")
            if written is None:
                return None
            return [written + f" /* {m} */"]

        # ── Moves ──
        # movaps/movups move all 16 bytes. Treating them like movss was the
        # defect that left every packed value 4 bytes wide.
        if m in ("movaps", "movups", "movapd", "movupd"):
            if nops >= 2:
                src = _packed_read(ops[1])
                if src is not None:
                    written = _packed_write(ops[0], src)
                    if written is not None:
                        return [written + f" /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]

        # movlps/movhps transfer 8 bytes into or out of one half.
        if m in ("movlps", "movhps"):
            half = "LOW" if m == "movlps" else "HIGH"
            if nops >= 2:
                if _is_xmm(ops[0]) and ops[1].type == "mem":
                    return [f"XMM_LOAD_{half}({ops[0].reg}, "
                            f"{_fmt_mem(ops[1])}); /* {m} */"]
                if ops[0].type == "mem" and _is_xmm(ops[1]):
                    return [f"XMM_STORE_{half}({_fmt_mem(ops[0])}, "
                            f"{ops[1].reg}); /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]

        if m in ("movlhps", "movhlps"):
            helper = ("XMM_MOVE_LOW_TO_HIGH" if m == "movlhps"
                      else "XMM_MOVE_HIGH_TO_LOW")
            lifted = _packed_binary(helper)
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]

        # movss/movsd are scalar. Loading from memory zeroes the upper lanes;
        # a register-to-register move leaves them untouched.
        if m in ("movss", "movsd"):
            if nops >= 2:
                scalar = ("XMM_SCALAR" if m == "movss"
                          else "XMM_SCALAR_DOUBLE")
                if _is_xmm(ops[0]) and ops[1].type == "mem":
                    return [f"{ops[0].reg} = "
                            f"{scalar}({_sse_read(ops[1])}); /* {m} */"]
                src = _sse_read(ops[1])
                return [_sse_write(ops[0], src) + f" /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]

        # movd moves 32 bits without converting. Into an XMM register it also
        # zeroes the upper lanes. The generated code redefines memcpy as a
        # guest-to-guest copy, so the bits are moved through the union instead
        # of taking the address of a host local.
        if m == "movd":
            if nops >= 2:
                if _is_xmm(ops[0]):
                    if _is_xmm(ops[1]):
                        src = f"{ops[1].reg}.u[0]"
                    elif _is_mmx(ops[1]):
                        src = f"(uint32_t){ops[1].reg}"
                    else:
                        src = _fmt_operand_read(ops[1])
                    return [f"{ops[0].reg} = XMM_SCALAR_BITS({src});"
                            " /* movd to xmm */"]
                if _is_xmm(ops[1]):
                    return [f"{_fmt_operand_write(ops[0], ops[1].reg + '.u[0]')}"
                            " /* movd */"]
                src = _fmt_operand_read(ops[1])
                return [f"{_fmt_operand_write(ops[0], src)} /* movd */"]
            return [f"/* movd {insn.op_str} */"]

        # ── Arithmetic ──
        if m in ("addss", "addsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} + {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("subss", "subsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} - {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("mulss", "mulsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} * {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("divss", "divsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"{_sse_read(ops[0])} / {_sse_read(ops[1])}") + f" /* {m} */"]
        if m in ("sqrtss", "sqrtsd"):
            if nops >= 2:
                return [_sse_write(ops[0], f"sqrtf({_sse_read(ops[1])})") + f" /* {m} */"]
        if m in ("minss", "minsd"):
            if nops >= 2:
                a, b = _sse_read(ops[0]), _sse_read(ops[1])
                return [_sse_write(ops[0], f"({a} < {b} ? {a} : {b})") + f" /* {m} */"]
        if m in ("maxss", "maxsd"):
            if nops >= 2:
                a, b = _sse_read(ops[0]), _sse_read(ops[1])
                return [_sse_write(ops[0], f"({a} > {b} ? {a} : {b})") + f" /* {m} */"]

        # ── Packed arithmetic ──
        # These used to emit a bare comment, so a matrix concatenation built
        # from shufps + mulps + addps executed as nothing at all.
        if m in ("addps", "subps", "mulps", "divps"):
            helper = {"addps": "XMM_ADD", "subps": "XMM_SUB",
                      "mulps": "XMM_MUL", "divps": "XMM_DIV"}[m]
            lifted = _packed_binary(helper)
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]

        # ── Conversions ──
        if m == "cvtsi2ss":
            if nops >= 2:
                src = _fmt_operand_read(ops[1])
                return [_sse_write(ops[0], f"(float)(int32_t){src}") + " /* cvtsi2ss */"]
        if m in ("cvtss2si", "cvttss2si"):
            if nops >= 2:
                return [_fmt_operand_write(ops[0], f"(int32_t){_sse_read(ops[1])}") + f" /* {m} */"]
        if m == "cvtsi2sd":
            if nops >= 2:
                src = _fmt_operand_read(ops[1])
                return [_sse_write(ops[0], f"(double)(int32_t){src}") + " /* cvtsi2sd */"]
        if m in ("cvtsd2si", "cvttsd2si"):
            if nops >= 2:
                return [_fmt_operand_write(ops[0], f"(int32_t){_sse_read(ops[1])}") + f" /* {m} */"]
        if m == "cvtss2sd":
            if nops >= 2:
                return [_sse_write(ops[0], f"(double){_sse_read(ops[1])}") + " /* cvtss2sd */"]
        if m == "cvtsd2ss":
            if nops >= 2:
                return [_sse_write(ops[0], f"(float){_sse_read(ops[1])}") + " /* cvtsd2ss */"]

        # ── Comparison ──
        if m in ("comiss", "comisd", "ucomiss", "ucomisd"):
            if nops >= 2:
                # Snapshot, not a comment. The consuming jcc can be several
                # instructions away, and what sits between it and here is
                # frequently a write to a register the operand address was
                # built from -- so the operands have to be read now, while
                # they still mean what the compare meant.
                return [f"_fca = {_sse_read(ops[0])}; _fcb = {_sse_read(ops[1])};"
                        f" /* {m} */"]

        # ── Bitwise ──
        # Done on the integer lanes: these carry sign-mask and select idioms
        # (fabs, negate, blend) that are meaningless as float arithmetic.
        if m in ("xorps", "xorpd"):
            if (nops >= 2 and _is_xmm(ops[0]) and _is_xmm(ops[1])
                    and ops[0].reg == ops[1].reg):
                return [f"{ops[0].reg} = XMM_ZERO(); /* {m} self = zero */"]
            lifted = _packed_binary("XMM_XOR")
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]
        if m in ("andps", "orps", "andnps"):
            helper = {"andps": "XMM_AND", "orps": "XMM_OR",
                      "andnps": "XMM_ANDN"}[m]
            lifted = _packed_binary(helper)
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]

        # ── Packed min/max ──
        if m in ("minps", "maxps"):
            helper = "XMM_MIN" if m == "minps" else "XMM_MAX"
            lifted = _packed_binary(helper)
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]

        # ── Reciprocal / rsqrt ──
        if m == "rsqrtss":
            if nops >= 2:
                return [_sse_write(ops[0], f"1.0f / sqrtf({_sse_read(ops[1])})") + " /* rsqrtss */"]
        if m == "rcpss":
            if nops >= 2:
                return [_sse_write(ops[0], f"1.0f / {_sse_read(ops[1])}") + " /* rcpss */"]

        # ── Packed sqrt / reciprocal / rsqrt ──
        # The SSE model here tracks only the low lane as a single float, so
        # these compute the low lane like their scalar ...ss forms rather than
        # all four. That is the same low-lane approximation the packed
        # arithmetic ops (addps/mulps) already use -- but computing the low lane
        # is strictly better than the TODO no-op these used to hit, which left
        # the destination stale and fed garbage into vector normalisation.
        # rsqrtps/sqrtps are the workhorse of 3D vector normalize; some titles
        # use them heavily, which is why this surfaced on those binaries.
        if m == "sqrtps":
            if nops >= 2:
                return [_sse_write(ops[0], f"sqrtf({_sse_read(ops[1])})")
                        + " /* sqrtps (low lane; 4-lane model TODO) */"]
        if m == "rsqrtps":
            if nops >= 2:
                return [_sse_write(ops[0], f"1.0f / sqrtf({_sse_read(ops[1])})")
                        + " /* rsqrtps (low lane; 4-lane model TODO) */"]
        if m == "rcpps":
            if nops >= 2:
                return [_sse_write(ops[0], f"1.0f / {_sse_read(ops[1])}")
                        + " /* rcpps (low lane; 4-lane model TODO) */"]

        # ── Packed comparison ──
        if m in ("cmpneqps", "cmpeqps", "cmpltps", "cmpleps"):
            helper = {"cmpeqps": "XMM_CMP_EQ", "cmpltps": "XMM_CMP_LT",
                      "cmpleps": "XMM_CMP_LE", "cmpneqps": "XMM_CMP_NEQ"}[m]
            lifted = _packed_binary(helper)
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]

        # ── Move mask ──
        # This feeds branches, so a hardcoded 0 silently picked one side.
        if m == "movmskps":
            if nops >= 2:
                src = _packed_read(ops[1])
                if src is not None:
                    return [_fmt_operand_write(ops[0], f"XMM_MOVEMASK({src})")
                            + f" /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]

        # ── MMX / integer SIMD ──
        if m in ("pand", "pandn", "por", "pxor", "pcmpgtd"):
            if nops >= 2:
                return [f"/* {m} {insn.op_str} (MMX/SIMD integer) */"]

        # ── Shuffle/unpack ──
        # shufps is the broadcast in every matrix concatenation.
        if m == "shufps":
            if nops >= 3 and ops[2].type == "imm":
                a = _packed_read(ops[0])
                b = _packed_read(ops[1])
                if a is not None and b is not None:
                    written = _packed_write(
                        ops[0],
                        f"XMM_SHUFFLE({a}, {b}, {_fmt_imm(ops[2].imm)})")
                    if written is not None:
                        return [written + f" /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]
        if m in ("unpcklps", "unpckhps"):
            helper = ("XMM_UNPACK_LOW" if m == "unpcklps"
                      else "XMM_UNPACK_HIGH")
            lifted = _packed_binary(helper)
            if lifted is not None:
                return lifted
            return [f"/* {m} {insn.op_str} */"]

        return [f"/* SSE: {m} {insn.op_str} */"]

    # ── FPU (x87) ──

    @staticmethod
    def _st_index(reg):
        """FPU register index from a capstone name like 'st(2)' or 'st2'."""
        mm = re.search(r"st\(?(\d+)\)?", reg or "")
        return int(mm.group(1)) if mm else 0

    @staticmethod
    def _st_expr(i):
        """C expression for FPU register st(i) relative to the current top."""
        if i == 0:
            return "fp_top()"
        if i == 1:
            return "fp_st1()"
        return f"g_fp_stack[(g_fp_top + {i}) & 7]"

    def _fcom_rhs(self, ops):
        """The value an fcom-family instruction compares st0 against.

        `fcom`/`fcomp`/`fcompp` with no operand compare st0 with st1. With a
        memory operand they compare st0 with that float/double. With an st(i)
        operand, that register. Returning fp_st1() unconditionally (the old
        behavior) is only right for the no-operand form.
        """
        if not ops:
            return "fp_st1()"
        op = ops[0]
        if getattr(op, "type", None) == "mem":
            if getattr(op, "mem_size", 4) == 8:
                return f"MEMD({_fmt_mem(op)})"
            return f"MEMF({_fmt_mem(op)})"
        if getattr(op, "type", None) == "reg" and op.reg:
            return self._st_expr(self._st_index(op.reg))
        return "fp_st1()"

    def _lift_fpu(self, insn, m, ops):
        """Basic FPU instruction translation using double locals."""
        # FPU is complex. We translate common patterns to double operations.
        # Full accuracy would require an x87 stack emulator.

        if m == "fld":
            if len(ops) >= 1:
                if ops[0].type == "mem":
                    if ops[0].mem_size == 4:
                        return [f"fp_push(MEMF({_fmt_mem(ops[0])})); /* fld float */"]
                    elif ops[0].mem_size == 8:
                        return [f"fp_push(MEMD({_fmt_mem(ops[0])})); /* fld double */"]
                    return [f"fp_push(MEMF({_fmt_mem(ops[0])})); /* fld */"]
                if ops[0].type == "reg":
                    # fld st(i) pushes a COPY of st(i). Was a no-op comment,
                    # which silently dropped a stack slot -- sub_00109150 (the
                    # world_to_view builder) duplicates values with fld st(0)
                    # three times. Capture the value first: fp_push mutates
                    # g_fp_top, so passing fp_top() directly would read the
                    # slot after the decrement.
                    src = self._st_expr(self._st_index(ops[0].reg))
                    return [f"{{ double _t = {src}; fp_push(_t); }}"
                            f" /* fld {insn.op_str} */"]
            return [f"/* fld {insn.op_str} */"]

        if m in ("fst", "fstp"):
            # Only fstp pops. fst stores st0 and leaves the stack alone. The old
            # code emitted fp_pop() for BOTH -- fp_pop() is g_fp_top++, a real
            # pop, not a no-op -- so every `fst [mem]` (store WITHOUT pop) shrank
            # the FP stack by one. valid_real_matrix4x3 does `fst [tmp]` before
            # its fabs/fcompare, so the compare ran on an emptied stack slot and
            # rejected orthonormal camera matrices at render_cameras.c:458.
            do_pop = " fp_pop();" if m == "fstp" else ""
            if len(ops) >= 1 and ops[0].type == "mem":
                pop = " fp_pop();" if m == "fstp" else ""
                if ops[0].mem_size == 4:
                    return [f"MEMF({_fmt_mem(ops[0])}) = (float)fp_top();{do_pop} /* {m} */"]
                elif ops[0].mem_size == 8:
                    return [f"MEMD({_fmt_mem(ops[0])}) = fp_top();{do_pop} /* {m} */"]
            # fst/fstp st(i): copy st0 to st(i); fstp then pops. This used to be
            # a bare comment -- a no-op -- which LEAKS the FPU stack. `fstp st(0)`
            # is the common idiom for "pop the value fptan/fsincos just pushed";
            # dropping it left every later float op one slot off. That is why
            # Halo's field-of-view came out 0.
            if len(ops) >= 1 and ops[0].type == "reg" and ops[0].reg:
                mm = re.search(r"st\(?(\d+)\)?", ops[0].reg)
                idx = int(mm.group(1)) if mm else 0
                parts = []
                if idx != 0:   # i==0 is a self-copy; skip, just (maybe) pop
                    dst = "fp_st1()" if idx == 1 else \
                          f"g_fp_stack[(g_fp_top + {idx}) & 7]"
                    parts.append(f"{dst} = fp_top();")
                if m == "fstp":
                    parts.append("fp_pop();")
                body = " ".join(parts) if parts else "(void)0;"
                return [f"{body} /* {m} st({idx}) */"]
            return [f"/* {m} {insn.op_str} */"]

        if m == "fild":
            if len(ops) >= 1 and ops[0].type == "mem":
                smem = _smem_accessor(ops[0].mem_size)
                return [f"fp_push((double){smem}({_fmt_mem(ops[0])})); /* fild */"]
            return [f"/* fild {insn.op_str} */"]

        if m in ("fist", "fistp"):
            if len(ops) >= 1 and ops[0].type == "mem":
                size = ops[0].mem_size
                mem_acc = _smem_accessor(size)
                int_type = {2: "int16_t", 4: "int32_t", 8: "int64_t"}.get(
                    size, "int32_t")
                pop = " fp_pop();" if m == "fistp" else ""
                return [f"{mem_acc}({_fmt_mem(ops[0])}) = "
                        f"({int_type})llrint(fp_top());{pop} /* {m} */"]
            return [f"/* {m} {insn.op_str} */"]

        if m in ("fadd", "faddp", "fsub", "fsubp", "fsubr", "fsubrp",
                 "fmul", "fmulp", "fdiv", "fdivp", "fdivr", "fdivrp",
                 "fiadd", "fisub", "fisubr", "fimul", "fidiv", "fidivr"):
            # x87 binary arithmetic, operand-aware. The old handlers hardcoded
            # the no-operand pop form (fp_st1() op= fp_top(); pop) for every
            # variant, so `fmul [mem]`, `fadd st(0),st(0)`, and the reversed
            # fsubr/fdivr were all wrong -- and fsubr/fdivr fell through to a
            # no-op. That corrupted the FPU stack on any float that used a
            # memory or register operand; Halo's field-of-view chain multiplied
            # by a constant with `fmul [k]` and came out 0.
            # The fi* forms are the same operations against a signed integer
            # in memory (fiadd -> fadd, fisubr -> fsubr). They are memory-only
            # and never pop.
            integer_operand = m.startswith("fi")
            base = m[:-1] if m.endswith("p") else m       # strip pop suffix
            if integer_operand:
                base = "f" + base[2:]
            reverse = base.endswith("r")                  # fsubr / fdivr
            core = base[:-1] if reverse else base         # fsub / fdiv / fadd / fmul
            cop = {"fadd": "+", "fsub": "-", "fmul": "*", "fdiv": "/"}[core]
            pops = m.endswith("p")

            def _combine(dst, src):
                if cop in ("+", "*") or not reverse:
                    return f"{dst} = {dst} {cop} {src};"
                return f"{dst} = {src} {cop} {dst};"   # reversed sub/div

            # Memory operand: dst is st0, no pop (memory forms never pop).
            if ops and ops[0].type == "mem":
                if integer_operand:
                    accessor = "SMEM16" if ops[0].mem_size == 2 else "SMEM32"
                    rhs = f"(double){accessor}({_fmt_mem(ops[0])})"
                else:
                    rhs = (f"MEMD({_fmt_mem(ops[0])})" if ops[0].mem_size == 8
                           else f"MEMF({_fmt_mem(ops[0])})")
                return [f"{_combine('fp_top()', rhs)} /* {m} {insn.op_str} */"]

            # Two register operands: fXXX st(i), st(j).
            if len(ops) >= 2 and ops[0].type == "reg" and ops[1].type == "reg":
                di, si = self._st_index(ops[0].reg), self._st_index(ops[1].reg)
                code = _combine(self._st_expr(di), self._st_expr(si))
                if pops:
                    code += " fp_pop();"
                return [f"{code} /* {m} {insn.op_str} */"]

            # One register operand. Capstone reports the pop forms this way too
            # -- `faddp st(1)` comes through as a single operand st(1), NOT as
            # two operands and NOT as the no-operand form. The pop variants put
            # the result in st(i) and pop; only the non-pop variants target st0:
            #   fadd  st(i)  ->  st0  = st0  op st(i)          (no pop)
            #   faddp st(i)  ->  st(i) = st(i) op st0 ; pop
            # Treating faddp st(i) as the no-pop st0 form left the FP stack one
            # slot deep AND wrote the wrong slot, so a normalize's sum of squares
            # double-counted -- a unit vector got length sqrt(2) and Halo's
            # world_to_view basis came out scaled by 1/sqrt(2) (0.707), failing
            # valid_real_matrix4x3.
            if len(ops) >= 1 and ops[0].type == "reg":
                si = self._st_index(ops[0].reg)
                if pops:
                    code = _combine(self._st_expr(si), "fp_top()") + " fp_pop();"
                else:
                    code = _combine("fp_top()", self._st_expr(si))
                return [f"{code} /* {m} {insn.op_str} */"]

            # No operand: the stack pop form, st1 op= st0, pop.
            code = _combine("fp_st1()", "fp_top()") + " fp_pop();"
            return [f"{code} /* {m} */"]
        if m == "fchs":
            return [f"fp_top() = -fp_top(); /* fchs */"]
        if m == "fabs":
            return [f"fp_top() = fabs(fp_top()); /* fabs */"]
        if m == "fsqrt":
            return [f"fp_top() = sqrt(fp_top()); /* fsqrt */"]
        # x87 transcendentals. None of these were implemented, so every one fell
        # through to the unknown-op path and left the FP stack untouched --
        # silently, because an unimplemented FPU op looks exactly like an
        # instruction that only had a side effect on the status word.
        #
        # Halo 2276 alone has 123 fcos, 108 fsin, 31 fpatan, 8 fptan, 5 fyl2x,
        # 3 f2xm1 and 1 fprem. Every camera matrix and rotation in the game goes
        # through them, which is why render_camera_build_frustum asserted on
        # valid_real_matrix4x3: the matrix was built out of tangents that were
        # never computed, so it held whatever had been in those globals before.
        #
        # These are exact-enough mappings onto libm. The 80-bit intermediates of
        # real x87 are not reproduced -- fp_stack is double -- which is the same
        # approximation every other op here already makes.
        if m == "fsin":
            return [f"fp_top() = sin(fp_top()); /* fsin */"]
        if m == "fcos":
            return [f"fp_top() = cos(fp_top()); /* fcos */"]
        if m == "fsincos":
            # Replaces st0 with sin, then pushes cos. Order matters: the push
            # must see the sine already stored.
            return [f"{{ double _a = fp_top(); fp_top() = sin(_a);"
                    f" fp_push(cos(_a)); }} /* fsincos */"]
        if m == "fptan":
            # st0 = tan(st0), then push 1.0. The constant push is not decoration:
            # callers use it as the denominator of a subsequent fdiv.
            return [f"{{ fp_top() = tan(fp_top()); fp_push(1.0); }} /* fptan */"]
        if m == "fpatan":
            # st1 = atan2(st1, st0), pop. Argument order is st1 over st0.
            return [f"{{ fp_st1() = atan2(fp_st1(), fp_top()); fp_pop(); }}"
                    f" /* fpatan */"]
        if m == "fyl2x":
            # st1 = st1 * log2(st0), pop.
            return [f"{{ fp_st1() = fp_st1() * log2(fp_top()); fp_pop(); }}"
                    f" /* fyl2x */"]
        if m == "fyl2xp1":
            return [f"{{ fp_st1() = fp_st1() * log2(fp_top() + 1.0); fp_pop(); }}"
                    f" /* fyl2xp1 */"]
        if m == "f2xm1":
            return [f"fp_top() = exp2(fp_top()) - 1.0; /* f2xm1 */"]
        if m in ("fprem", "fprem1"):
            # Both leave the remainder in st0 and clear C2 to say "complete".
            # fprem truncates toward zero, fprem1 rounds to nearest (IEEE), which
            # is the difference between fmod and remainder.
            fn = "fmod" if m == "fprem" else "remainder"
            return [f"fp_top() = {fn}(fp_top(), fp_st1()); /* {m} */"]
        if m == "fscale":
            return [f"fp_top() = ldexp(fp_top(), (int)fp_st1()); /* fscale */"]
        if m == "frndint":
            return [f"fp_top() = rint(fp_top()); /* frndint */"]
        if m == "fldpi":
            return [f"fp_push(3.14159265358979323846); /* fldpi */"]
        if m == "fldl2e":
            return [f"fp_push(1.44269504088896340736); /* fldl2e */"]
        if m == "fldl2t":
            return [f"fp_push(3.32192809488736234787); /* fldl2t */"]
        if m == "fldlg2":
            return [f"fp_push(0.30102999566398119521); /* fldlg2 */"]
        if m == "fldln2":
            return [f"fp_push(0.69314718055994530942); /* fldln2 */"]
        if m == "ftst":
            return [f"g_fp_cmp = (fp_top() < 0.0) ? -1 : "
                    f"(fp_top() > 0.0) ? 1 : 0; /* ftst */"]
        if m == "fxch":
            # fxch st(i) swaps st0 with st(i); the bare form is st(1). Was
            # hardcoded to st1, so fxch st(2)/st(3)/st(4) (86/7/1 sites in Halo)
            # swapped the wrong slot -- corrupting the FP stack in the camera
            # basis builder that feeds world_to_view.
            # Capstone reports fxch with BOTH operands -- (st(0), st(i)) -- and
            # it is the only x87 form that does; fadd/fcom/fld st(i) all come
            # through with the explicit register alone. Reading ops[0] therefore
            # picked up the implicit st(0) and emitted a swap of st0 with
            # itself, so every `fxch st(i)` was a silent no-op.
            i = (self._st_index(ops[-1].reg)
                 if (ops and ops[-1].type == "reg" and ops[-1].reg) else 1)
            dst = self._st_expr(i)
            return [f"{{ double _t = fp_top(); fp_top() = {dst}; {dst} = _t; }}"
                    f" /* fxch {insn.op_str} */"]
        if m in ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp"):
            # Compare st0 against the operand, not always st1. `fcomp [mem]`
            # compares st0 with the memory value; only the no-operand form
            # compares st0 with st1. Getting this wrong made every float compare
            # against a constant read a garbage st1 -- Halo's camera FOV and
            # world_to_view checks both fed on it.
            rhs = self._fcom_rhs(ops)
            # Pop count is in the mnemonic and was being ignored: fcom/fucom pop
            # nothing, fcomp/fucomp pop once, fcompp/fucompp pop twice. Emitting
            # zero pops for every form leaked a stack slot on each fcomp -- and
            # float compares are everywhere -- so g_fp_top drifted and later fld
            # st(i)/faddp read the wrong slots. valid_real_matrix4x3 calls the
            # leaking per-vector check three times, then its own dot products ran
            # on a drifted stack: an orthonormal matrix failed non-deterministically
            # at render_cameras.c:458. Compare first (rhs may be fp_st1()), then pop.
            npop = 2 if m.endswith("pp") else (1 if m.endswith("p") else 0)
            pops = " fp_pop();" * npop
            return [f"g_fp_cmp = RECOMP_FCMP(fp_top(), {rhs});"
                    f"{pops} /* {m} {insn.op_str} */"]
        if m in ("fcompi", "fcomip", "fucomi", "fucompi", "fucomip", "fcomi"):
            # These set EFLAGS directly (CF, ZF, PF) from FPU comparison
            # fcompi/fucompi pop st(0) after comparing; fcomi/fucomi do not
            pops = m.endswith("pi") or m.endswith("ip")
            pop_code = " fp_pop();" if pops else ""
            rhs = self._fcom_rhs(ops)
            return [f"g_fp_cmp = RECOMP_FCMP(fp_top(), {rhs});"
                    f"{pop_code} /* {m} */"]
        if m == "fnstsw":
            # `fnstsw ax` after an FPU compare is half of the pre-SSE float
            # branch idiom `fcomp; fnstsw ax; test ah, mask; j(p/np/z/nz)`.
            # Put the compare's C3/C2/C0 condition bits into ah so the following
            # test reads a real value instead of stale eax. g_fp_cmp is -1/0/1
            # for st0 </=/> src; the FPU sets C3 on equal (ah bit 6 = 0x40) and
            # C0 on less-than (ah bit 0 = 0x01), C2 only on unordered (NaN),
            # which non-NaN game math does not hit.
            # The status word also carries TOP in bits 11-13, which lands in
            # AH bits 3-5. We model TOP (it is g_fp_top), so emit it: leaving it
            # out made `fnstsw ax` disagree with the hardware on every AH read
            # taken while the stack was non-empty. The exception-flag byte (AL)
            # we do not model and it is zero after masked, clean operations.
            # C3/C2/C0 as the hardware sets them: unordered is C3|C2|C0, and
            # `test ah, 0x44; jp` -- the standard isnan idiom -- reads exactly
            # those two bits. Reporting equal for a NaN compare sends every
            # float classification in a title down the wrong branch.
            status = ("(uint16_t)(((g_fp_top & 7u) << 11) |"
                      " (g_fp_cmp == 2 ? 0x4500u :"
                      " g_fp_cmp < 0 ? 0x0100u :"
                      " g_fp_cmp > 0 ? 0x0000u : 0x4000u))")
            if insn.op_str.strip() in ("ax", "eax"):
                # `fnstsw ax` writes the whole of AX, not just AH.
                return [f"eax = (eax & 0xFFFF0000u) | (uint32_t){status};"
                        " /* fnstsw ax <- fpu status */"]
            if ops and ops[0].type == "mem":
                return [f"MEM16({_fmt_mem(ops[0])}) = {status};"
                        f" /* fnstsw {insn.op_str} */"]
            if ops and ops[0].type == "reg":
                return [_fmt_set_reg(ops[0].reg, status)
                        + f" /* fnstsw {insn.op_str} */"]
            return [f"/* fnstsw {insn.op_str} - no destination operand */"]
        if m == "fnstcw":
            # The CRT reads the control word back to decide whether an
            # exception is masked, so it must be stored, not dropped.
            if ops and ops[0].type == "mem":
                return [f"MEM16({_fmt_mem(ops[0])}) = g_fp_control_word;"
                        f" /* fnstcw {insn.op_str} */"]
            if ops and ops[0].type == "reg":
                return [_fmt_set_reg(ops[0].reg, "g_fp_control_word")
                        + f" /* fnstcw {insn.op_str} */"]
            return [f"/* fnstcw {insn.op_str} - no destination operand */"]
        if m == "fldcw":
            if ops and ops[0].type == "mem":
                return [f"g_fp_control_word = MEM16({_fmt_mem(ops[0])});"
                        f" /* fldcw {insn.op_str} */"]
            if ops and ops[0].type == "reg":
                return [f"g_fp_control_word = (uint16_t)"
                        f"{_fmt_operand_read(ops[0])};"
                        f" /* fldcw {insn.op_str} */"]
            return [f"/* fldcw {insn.op_str} - no source operand */"]
        if m == "fldz":
            return [f"fp_push(0.0); /* fldz */"]
        if m == "fld1":
            return [f"fp_push(1.0); /* fld1 */"]

        return [f"/* FPU: {m} {insn.op_str} */"]


def lift_basic_block(lifter, bb, flag_state=None):
    """
    Lift a basic block to C statements.
    Tracks flags to generate proper conditions for jcc/setcc/cmovcc.

    Args:
        lifter: Lifter instance
        bb: BasicBlock with instructions
        flag_state: tuple of (flag_setter_mnemonic, flag_operands) from
                    a preceding block, or None

    Returns:
        (stmts, flag_state) where stmts is a list of C statement strings
        and flag_state is a tuple for passing to the next block.
    """
    stmts = []
    insns = bb.instructions
    i = 0

    # Track the last instruction that set flags
    if flag_state:
        last_flag_setter, last_flag_ops = flag_state
    else:
        last_flag_setter = None
        last_flag_ops = []

    while i < len(insns):
        curr = insns[i]

        # Try cmp/test + jcc pattern first (2-instruction match)
        match = try_match_cmp_jcc(insns, i, lifter=lifter)
        if match:
            stmt, consumed = match
            flag_insn = insns[i]
            # The fused form tests the operands inline, but the flags stay live
            # for any later jcc, and that one reads the snapshot. Emit the
            # snapshot here too or those temps are stale - which silently sends
            # every reusing branch the wrong way.
            if flag_insn.mnemonic in ("cmp", "test") and len(flag_insn.operands) >= 2:
                stmts.extend(lifter._snapshot_flags(
                    flag_insn, flag_insn.operands, flag_insn.mnemonic))
            stmts.append(stmt)
            last_flag_setter = flag_insn.mnemonic
            last_flag_ops = list(flag_insn.operands)
            i += consumed
            continue

        # Handle jecxz/jcxz specially (not flag-based)
        if curr.mnemonic in ("jecxz", "jcxz"):
            results = lifter._lift_jcc(curr)
            stmts.extend(results)
            i += 1
            continue

        # Check if this instruction uses flags (jcc, setcc, cmovcc)
        if curr.is_cond_jump and last_flag_setter:
            result = _make_condition(
                curr.mnemonic, last_flag_setter, last_flag_ops)
            if result:
                cond_expr, desc = result
                target = curr.jump_target
                stmt = _emit_cond_goto(
                    cond_expr, curr.mnemonic, desc, target, lifter)
                stmts.append(stmt)
                i += 1
                continue

        if (curr.mnemonic in ("sete", "setne", "setb", "setae", "setbe",
                              "seta", "setl", "setge", "setle", "setg",
                              "sets", "setns")
                and last_flag_setter and len(curr.operands) >= 1):
            cond = _make_setcc_value(
                curr.mnemonic, last_flag_setter, last_flag_ops)
            if cond:
                stmts.append(
                    _fmt_operand_write(curr.operands[0],
                                       f"({cond}) ? 1 : 0")
                    + f" /* {curr.mnemonic} */")
                i += 1
                continue

        if (curr.mnemonic in ("cmove", "cmovne", "cmovb", "cmovae",
                              "cmovbe", "cmova", "cmovl", "cmovge",
                              "cmovle", "cmovg", "cmovs", "cmovns")
                and last_flag_setter and len(curr.operands) >= 2):
            cond = _make_cmovcc_cond(
                curr.mnemonic, last_flag_setter, last_flag_ops)
            if cond:
                src = _fmt_operand_read(curr.operands[1])
                stmts.append(
                    f"if ({cond}) "
                    + _fmt_operand_write(curr.operands[0], src)
                    + f" /* {curr.mnemonic} */")
                i += 1
                continue

        # NEG sets CF when its operand is nonzero. Preserve that value when
        # a later SBB/ADC consumes it, skipping over EFLAGS-preserving
        # instructions (e.g. neg eax; push edi; sbb eax, eax).
        if curr.mnemonic == "neg":
            j = i + 1
            while (j < len(insns)
                    and insns[j].mnemonic in _EFLAGS_PRESERVE
                    and insns[j].mnemonic != "popfd"
                    and not insns[j].is_branch
                    and not insns[j].is_call
                    and not insns[j].is_ret):
                j += 1
            preserve = (j < len(insns)
                        and insns[j].mnemonic in ("sbb", "adc"))
            results = lifter._lift_neg(
                curr, curr.operands, preserve_carry=preserve)
        else:
            results = lifter.lift_instruction(insns[i])
        stmts.extend(results)

        # Track flag-setting instructions
        if curr.mnemonic in FLAG_SETTERS:
            last_flag_setter = curr.mnemonic
            last_flag_ops = list(curr.operands)
        elif curr.mnemonic in _FLAGS_UNDEFINED:
            # Flags are undefined after these - clear tracking
            last_flag_setter = None
            last_flag_ops = []
        elif curr.mnemonic in _EFLAGS_SETTERS:
            # Additional flag-setting instructions
            last_flag_setter = curr.mnemonic
            last_flag_ops = list(curr.operands)
        elif curr.mnemonic in _EFLAGS_PRESERVE:
            pass  # These don't affect EFLAGS
        elif curr.mnemonic in ("fcompi", "fcomip", "fucomi", "fucompi",
                                "fucomip", "fcomi"):
            # FPU compare-to-EFLAGS: sets CF, ZF, PF directly
            last_flag_setter = curr.mnemonic
            last_flag_ops = list(curr.operands)
        elif curr.mnemonic == "sahf":
            # sahf loads AH into flags - typically after fnstsw ax
            # in the fcomp/fnstsw/sahf pattern for FPU comparisons
            last_flag_setter = "sahf"
            last_flag_ops = list(curr.operands)
        elif curr.mnemonic.startswith("f") or curr.mnemonic.startswith("cmov"):
            pass  # FPU and already-handled CMOVcc
        elif curr.mnemonic.startswith("j"):
            pass  # Jumps don't set flags
        elif curr.mnemonic.startswith("set"):
            pass  # SETcc doesn't set flags
        elif curr.mnemonic.startswith("rep"):
            # rep movsb/movsd = data copy, preserves flags
            # repe cmpsb/repne scasb = comparison, sets flags
            rest = curr.op_str.strip() if hasattr(curr, 'op_str') else ""
            raw_m = curr.mnemonic
            if "cmpsb" in raw_m or "scasb" in raw_m:
                last_flag_setter = raw_m
                last_flag_ops = list(curr.operands)
            elif "cmpsb" in rest or "scasb" in rest:
                last_flag_setter = raw_m
                last_flag_ops = list(curr.operands)
            else:
                pass  # rep movs/stos = data movement, flags preserved
        else:
            # Unknown instruction - conservatively clear flag state
            last_flag_setter = None
            last_flag_ops = []

        i += 1

    out_flag_state = (last_flag_setter, last_flag_ops) if last_flag_setter else None
    return stmts, out_flag_state
