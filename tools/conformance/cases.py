"""Snippets whose net effect lands in eax.

Each case is a short x86 sequence, written as MSVC inline-assembly text so the
assembler -- not us -- decides the encoding. The same bytes are then executed
natively and lifted to C, and the two eax values are compared.

Inputs are (eax, ecx) pairs, chosen for the edges that matter: sign-bit
boundaries at each operand width, zero, and -1.
"""

def Case(name, why, asm, inputs, kind="gpr", tol=0.0):
    """One snippet, its inputs, and what gets compared afterwards.

    kind:
      "gpr"  inputs are (eax, ecx) uint32 pairs; eax is compared.
      "fpu"  inputs are (double, double) written to the scratch buffer; the
             x87 stack *depth* and every live st(i) are compared, plus eax
             (so `fnstsw ax` cases work). Depth is the point: a handler that
             pops the wrong number of times is the bug class that desynced
             Halo's camera maths, and the value alone will not show it.
      "sse"  inputs are two 4-float tuples in the scratch buffer; all eight
             XMM registers are compared as raw bytes, plus eax.

    For "fpu" and "sse", eax points at a 64-byte scratch buffer:
      [eax]      operand A (16 bytes: 4 floats, or a double in the low 8)
      [eax+16]   operand B
      [eax+32]   free space for the snippet to store into
    Guest addresses equal host addresses in the harness, so both sides read
    the same bytes.

    tol: relative tolerance for "fpu" comparisons. Zero -- bit-exact -- is the
    default and is what almost everything should use: at PC=53 the hardware
    rounds add/sub/mul/div/sqrt to double exactly as C does. Only the
    transcendentals need slack, because the x87's polynomial and libm's are
    different implementations of the same function and differ in the last
    place. That is a real divergence between the recompiled code and the
    hardware, not a lifting bug, and it is recorded here rather than hidden by
    a blanket tolerance so a genuinely wrong result still fails.
    """
    return {"name": name, "why": why, "asm": asm, "inputs": inputs,
            "kind": kind, "tol": tol}

# Values that sit on a boundary at one width but not another. A bug that
# evaluates an 8- or 16-bit operand at 32 bits shows up here and nowhere else.
_EDGES = [0x00000000, 0x00000001, 0x0000007F, 0x00000080, 0x000000FF,
          0x00007FFF, 0x00008000, 0x0000FFFF, 0x7FFFFFFF, 0x80000000,
          0xFFFFFFFF, 0x12345678]

_PAIRS = [(a, b) for a in _EDGES for b in (0x00000001, 0x0000007F, 0x00000080,
                                           0x000000FF, 0xFFFFFFFF)]

# Doubles for the x87 cases. Ordinary values, the signed zeroes, a tie for
# the rounding modes, and one pair that is only interesting in reverse
# (fsubr/fdivr swap the operands, so a/b and b/a must differ).
_FP = [
    (1.0, 2.0), (2.0, 1.0), (-3.25, 0.5), (0.5, -3.25),
    (100.0, 7.0), (7.0, 100.0), (1.0, 3.0), (3.0, 1.0),
    (0.0, 1.0), (1.0, 0.0), (-0.0, 1.0), (1.0, -0.0),
    (2.5, 2.0), (-2.5, 2.0), (0.5, 0.5), (-1.0, -1.0),
    (1e10, 3.0), (1e-10, 3.0), (65536.0, 256.0),
]

# Only for the cases that mask fnstsw down to the condition codes. The
# status word's low byte carries the exception flags -- invalid is set by
# a NaN compare -- and those are not modelled, so a case comparing the
# whole of ax would diverge on bits that have nothing to do with the
# comparison it is testing.
_FP_NAN = [
    (float('nan'), 1.0), (1.0, float('nan')),
    (float('nan'), float('nan')), (1.0, 2.0), (2.0, 1.0),
    (1.0, 1.0), (float('inf'), 1.0), (-0.0, 0.0),
]

# Four-float lanes for the SSE cases. Lane 0 usually differs from lanes 1-3 so
# a scalar-only lift shows up as three wrong lanes rather than a wrong answer,
# and the last two rows carry NaN and the signed zeroes for the compare and
# min/max tie-break cases.
_NAN = float("nan")
_SSE = [
    ((1.0, 2.0, 3.0, 4.0),        (10.0, 20.0, 30.0, 40.0)),
    ((-1.0, -2.0, -3.0, -4.0),    (1.0, 0.5, 0.25, 0.125)),
    ((0.0, 1.0, -1.0, 0.5),       (1.0, 1.0, 1.0, 1.0)),
    ((3.0, 3.0, 3.0, 3.0),        (3.0, 3.0, 3.0, 3.0)),
    ((1.5, -2.5, 1e10, 1e-10),    (2.5, -1.5, 1e-10, 1e10)),
    ((_NAN, 1.0, -0.0, 0.0),      (1.0, _NAN, 0.0, -0.0)),
    ((-0.0, 0.0, _NAN, _NAN),     (0.0, -0.0, _NAN, 1.0)),
]

CASES = [
    # -- signed compare width (the RECOMP_SIGNED vs SXV decision) -------------
    Case("setl_i8", "cmp at byte width, then jl's condition",
         ["cmp al, cl", "setl al", "movzx eax, al"], _PAIRS),
    Case("setl_i16", "cmp at word width",
         ["cmp ax, cx", "setl al", "movzx eax, al"], _PAIRS),
    Case("setl_i32", "cmp at dword width",
         ["cmp eax, ecx", "setl al", "movzx eax, al"], _PAIRS),
    Case("setle_i8", "<= at byte width",
         ["cmp al, cl", "setle al", "movzx eax, al"], _PAIRS),
    Case("setg_i16", "> at word width",
         ["cmp ax, cx", "setg al", "movzx eax, al"], _PAIRS),
    Case("setb_i32", "unsigned below, for contrast with the signed forms",
         ["cmp eax, ecx", "setb al", "movzx eax, al"], _PAIRS),

    # -- test / sign flag ----------------------------------------------------
    Case("tests_i8", "SF from an 8-bit test",
         ["test al, cl", "sets al", "movzx eax, al"], _PAIRS),
    Case("testz_i16", "ZF from a 16-bit test",
         ["test ax, cx", "setz al", "movzx eax, al"], _PAIRS),

    # -- xor zeroing must clear an incoming carry ----------------------------
    Case("xor_self_adc_i8", "byte zeroing clears CF before adc",
         ["stc", "xor cl, cl", "adc eax, 0"], [(a, 0) for a in _EDGES]),
    Case("xor_self_adc_hi8", "high-byte zeroing clears CF before adc",
         ["stc", "xor ch, ch", "adc eax, 0"], [(a, 0) for a in _EDGES]),
    Case("xor_self_adc_i16", "word zeroing clears CF before adc",
         ["stc", "xor cx, cx", "adc eax, 0"], [(a, 0) for a in _EDGES]),
    Case("xor_self_adc_i32", "dword zeroing clears CF before adc",
         ["stc", "xor ecx, ecx", "adc eax, 0"], [(a, 0) for a in _EDGES]),
    Case("xor_self_sbb_i32", "dword zeroing clears CF before sbb",
         ["stc", "xor ecx, ecx", "sbb eax, 0"], [(a, 0) for a in _EDGES]),

    # -- neg carry into a dependent sbb (PR #8) ------------------------------
    Case("neg_sbb_adjacent", "neg sets CF = (operand != 0); sbb consumes it",
         ["neg eax", "sbb ecx, ecx", "mov eax, ecx"], _PAIRS),
    Case("neg_sbb_separated",
         "same, with flag-safe instructions in between -- the real codegen shape",
         ["neg eax", "push edx", "mov edx, 1", "pop edx",
          "sbb ecx, ecx", "mov eax, ecx"], _PAIRS),
    Case("neg_adc", "neg's carry into adc",
         ["neg eax", "adc ecx, 0", "mov eax, ecx"], _PAIRS),

    # -- sign/zero extension -------------------------------------------------
    Case("movsx_8_32", "sign-extend byte to dword",
         ["movsx eax, al"], _PAIRS),
    Case("movsx_16_32", "sign-extend word to dword",
         ["movsx eax, ax"], _PAIRS),
    Case("movzx_8_32", "zero-extend byte to dword",
         ["movzx eax, al"], _PAIRS),
    # Divisor forced odd and positive: idiv raises #DE both on a zero divisor
    # and on INT32_MIN / -1, and a trapping case tells us nothing about lifting.
    Case("cdq_idiv", "cdq's sign-extend feeding a signed divide",
         ["or ecx, 1", "and ecx, 07FFFFFFFh", "cdq", "idiv ecx"], _PAIRS),

    # -- shifts --------------------------------------------------------------
    Case("shl_cl", "variable shift left, including the &31 masking",
         ["shl eax, cl"], _PAIRS),
    Case("sar_cl", "arithmetic right shift keeps the sign",
         ["sar eax, cl"], _PAIRS),
    Case("shr_cl", "logical right shift does not",
         ["shr eax, cl"], _PAIRS),
    Case("rol_cl", "rotate left",
         ["rol eax, cl"], _PAIRS),
    Case("shld", "double-precision shift",
         ["shld eax, ecx, 5"], _PAIRS),

    # -- arithmetic ----------------------------------------------------------
    Case("imul_32", "signed multiply, low half",
         ["imul eax, ecx"], _PAIRS),
    Case("add_adc_64", "the 64-bit add idiom: add produces carry, adc consumes",
         ["add eax, ecx", "adc edx, 0", "mov eax, edx"], _PAIRS),
    Case("sub_sbb_64", "the 64-bit subtract idiom",
         ["sub eax, ecx", "sbb edx, 0", "mov eax, edx"], _PAIRS),
    Case("inc_dec", "inc/dec leave CF alone -- a classic place to get flags wrong",
         ["stc", "inc eax", "adc ecx, 0", "mov eax, ecx"], _PAIRS),
    Case("bswap", "byte swap", ["bswap eax"], _PAIRS),


    # The dword string compares were emitted as a bare comment until the
    # Wreckless bring-up: nothing compared, esi/edi never advanced, and the
    # jcc reading the flags went wherever the previous instruction left them.
    # Buffers come off the stack so the case needs no external symbol.
    Case("repe_cmpsd_equal", "repe cmpsd over identical dwords sets ZF",
         ["push 0x11111111", "push 0x11111111",
          "mov esi, esp", "mov edi, esp", "mov ecx, 2", "repe cmpsd",
          "setz al", "movzx eax, al", "add esp, 8"], _PAIRS),
    Case("repe_cmpsd_differ", "and clears it when they differ",
         ["push 0x22222222", "push 0x11111111",
          "mov esi, esp", "lea edi, [esp+4]", "mov ecx, 1", "repe cmpsd",
          "setz al", "movzx eax, al", "add esp, 8"], _PAIRS),
    Case("repne_scasd", "repne scasd stops on a match and leaves edi past it",
         ["push 0x44444444", "push 0x33333333",
          "mov edi, esp", "mov eax, 0x44444444", "mov ecx, 2", "repne scasd",
          "sub edi, esp", "mov eax, edi", "add esp, 8"], _PAIRS),

    # bsf/bsr were emitted as TODO comments until the Wreckless bring-up.
    # RtlAllocateHeap picks a free-list bucket by bsf-ing an in-use bitmap, so
    # a bsf that does nothing returns its own argument and the allocator hands
    # out the address of an empty list head. Both the value and the zero case
    # (destination untouched, ZF set) are checked.
    Case("bsf", "bit scan forward: index of the lowest set bit",
         ["bsf eax, ecx"], _PAIRS),
    Case("bsr", "bit scan reverse: index of the highest set bit",
         ["bsr eax, ecx"], _PAIRS),
    Case("bsf_zero_leaves_dest", "bsf with a zero source must not write dest",
         ["xor ecx, ecx", "bsf eax, ecx"], _PAIRS),
    Case("bsf_zf", "ZF after bsf is set exactly when the source was zero",
         ["bsf eax, ecx", "setz al", "movzx eax, al"], _PAIRS),
    Case("bsr_zf", "and the same for bsr",
         ["bsr eax, ecx", "setz al", "movzx eax, al"], _PAIRS),

    # Nonzero-forced variants and a scan into a different destination,
    # contributed in #16. The cases above leave the source to the input
    # pairs, so these pin the common path explicitly.
    Case("bsf_nonzero", "least-significant set bit, with a nonzero source",
         ["or ecx, 1", "bsf eax, ecx"], _PAIRS),
    Case("bsr_nonzero", "most-significant set bit, with a nonzero source",
         ["or ecx, 1", "bsr eax, ecx"], _PAIRS),
    Case("bit_scan_zf", "bit scan sets ZF when its source is zero",
         ["bsf edx, ecx", "setz al", "movzx eax, al"], _PAIRS),
    Case("not_and", "bitwise", ["not eax", "and eax, ecx"], _PAIRS),


    # ══ MMX ══════════════════════════════
    #
    # Every one of these was a TODO comment, which is a silent no-op: the
    # destination kept its previous value. Wreckless's WMV decoder is 1796
    # MMX instructions of IDCT and motion compensation and D3DX's texture
    # converters another 900, so both wrote whatever was already there.
    #
    # The harness uses a 32-bit MSVC, so the host executes real MMX and is
    # a true reference. Each case builds mm0/mm1 from the vector pair and
    # reports one dword of the result.
    Case("mmx_paddw", "packed word add wraps per lane",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'paddw mm0, mm1', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_paddd", "packed dword add",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'paddd mm0, mm1', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_psubw", "packed word subtract",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'psubw mm0, mm1', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_paddsw", "saturating signed word add",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'paddsw mm0, mm1', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_pmullw", "packed word multiply, low half",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'pmullw mm0, mm1', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_pmulhw", "packed word multiply, high half",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'pmulhw mm0, mm1', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_pmaddwd", "multiply words and add adjacent pairs",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'pmaddwd mm0, mm1', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_psraw", "arithmetic word shift right saturates its count",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'psraw mm0, 3', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_psrlw", "logical word shift right",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'psrlw mm0, 5', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_pslld", "dword shift left",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'pslld mm0, 7', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_psrlq", "whole-register shift right",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'psrlq mm0, 11', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_psraw_big", "a count past the lane width is a full sign fill",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'psraw mm0, 40', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_psrlw_big", "and zero for the logical form",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'psrlw mm0, 40', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_punpcklbw", "interleave low bytes",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'punpcklbw mm0, mm1', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_punpckhbw", "interleave high bytes",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'punpckhbw mm0, mm1', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_punpcklwd", "interleave low words",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'punpcklwd mm0, mm1', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_punpckhdq", "interleave high dwords",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'punpckhdq mm0, mm1', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_packuswb", "pack words to unsigned bytes with saturation",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'packuswb mm0, mm1', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_packssdw", "pack dwords to signed words with saturation",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'packssdw mm0, mm1', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_pcmpgtw", "packed signed word compare",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'pcmpgtw mm0, mm1', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_pand", "bitwise and",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'pand mm0, mm1', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_pandn", "and-not is ~dst & src, not dst & ~src",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'pandn mm0, mm1', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_pxor", "bitwise xor",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'pxor mm0, mm1', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_pavgb", "unsigned byte average rounds up",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'pavgb mm0, mm1', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_pshufw", "word shuffle by immediate",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'pshufw mm0, mm1, 0x1B', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+0]', 'add esp, 8', 'emms'], _PAIRS),
    Case("mmx_paddw_hi", "the high lane too, not just lane 0",
         ['push eax', 'push ecx', 'push eax', 'push ecx', 'movq mm0, qword ptr [esp]', 'push eax', 'push eax', 'push ecx', 'push ecx', 'movq mm1, qword ptr [esp]', 'add esp, 32', 'paddw mm0, mm1', 'push 0', 'push 0', 'movq qword ptr [esp], mm0', 'mov eax, dword ptr [esp+4]', 'add esp, 8', 'emms'], _PAIRS),

    # ══ x87 ═════════════════════════════════════════════════════════════════
    #
    # Stack depth is compared as well as the values. Most of the x87 bugs this
    # project has had were pop-count bugs, not arithmetic bugs: a handler that
    # forgets to pop leaks a slot, everything after it reads st(i) one off, and
    # the *first* value still looks right.

    Case("fpu_load_store", "fld then fstp round-trips and leaves the stack empty",
         ["fld qword ptr [eax]", "fstp qword ptr [eax+32]"], _FP, "fpu"),
    Case("fpu_fst_no_pop", "fst stores WITHOUT popping; only fstp pops",
         ["fld qword ptr [eax]", "fst qword ptr [eax+32]"], _FP, "fpu"),
    Case("fpu_fld_st0_dup", "fld st(0) pushes a copy -- depth 2, not 1",
         ["fld qword ptr [eax]", "fld st(0)"], _FP, "fpu"),
    Case("fpu_add_mem", "fadd [mem] updates st0 in place and does not pop",
         ["fld qword ptr [eax]", "fadd qword ptr [eax+16]"], _FP, "fpu"),
    Case("fpu_addp", "faddp adds into st1 and pops",
         ["fld qword ptr [eax]", "fld qword ptr [eax+16]", "faddp st(1), st(0)"],
         _FP, "fpu"),
    Case("fpu_sub_mem", "fsub [mem]: st0 = st0 - mem",
         ["fld qword ptr [eax]", "fsub qword ptr [eax+16]"], _FP, "fpu"),
    Case("fpu_subr_mem", "fsubr [mem]: st0 = mem - st0, the reversed form",
         ["fld qword ptr [eax]", "fsubr qword ptr [eax+16]"], _FP, "fpu"),
    Case("fpu_div_mem", "fdiv [mem]: st0 = st0 / mem",
         ["fld qword ptr [eax]", "fdiv qword ptr [eax+16]"], _FP, "fpu"),
    Case("fpu_divr_mem",
         "fdivr [mem]: st0 = mem / st0 -- dropping the reverse turns a "
         "normalise into its own inverse",
         ["fld qword ptr [eax]", "fdivr qword ptr [eax+16]"], _FP, "fpu"),
    Case("fpu_mul_sti", "fmul st(1): st0 *= st1, no pop",
         ["fld qword ptr [eax]", "fld qword ptr [eax+16]", "fmul st(0), st(1)"],
         _FP, "fpu"),
    Case("fpu_mulp_sti", "fmulp st(1): st1 *= st0, then pop",
         ["fld qword ptr [eax]", "fld qword ptr [eax+16]", "fmulp st(1), st(0)"],
         _FP, "fpu"),
    Case("fpu_xch", "fxch swaps st0 and st1",
         ["fld qword ptr [eax]", "fld qword ptr [eax+16]", "fxch st(1)"],
         _FP, "fpu"),
    Case("fpu_chs_abs", "fchs / fabs",
         ["fld qword ptr [eax]", "fchs", "fabs", "fchs"], _FP, "fpu"),
    Case("fpu_sqrt", "fsqrt", ["fld qword ptr [eax]", "fabs", "fsqrt"],
         _FP, "fpu"),
    Case("fpu_cmp_pops",
         "fcompp compares and pops TWICE; fnstsw ax then carries the result "
         "into eax -- the whole pre-SSE float branch idiom in one case",
         ["fld qword ptr [eax+16]", "fld qword ptr [eax]", "fcompp",
          "fnstsw ax"], _FP, "fpu"),
    Case("fpu_comp_one_pop", "fcomp pops once, fcom not at all",
         ["fld qword ptr [eax+16]", "fld qword ptr [eax]",
          "fcom st(1)", "fcomp st(1)", "fnstsw ax"], _FP, "fpu"),
    Case("fpu_int_roundtrip", "fild / fistp integer conversion",
         ["fild dword ptr [eax]", "fistp dword ptr [eax+32]",
          "fild dword ptr [eax+32]"], _FP, "fpu"),
    Case("fpu_rndint", "frndint rounds per the control word",
         ["fld qword ptr [eax]", "frndint"], _FP, "fpu"),
    Case("fpu_patan", "fpatan: st1 = atan2(st1, st0), pop",
         ["fld qword ptr [eax]", "fld qword ptr [eax+16]", "fpatan"],
         _FP, "fpu", tol=1e-15),
    Case("fpu_sincos_pair", "fsin and fcos",
         ["fld qword ptr [eax]", "fsin", "fld qword ptr [eax]", "fcos"],
         _FP, "fpu", tol=1e-15),
    Case("fpu_ptan", "fptan replaces st0 and pushes 1.0 -- depth grows by one",
         ["fld qword ptr [eax]", "fptan"], _FP, "fpu", tol=1e-15),

    # ══ SSE ═════════════════════════════════════════════════════════════════
    #
    # All four lanes are compared, which is the whole point: modelling XMM as a
    # scalar float made movaps move 4 of 16 bytes and nothing noticed.

    Case("sse_movaps", "movaps moves all 16 bytes, not just lane 0",
         ["movaps xmm0, xmmword ptr [eax]",
          "movaps xmmword ptr [eax+32], xmm0"], _SSE, "sse"),
    Case("sse_movups", "unaligned move, same 16 bytes",
         ["movups xmm1, xmmword ptr [eax+16]"], _SSE, "sse"),
    Case("sse_arith", "packed add/sub/mul/div are lane-wise",
         ["movaps xmm0, xmmword ptr [eax]",
          "movaps xmm1, xmmword ptr [eax+16]",
          "addps xmm0, xmm1", "subps xmm0, xmm1",
          "mulps xmm0, xmm1", "divps xmm0, xmm1"], _SSE, "sse"),
    Case("sse_minmax",
         "MINPS/MAXPS return the SECOND operand on a tie or unordered "
         "compare -- that is the hardware tie-break, not fmin/fmax",
         ["movaps xmm0, xmmword ptr [eax]",
          "movaps xmm1, xmmword ptr [eax+16]",
          "movaps xmm2, xmm0", "minps xmm2, xmm1",
          "movaps xmm3, xmm0", "maxps xmm3, xmm1"], _SSE, "sse"),
    Case("sse_bitwise",
         "ANDNPS is ~dst & src, not dst & ~src",
         ["movaps xmm0, xmmword ptr [eax]",
          "movaps xmm1, xmmword ptr [eax+16]",
          "movaps xmm2, xmm0", "andps xmm2, xmm1",
          "movaps xmm3, xmm0", "orps xmm3, xmm1",
          "movaps xmm4, xmm0", "xorps xmm4, xmm1",
          "movaps xmm5, xmm0", "andnps xmm5, xmm1"], _SSE, "sse"),
    Case("sse_compares",
         "CMPNEQPS is the unordered form, so it is TRUE on NaN while "
         "EQ/LT/LE are ordered and false",
         ["movaps xmm0, xmmword ptr [eax]",
          "movaps xmm1, xmmword ptr [eax+16]",
          "movaps xmm2, xmm0", "cmpeqps xmm2, xmm1",
          "movaps xmm3, xmm0", "cmpltps xmm3, xmm1",
          "movaps xmm4, xmm0", "cmpleps xmm4, xmm1",
          "movaps xmm5, xmm0", "cmpneqps xmm5, xmm1"], _SSE, "sse"),
    Case("sse_shuffle", "shufps takes lanes 0-1 from dst and 2-3 from src",
         ["movaps xmm0, xmmword ptr [eax]",
          "movaps xmm1, xmmword ptr [eax+16]",
          "shufps xmm0, xmm1, 27"], _SSE, "sse"),
    Case("sse_unpack", "unpcklps / unpckhps interleave",
         ["movaps xmm0, xmmword ptr [eax]",
          "movaps xmm1, xmmword ptr [eax+16]",
          "movaps xmm2, xmm0", "unpcklps xmm2, xmm1",
          "movaps xmm3, xmm0", "unpckhps xmm3, xmm1"], _SSE, "sse"),
    Case("sse_halves", "movlps/movhps/movlhps/movhlps move one half",
         ["movaps xmm0, xmmword ptr [eax]",
          "movaps xmm1, xmmword ptr [eax+16]",
          "movlps xmm0, qword ptr [eax+16]",
          "movhps xmm1, qword ptr [eax]",
          "movaps xmm2, xmm0", "movlhps xmm2, xmm1",
          "movaps xmm3, xmm0", "movhlps xmm3, xmm1"], _SSE, "sse"),
    Case("sse_scalar_move",
         "movss from memory zeroes the upper three lanes; "
         "register-to-register leaves them alone",
         ["movaps xmm1, xmmword ptr [eax+16]",
          "movss xmm0, dword ptr [eax]",
          "movss xmm1, xmm0"], _SSE, "sse"),
    Case("sse_movmskps", "movmskps packs the four lane sign bits into eax",
         ["movaps xmm0, xmmword ptr [eax]", "movmskps eax, xmm0"],
         _SSE, "sse"),
    Case("sse_zero_idiom", "xorps xmm,xmm is the zeroing idiom",
         ["movaps xmm0, xmmword ptr [eax]", "xorps xmm0, xmm0"], _SSE, "sse"),

    # SF is the sign bit at the operand's width. Evaluating an 8- or 16-bit
    # test as int32 makes 0x80..0xFF look positive, so the branch goes the same
    # way regardless -- found by the whole-function corpus, kept here so the
    # unit-level suite catches it too.
    Case("jns_i8", "jns after an 8-bit test reads bit 7, not bit 31",
         ["test al, al", "setns al", "movzx eax, al"], _PAIRS),
    Case("js_i8", "js after an 8-bit test",
         ["test al, cl", "sets al", "movzx eax, al"], _PAIRS),
    Case("jns_i16", "jns after a 16-bit test reads bit 15",
         ["test ax, ax", "setns al", "movzx eax, al"], _PAIRS),
    Case("js_cmp_i8", "sign flag after an 8-bit cmp, which truncates first",
         ["cmp al, cl", "sets al", "movzx eax, al"], _PAIRS),

    # bt/btr/bts/btc are 386 instructions, so real Xbox code has them. They
    # were unhandled until the corpus lifted the CRT's float-to-int helper,
    # which uses btr to clear a rounding-control bit of the x87 control word.
    Case("bt_read", "bt sets CF from the selected bit",
         ["bt eax, ecx", "sbb eax, eax"], _PAIRS),
    Case("btr_clear", "btr reads the bit and then clears it",
         ["btr eax, 5"], _PAIRS),
    Case("bts_set", "bts sets it", ["bts eax, 5"], _PAIRS),
    Case("btc_flip", "btc complements it", ["btc eax, 5"], _PAIRS),
    Case("btr_reg", "btr with a register bit index, which is taken mod 32",
         ["btr eax, ecx"], _PAIRS),

    # An x87 compare against a NaN is unordered: C3, C2 and C0 all set. The
    # model reported "equal", so `fucompp; fnstsw ax; test ah,44h; jp` -- how
    # this era's CRT asks "is this a NaN" -- answered no every time. Found by
    # running Crimson Skies' own float classification against itself.
    Case("fpu_nan_unordered",
         "comparing a value with itself is the isnan idiom; NaN is unordered",
         ["fld qword ptr [eax]", "fld qword ptr [eax]", "fucompp",
          "fnstsw ax", "and eax, 04500h"], _FP_NAN, "fpu"),
    Case("fpu_nan_vs_number", "NaN against an ordinary value is also unordered",
         ["fld qword ptr [eax+16]", "fld qword ptr [eax]", "fucompp",
          "fnstsw ax", "and eax, 04500h"], _FP_NAN, "fpu"),
]
