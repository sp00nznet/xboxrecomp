# Indirect Call Dispatch

The hardest problem in static recompilation: resolving function pointers and vtable calls at runtime.

## What Are Indirect Calls?

In x86, a direct call looks like `call 0x000636D0` -- the target is a constant embedded in the instruction. The recompiler handles these trivially by emitting `sub_000636D0();`.

An indirect call looks like:

```asm
call [eax]          ; call through function pointer
call [ecx+0x10]     ; vtable dispatch (method at vtable offset 0x10)
call [0x0036B7C0]   ; call through global function pointer table
```

The target address is computed at runtime. The recompiler cannot know at compile time which function will be called. This is the fundamental challenge of static recompilation.

## Sources of Indirect Calls

In Burnout 3, indirect calls come from:

1. **C++ vtable dispatch**: `obj->vtable[method_index]()` -- the most common source. RenderWare is an object-oriented engine with deep class hierarchies.
2. **Function pointer callbacks**: Event handlers, state machine transitions, resource loaders.
3. **Xbox kernel thunks**: The kernel thunk table at 0x0036B7C0 contains function pointers to kernel imports. Game code calls kernel functions via `call [thunk_address]`.
4. **COM interfaces**: D3D8 device methods are called through COM vtables.
5. **Jump tables**: Switch statements compiled to indexed jumps (these are actually indirect jumps, not calls, but face the same problem).

## The RECOMP_ICALL Macro

Every indirect call in generated code goes through the RECOMP_ICALL macro. It implements a 3-tier lookup:

```c
#define RECOMP_ICALL(xbox_va) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    \
    /* Trace ring buffer for crash diagnosis */ \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    \
    /* Garbage pointer early-out: VAs in [0x400000, 0xFE000000) */ \
    /* are outside both code range and kernel thunk range */ \
    if (_va >= 0x00400000 && _va < 0xFE000000) { \
        g_esp += 4; eax = 0; break; \
    } \
    \
    /* Tier 1: Manual overrides (hand-written replacements) */ \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    \
    /* Tier 2: Auto-generated dispatch table (22K entries) */ \
    if (!_fn) _fn = recomp_lookup(_va); \
    \
    /* Tier 3: Kernel bridge (thunks at 0xFE000000+) */ \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    \
    if (_fn) _fn(); \
    else { g_esp += 4; eax = 0; } /* not found: clean up */ \
} while(0)
```

### Tier 1: Manual Overrides

Some functions need hand-written replacements because the generated code does not work correctly (e.g., functions that rely on GPU hardware, or functions with bugs in the recompiler output). The manual lookup checks a small table (~30 entries) first:

```c
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) {
    switch (xbox_va) {
    case 0x000636D0: return sub_000636D0;  // physics force
    case 0x000110E0: return sub_000110E0;  // frame pump
    case 0x00011240: return sub_00011240;  // resource loader
    // ... ~30 more entries
    default: return NULL;
    }
}
```

### Tier 2: Auto-Generated Dispatch Table

The main dispatch table maps all 22,097 recompiled functions. It is sorted by Xbox VA for binary search:

```c
typedef struct {
    uint32_t xbox_va;
    recomp_func_t func;
} recomp_dispatch_entry_t;

static const recomp_dispatch_entry_t g_dispatch_table[] = {
    { 0x00011000, sub_00011000 },
    { 0x00011040, sub_00011040 },
    { 0x000110E0, sub_000110E0 },
    // ... 22,094 more entries ...
    { 0x003697E0, sub_003697E0 },
};

recomp_func_t recomp_lookup(uint32_t xbox_va) {
    // Binary search over 22K entries
    int lo = 0, hi = DISPATCH_TABLE_SIZE - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_dispatch_table[mid].xbox_va == xbox_va)
            return g_dispatch_table[mid].func;
        if (g_dispatch_table[mid].xbox_va < xbox_va)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;  // not found
}
```

Binary search over 22K entries is at most 15 comparisons. At ~120 ICALLs per second, this is negligible overhead.

### Tier 3: Kernel Bridge

Xbox kernel functions are not in the dispatch table. They live at synthetic VAs in the 0xFE000000+ range (one per kernel thunk slot). The kernel bridge maps these to Win32 implementations:

```c
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va) {
    if (xbox_va < KERNEL_VA_BASE || xbox_va >= KERNEL_VA_END)
        return NULL;

    uint32_t slot = (xbox_va - KERNEL_VA_BASE) / 4;
    // Each slot has a per-ordinal bridge function
    return kernel_bridge_for_slot(slot);
}
```

See [kernel-replacement.md](kernel-replacement.md) for details on the bridge functions.

## The Stack Leak Problem

When the calling convention is stdcall (callee cleans stack), the generated code pushes arguments and a dummy return address before the ICALL:

```c
PUSH32(esp, arg2);         // esp -= 4
PUSH32(esp, arg1);         // esp -= 4
PUSH32(esp, 0xDEAD0000);  // dummy return address, esp -= 4
RECOMP_ICALL(target_va);
// If stdcall, callee pops args + ret addr
// If cdecl, caller does: esp += 12;
```

If the lookup fails (unknown VA), the callee never runs, and the dummy return address plus arguments stay on the stack. Every failed ICALL leaks 4+ bytes of stack space. At 60 FPS with multiple failed ICALLs per frame, the stack grows unbounded.

**Fix**: When a lookup fails, pop the dummy return address:

```c
else { g_esp += 4; eax = 0; }  // pop dummy ret addr, return 0
```

This does not clean up the arguments (they vary per call site), but the 8 MB stack provides enough headroom. For cases where argument cleanup is critical, use RECOMP_ICALL_SAFE:

```c
#define RECOMP_ICALL_SAFE(xbox_va, saved_esp) do { \
    /* ... same lookup logic ... */ \
    if (_fn) _fn(); \
    else { g_esp = (saved_esp); eax = 0; } /* restore full stack */ \
} while(0)
```

The caller saves esp before pushing arguments:

```c
uint32_t saved_esp = esp;
PUSH32(esp, arg1);
PUSH32(esp, 0xDEAD0000);
RECOMP_ICALL_SAFE(MEM32(ecx + 0x10), saved_esp);
```

## Garbage Pointer Detection

Corrupted vtable pointers are the #1 source of ICALL failures. When game objects are not fully initialized (because we stubbed their constructors), their vtable pointers contain whatever was in memory -- often addresses like 0x1C45BA68 or 0x76657240 that are clearly not valid code addresses.

### Centralized Range Check

Valid code VAs are in [0x00011000, 0x003B0000). Kernel thunks are at 0xFE000000+. Anything in between is garbage:

```c
if (_va >= 0x00400000 && _va < 0xFE000000) {
    g_esp += 4; eax = 0; break;  // garbage VA
}
```

This single check eliminated the most common garbage VA (0x1C45BA68) and reduced ICALL failures from 180 to 121 per 2-second window -- a 33% reduction.

### Per-Function Vtable Guards

Some functions iterate over linked lists or arrays of objects, calling vtable methods on each. If any object has a corrupted vtable, the function crashes. For these functions, add guards before the ICALL:

```c
// In sub_001B4170 (recomp_0004.c) -- linked list vtable dispatch
uint32_t vtable_va = MEM32(MEM32(esi));
if (vtable_va >= 0x00400000 && vtable_va < 0xFE000000) {
    goto loc_001B41E0;  // skip this object, continue to next
}
PUSH32(esp, 0);
RECOMP_ICALL(MEM32(vtable_va + 0x10));
```

Known functions requiring guards in Burnout 3:
- `sub_0017D790`: vtable guard at loc_0017D88D (range check before 3 ICALLs)
- `sub_001B4170`: vtable loop guard (linked list dispatch)
- `sub_001B41F0`: vtable loop guard
- `sub_001AEE20`: vtable guard (skips to loc_001AEE96)

## Indirect Tail Calls

An indirect jump (not call) reuses the current frame's return address:

```asm
jmp [eax+0x0C]    ; tail call through vtable
```

No dummy return address is pushed. The RECOMP_ITAIL macro handles this:

```c
#define RECOMP_ITAIL(xbox_va) do { \
    recomp_func_t _fn = recomp_lookup_manual((uint32_t)(xbox_va)); \
    if (!_fn) _fn = recomp_lookup((uint32_t)(xbox_va)); \
    if (!_fn) _fn = recomp_lookup_kernel((uint32_t)(xbox_va)); \
    if (_fn) _fn(); \
} while(0)
```

No stack cleanup on failure -- there is nothing to clean up.

## ICALL Diagnostics

### Trace Ring Buffer

The last 16 ICALL targets are stored in a ring buffer for crash diagnosis:

```c
#define ICALL_TRACE_SIZE 16
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];
volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count;
```

When the game crashes, inspect `g_icall_trace` in the debugger to see the most recent indirect call targets. The crashing ICALL is typically the last entry.

### Failure Logging

Unknown VAs are logged with the caller's return address for tracing back to the call site:

```c
void recomp_icall_fail_log(uint32_t va) {
    void *caller = _ReturnAddress();
    fprintf(stderr, "[ICALL-FAIL] VA=0x%08X caller=%p\n", va, caller);
    // Look up caller in burnout3.map to find the source function
}
```

The .map file (generated by MSVC linker) maps native addresses back to function names, allowing you to identify which recompiled function is making the failing ICALL.

### Rate Monitoring

Track ICALLs per time window to measure improvement:

```c
// In the frame pump:
static uint64_t last_count = 0;
static DWORD last_time = 0;
DWORD now = GetTickCount();
if (now - last_time >= 2000) {
    uint64_t delta = g_icall_count - last_count;
    fprintf(stderr, "[ICALL] %llu calls in 2s (rate: %llu/s)\n",
            delta, delta / 2);
    last_count = g_icall_count;
    last_time = now;
}
```

## Performance Characteristics

| Metric | Value |
|--------|-------|
| Dispatch table size | 22,097 entries |
| Lookup method | Binary search (15 comparisons max) |
| ICALL rate (typical) | ~60/second during gameplay |
| ICALL rate (init) | ~500/second during game boot |
| Failed ICALLs (before guards) | ~90/second |
| Failed ICALLs (after guards) | ~60/second |
| Stack leak per failed ICALL | 4 bytes (dummy return address) |
| Stack headroom | 8 MB (sufficient for hours of gameplay) |

## Key Insight

The hardest 10% of static recompilation is indirect calls. Direct calls are mechanical translation. Memory access is solved by the mapping layer. But indirect calls require runtime dispatch with correct stack management, failure recovery, garbage detection, and per-function workarounds for corrupted vtables. Every new crash in the recompiled game is almost certainly an ICALL problem.
