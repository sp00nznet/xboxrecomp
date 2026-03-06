# The Register Model

How global registers enable correct static recompilation of x86 Xbox code to C.

## The Problem

x86 has only 8 general-purpose registers (eax, ecx, edx, ebx, esi, edi, esp, ebp), and they are shared across all code in a process. When function A sets `esi = 0x557880` and calls function B, B can read that value from esi. Arguments are passed via registers and the stack. Return values come back in eax. The calling convention is not just a suggestion -- it is the ABI that the compiled binary relies on.

When you statically recompile x86 machine code to C, you need to preserve this shared-register semantics exactly. The simplest correct approach: make each x86 register a C global variable.

## Register Categories

### Volatile Registers (Caller-Saved)

These registers can be freely clobbered by any function call. The caller must save them if it needs their values after a call.

```c
extern uint32_t g_eax, g_ecx, g_edx, g_esp;
```

| Register | Role |
|----------|------|
| `g_eax`  | Return values, general accumulator, multiply/divide results |
| `g_ecx`  | `this` pointer for thiscall, loop counter (REP/LOOP) |
| `g_edx`  | High dword of 64-bit multiply/divide, general scratch |
| `g_esp`  | Stack pointer -- points into Xbox memory, starts at 0x00F7FFF0 |

### Callee-Saved Registers

These registers must be preserved across function calls. In the original x86 code, callee-saved registers are preserved via PUSH at function entry and POP at function exit. The recompiled code replicates this exactly:

```c
extern uint32_t g_ebx, g_esi, g_edi;
```

They are global (not local) because callers pass implicit parameters through them. For example, many Burnout 3 functions use `esi` as a `this` pointer in thiscall convention. The callee-save contract is enforced by the generated PUSH32/POP32 instructions, not by C scoping.

A typical callee-saved prologue in generated code:

```c
void sub_000636D0(void) {
    uint32_t ebp;  // local!
    PUSH32(esp, ebx);   // save callee-saved registers
    PUSH32(esp, esi);
    PUSH32(esp, edi);
    PUSH32(esp, ebp);

    // ... function body uses g_ebx, g_esi, g_edi freely ...

    POP32(esp, ebp);    // restore callee-saved registers
    POP32(esp, edi);
    POP32(esp, esi);
    POP32(esp, ebx);
}
```

### ebp: The Exception

**ebp is NOT global.** It is declared as a local `uint32_t ebp` in every generated function.

Why? Over 20,000 functions in the Burnout 3 binary use Frame Pointer Omission (FPO). With FPO, the compiler treats ebp as a general-purpose scratch register and does not save/restore it. If ebp were global, function A could set ebp to some value, call function B (which uses ebp as scratch without saving it), and when B returns, A's ebp is destroyed.

By making ebp local, each function gets its own copy via C's stack frame mechanism. FPO functions can use it freely without corrupting anyone else's value.

### The SEH Bridge: g_seh_ebp

The one case where ebp's locality causes a problem is Structured Exception Handling (SEH). The Xbox CRT's `__SEH_prolog` function (at VA 0x00244784) sets up an exception frame and initializes ebp for the calling function. But since ebp is local, the caller cannot see the change that `__SEH_prolog` made to its local ebp.

The solution is a bridge variable:

```c
extern uint32_t g_seh_ebp;
```

`__SEH_prolog` writes its computed ebp value to `g_seh_ebp`. The calling function reads it back after the call:

```c
// In the caller (generated code):
sub_00244784();          // __SEH_prolog -- sets g_seh_ebp
ebp = g_seh_ebp;        // read back the value

// ... function body using local ebp ...

g_seh_ebp = ebp;        // write it for __SEH_epilog
sub_00244797();          // __SEH_epilog
```

## Register Name Aliases

Generated code uses natural x86 register names. A set of preprocessor macros maps them to the globals, but only when `RECOMP_GENERATED_CODE` is defined (to avoid polluting hand-written code):

```c
#ifdef RECOMP_GENERATED_CODE
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
#define ebx g_ebx
#define esi g_esi
#define edi g_edi
/* ebp is NOT aliased -- it stays local in each function */
#endif
```

This means generated code reads naturally:

```c
void sub_00011A30(void) {
    uint32_t ebp;
    PUSH32(esp, ebp);
    ebp = esp;
    eax = MEM32(ebp + 8);    // first argument from Xbox stack
    ecx = MEM32(0x4D532C);   // global variable
    eax = eax + ecx;
    esp = ebp;
    POP32(esp, ebp);
    // return value in eax
}
```

## Stack Simulation

The Xbox stack lives in mapped Xbox memory. `g_esp` starts at the top of an 8 MB region:

```c
#define XBOX_STACK_BASE  0x00780000
#define XBOX_STACK_SIZE  (8 * 1024 * 1024)
#define XBOX_STACK_TOP   (XBOX_STACK_BASE + XBOX_STACK_SIZE - 16)

// During init:
g_esp = XBOX_STACK_TOP;  // 0x00F7FFF0
```

Push and pop operations read/write through the MEM macros, which translate Xbox virtual addresses to actual mapped memory:

```c
#define PUSH32(sp, val) do { \
    uint32_t _pv = (uint32_t)(val); \
    (sp) -= 4; \
    MEM32(sp) = _pv; \
} while(0)

#define POP32(sp, dst) do { \
    (dst) = MEM32(sp); \
    (sp) += 4; \
} while(0)
```

Note that PUSH32 evaluates `val` before decrementing `sp`. This matches x86 semantics where `push [esp+N]` reads the operand at the old ESP before adjusting.

## Calling Convention: void(void)

All recompiled functions have the same C signature:

```c
typedef void (*recomp_func_t)(void);
```

This is not a simplification -- it is the natural representation of x86 function calls when registers and stack are global state. Arguments are passed through:
1. **Registers**: ecx for thiscall `this`, eax/ecx/edx for fastcall, etc.
2. **Xbox stack**: pushed via PUSH32 before the call, read via `MEM32(esp + offset)` inside the callee.

Return values are communicated through `g_eax` (or `g_eax:g_edx` for 64-bit returns).

A call sequence in generated code:

```c
// Caller pushes arguments (right to left for cdecl/stdcall):
PUSH32(esp, 0x00000001);      // arg2
PUSH32(esp, MEM32(esi + 8));  // arg1
PUSH32(esp, 0xDEAD0000);      // dummy return address

// Call the function:
sub_000636D0();

// For cdecl, caller cleans the stack:
esp += 12;  // pop 2 args + dummy ret addr

// Return value is now in eax
```

## Compiler Optimization

A common concern is that global variables are slow because the compiler must reload them from memory after every function call (it cannot keep them in CPU registers across calls since the callee might modify them). In practice, MSVC handles this surprisingly well:

1. **Within basic blocks** (straight-line code between branches), MSVC keeps global register values in CPU registers and only writes back when needed.
2. **The globals are hot** -- they are accessed on nearly every line of generated code, so they stay in L1 cache permanently.
3. **Profile-guided optimization (PGO)** can further reduce redundant loads/stores.
4. **Link-time code generation (LTCG)** allows MSVC to see across translation units and optimize global access patterns.

The alternative (passing registers as function parameters) would require changing the signature of all 22,000+ functions and managing the parameter passing at every call site. The global approach is simpler, correct, and performs well enough.

## Flag Computation

x86 instructions set CPU flags (Zero, Sign, Carry, Overflow) as side effects. The recompiler does not maintain a global flags register. Instead, it pattern-matches common `cmp; jcc` and `test; jcc` sequences and emits inline comparisons:

```c
// x86: cmp eax, 0x10; jge loc_1234
if (CMP_GE(eax, 0x10)) goto loc_1234;

// x86: test ecx, ecx; jz loc_5678
if (TEST_Z(ecx, ecx)) goto loc_5678;
```

The comparison macros handle signedness correctly:

```c
#define CMP_GE(a, b)  ((int32_t)(a) >= (int32_t)(b))     // signed >=
#define CMP_AE(a, b)  ((uint32_t)(a) >= (uint32_t)(b))    // unsigned >=
#define TEST_Z(a, b)  (((uint32_t)(a) & (uint32_t)(b)) == 0)
```

## Byte and Word Register Access

x86 allows accessing sub-registers (al, ah, ax, bl, etc.). The recompiler handles this with extraction and insertion macros:

```c
#define LO8(r)   ((uint8_t)((r) & 0xFF))           // al, bl, cl, dl
#define HI8(r)   ((uint8_t)(((r) >> 8) & 0xFF))    // ah, bh, ch, dh
#define LO16(r)  ((uint16_t)((r) & 0xFFFF))         // ax, bx, cx, dx

#define SET_LO8(r, v)  ((r) = ((r) & 0xFFFFFF00u) | ((uint32_t)(uint8_t)(v)))
#define SET_HI8(r, v)  ((r) = ((r) & 0xFFFF00FFu) | (((uint32_t)(uint8_t)(v)) << 8))
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(uint16_t)(v)))
```

For example, `mov al, [esi+4]` becomes:

```c
SET_LO8(eax, MEM8(esi + 4));
```

This preserves the upper 24 bits of eax, matching x86 behavior exactly.

## Summary

| Aspect | Approach |
|--------|----------|
| Volatile registers | Global variables (g_eax, g_ecx, g_edx, g_esp) |
| Callee-saved registers | Global variables (g_ebx, g_esi, g_edi), saved/restored by PUSH32/POP32 |
| ebp | Local variable per function (20K+ FPO functions use it as scratch) |
| SEH ebp bridge | g_seh_ebp written by prolog, read by caller |
| Function signatures | All void(void) -- args via stack/registers, returns via g_eax |
| Stack | g_esp indexes into mapped Xbox memory via MEM macros |
| Flags | Pattern-matched inline comparisons, no global flags register |
| Sub-registers | Extraction/insertion macros (LO8, SET_LO8, etc.) |
