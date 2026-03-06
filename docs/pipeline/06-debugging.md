# Step 6: Iterative Debugging

## The Reality

The game will crash. Constantly. The first boot attempt will fail within milliseconds. You will fix one crash, and the next instruction will crash differently. This is normal and expected.

Static recompilation is an iterative process: crash, diagnose, fix, repeat. The good news is that each fix is permanent and cumulative. After fixing 50-100 crashes, you'll hit a tipping point where the game starts running meaningful code -- loading assets, initializing the renderer, entering the main loop. After 200+ fixes, you'll see gameplay.

This document covers the tools and techniques for efficient debugging.

## ICALL Trace Ring Buffer

The most common crash type is a failed indirect call -- the game tries to call a function through a pointer, but the pointer is garbage (corrupted vtable, uninitialized data, address space mismatch).

The runtime maintains a ring buffer of the last 16 indirect call targets:

```c
#define ICALL_TRACE_SIZE 16
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];
volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count;
```

Every `RECOMP_ICALL` macro writes to this buffer before dispatching. When the game crashes, inspect the ring buffer in the debugger:

```
g_icall_trace[0]  = 0x00145670   ; RwTextureRead (valid)
g_icall_trace[1]  = 0x00012340   ; sub_00012340 (valid)
g_icall_trace[2]  = 0x1C45BA68   ; GARBAGE - this is the problem
g_icall_trace[3]  = 0x00156F00   ; (this was before the garbage call)
```

The garbage VA tells you what the code *tried* to call. The preceding valid VAs tell you the execution context -- what was happening right before the crash.

## ICALL Fail Logging

For more detailed diagnostics, the runtime logs every failed indirect call:

```c
void recomp_icall_fail_log(uint32_t va) {
    void *caller = _ReturnAddress();  // MSVC intrinsic
    fprintf(stderr, "ICALL FAIL: VA=0x%08X caller=%p\n", va, caller);
}
```

The caller address is a native Windows address in the compiled binary. Map it to a function name using the linker's `.map` file:

1. Build with `/MAP` flag: `cmake --build build --config Release -- /p:MapFile=true`
2. Find the `.map` file in the build output
3. Search for the caller address to find which recompiled function triggered the failed ICALL

Example:
```
ICALL FAIL: VA=0x1C45BA68 caller=0x00007FF6A1234567

# In burnout3.map:
0x00007FF6A1234500  sub_001B4170
```

This tells you that `sub_001B4170` tried to call VA `0x1C45BA68`, which is clearly garbage (valid Xbox code lives in `0x00011000-0x002CC000`).

## Memory Access Violations

After ICALL failures, memory access violations are the next most common crash.

### Diagnosis

The crash address tells you what happened:

| Crash Address Pattern | Likely Cause |
|----------------------|--------------|
| `0x00000000` - `0x0000FFFF` | NULL pointer dereference |
| `0x00010000` - `0x00780000` | Valid Xbox VA range -- probably an uninitialized global |
| `0x00780000` - `0x00F80000` | Stack access -- stack corruption or overflow |
| `0x00F80000` - `0x04000000` | Heap access -- allocation failed or pointer stale |
| `0x76XXXXXX` | BSS address that failed to map (Windows DLL conflict) |
| `0x80000000`+ | Xbox kernel address -- code expecting kernel memory |
| `0xFD000000`+ | NV2A GPU register read -- need VEH handler |
| `0xCDCDCDCD` | MSVC debug fill -- uninitialized heap memory |
| `0xDDDDDDDD` | MSVC debug fill -- freed heap memory |

### Common Fixes

**NULL pointer from uninitialized global:**
The game reads a global that should have been set during initialization but the initialization function was stubbed or failed silently. Solution: trace back through the call graph to find where the global should be written, and fix that initialization.

**BSS address conflicts:**
Xbox BSS addresses like `0x76657240` collide with Windows system DLLs loaded at similar addresses. The `CreateFileMapping` approach handles most of these, but some addresses remain unmappable. Solution: accept the mirror failure and add a fallback path.

**NV2A GPU register access:**
The VEH handler (described in Step 5) catches these and maps a zero page. If you're still getting crashes at `0xFD000000+`, ensure the VEH is registered before any recompiled code runs.

## Vtable Corruption

### The Pattern

Xbox games use C++ objects with virtual method tables (vtables). When a vtable pointer is corrupted, the indirect call dispatches to a random address:

```c
// Original code equivalent:
RwObject *obj = get_object();       // obj->vtable is garbage
obj->vtable[3]();                   // calls 0x1C45BA68 or similar nonsense
```

### Why Vtables Get Corrupted

1. **Object not constructed**: the memory for the object was allocated but the constructor never ran (because we stubbed the allocator or the initialization sequence was wrong)
2. **Object already destroyed**: use-after-free; the destructor zeroed or freed the vtable
3. **Wrong object type**: a pointer cast is wrong, and the "vtable" is actually data from a different structure
4. **Memory layout mismatch**: our memory mapping put something different at the address where the object should live

### Per-Function Vtable Guards

When you identify a function that crashes on vtable dispatch, add a guard:

```c
// Before (generated code):
eax = MEM32(ecx);           // load vtable
PUSH32(esp, 0xDEAD);
RECOMP_ICALL(MEM32(eax + 0x10));  // call vtable[4] -- crashes

// After (patched):
eax = MEM32(ecx);
if (eax < 0x00011000 || eax > 0x003B2360) {
    // Vtable pointer is outside valid .text/.rdata range -- skip the call
    goto loc_safe_skip;
}
PUSH32(esp, 0xDEAD);
RECOMP_ICALL(MEM32(eax + 0x10));
```

### Centralized Early-Out

Rather than guarding every individual ICALL site, the `RECOMP_ICALL` macro includes a range check:

```c
if (_va >= 0x00400000 && _va < 0xFE000000) {
    g_esp += 4;   // pop dummy return address
    eax = 0;      // return "failure"
    break;         // skip the call
}
```

This catches all garbage VAs in one place. The ranges are:
- `0x00011000` - `0x003B2354`: valid code + data (allow)
- `0x00400000` - `0xFDFFFFFF`: nothing should be here (block)
- `0xFE000000` - `0xFE000600`: kernel thunk synthetic VAs (allow)

For Burnout 3, this centralized guard reduced ICALL volume by 33% (from 180/2s to 121/2s) and completely eliminated the most frequent garbage VA (0x1C45BA68).

## Stack Leaks

### The Problem

When `RECOMP_ICALL` fails (target function not found), the game code has already pushed arguments and a dummy return address onto the simulated stack. If the macro just returns without cleaning up, those bytes remain on the stack permanently.

Over many frames, this causes the stack pointer to drift downward until it underflows the stack region and crashes.

### The Fix

The `RECOMP_ICALL` macro pops the dummy return address on failure:

```c
else { g_esp += 4; eax = 0; }  // pop dummy ret addr
```

For stdcall functions where the callee is responsible for popping arguments, use `RECOMP_ICALL_SAFE` which restores `g_esp` to its pre-argument value:

```c
uint32_t saved_esp = esp;
PUSH32(esp, arg1);
PUSH32(esp, arg2);
PUSH32(esp, 0xDEAD);
RECOMP_ICALL_SAFE(target_va, saved_esp);
```

### Detection

Monitor `g_esp` over time. In a healthy run, it oscillates within a narrow range (the stack grows and shrinks around each function call). If `g_esp` trends steadily downward, you have a stack leak. Log `g_esp` every N frames to spot the trend.

## Division by Zero

### The Problem

Some Xbox code intentionally divides by zero, relying on SEH to catch the exception and skip to a recovery path:

```asm
__try {
    result = value / divisor;    ; divisor might be 0
} __except {
    result = 0;                  ; recover gracefully
}
```

### Why VEH Doesn't Work Here

An early approach used a Vectored Exception Handler to catch `EXCEPTION_INT_DIVIDE_BY_ZERO`. This fails because:

1. VEH runs *before* SEH. If the game has its own SEH handler (it does -- Xbox games use `__SEH_prolog`), VEH intercepts the exception before the game's handler sees it.
2. Deciding what to do in VEH is hard -- you'd need to know the game's intended recovery address, which is in the SEH handler's scope table.

### The Fix

Let the exception propagate naturally. The translated code's SEH prolog/epilog emulation sets up exception handlers that the C runtime's SEH mechanism can use. If the division by zero is in a `__try` block, the C runtime will find the handler.

If the exception is NOT in a `__try` block, it's a real bug (the original code didn't expect a zero divisor), and the crash is legitimate.

## State Machine Tracing

Xbox games are complex state machines. Understanding the boot sequence is essential for debugging initialization failures.

### Adding State Traces

Identify the main state variable (often a global integer) and log transitions:

```c
// In the frame pump override:
static int last_state = -1;
int state = MEM32(0x004D53B0);  // main game state variable
if (state != last_state) {
    fprintf(stderr, "STATE: %d -> %d (frame %llu)\n",
            last_state, state, g_icall_count);
    last_state = state;
}
```

For Burnout 3, the state machine progression is:
```
STATE: 0 -> 1 (init)
STATE: 1 -> 2 (alloc memory)
STATE: 2 -> 3 (load Global.txd + locale files)
STATE: 3 -> 4 (process loaded resources)
STATE: 4 -> 5 (load vehicle list)
...
STATE: 12 -> 13 (load track)
STATE: 13 -> 4 (gameplay loop)
```

When the game gets stuck in a state, you know exactly which loading step failed. Check what that state's code does and which files/resources it expects.

### Key Global Variables to Monitor

Track globals that change during initialization:

```c
fprintf(stderr, "DIAG: state=%d rwWorld=%08X carObj=%08X heapCursor=%08X\n",
    MEM32(0x004D53B0),    // game state
    MEM32(0x004D5370),    // RW world pointer
    MEM32(0x00557880),    // car object pointer
    heap_cursor            // heap allocation progress
);
```

## .rdata Corruption

### The Problem

The game's .rdata section is supposed to be read-only, but some Xbox code writes to .rdata addresses at runtime. On the real Xbox, the memory protection is permissive. On Windows, if we set .rdata as `PAGE_READONLY`, the writes crash. If we leave it writable, the writes succeed but corrupt string data that other code reads later.

### The Symptom

String data that was correct during initialization becomes garbled during gameplay. Function names, file paths, and debug messages turn into garbage. This causes file loading to fail (wrong filenames), resource lookup to fail (wrong keys), and debug output to be unreadable.

### The Fix

Keep a copy of the original XBE file data in memory. When code needs to read a string from .rdata, read it from the original XBE data instead of the mapped memory:

```c
// Instead of:
const char *name = (const char *)XBOX_PTR(string_va);

// Use:
const char *name = (const char *)(g_xbe_data + (string_va - 0x0036B7C0) + 0x0035C000);
```

The calculation maps the Xbox .rdata VA back to its file offset in the original XBE. The original data is never modified.

This is only needed for .rdata reads that are sensitive to corruption. Most code can tolerate the mapped memory; only resource loading paths need the fallback.

## Common Crash Patterns and Fixes

### Game Hangs (Infinite Loop)

**Symptom**: the window goes unresponsive, CPU spins at 100%.

**Cause**: NV2A push buffer functions that spin-wait on GPU registers. The GPU registers are zero, so the wait condition never clears.

**Fix**: stub the function. Find it in the recompiled code and wrap it in `#if 0`:
```c
#if 0
void sub_001CFDD0(void) {  // NtFlushBuffersFile or push buffer wait
    ...
}
#endif
```
Then add a stub in `recomp_manual.c` that returns immediately.

### Access Violation at 0x00000000

**Symptom**: crash reading or writing address 0.

**Cause**: a global pointer that should have been initialized during startup. Common offenders: RenderWare world pointer, physics world pointer, scene graph root.

**Fix**: trace back. Find which function was supposed to initialize the pointer. Check if that function ran (add a log). If it ran but the pointer is still NULL, the initialization logic itself is broken (probably depends on another NULL pointer). Fix the dependencies.

### Stack Overflow

**Symptom**: access violation at an address far below the stack base.

**Cause**: usually recursive `RECOMP_ICALL` dispatch -- function A calls function B calls function A via indirect calls, and the dispatch lookup burns through the native Windows stack (not the simulated Xbox stack).

**Fix**: increase the native stack size in the linker settings, or break the recursion by stubbing one of the functions in the cycle.

### Crash After Hours of Gameplay

**Symptom**: game runs fine for 30+ minutes, then crashes.

**Cause**: stack leak from failed ICALLs. Each failed indirect call leaks 4-16 bytes of simulated stack space. Over thousands of frames, the stack underflows.

**Fix**: ensure all ICALL failure paths clean up the stack. Use `RECOMP_ICALL_SAFE` for stdcall callsites. Monitor `g_esp` trend.

## Debugging Workflow

1. **Run the game**. It crashes.
2. **Check the ICALL trace ring buffer**. Was the crash an indirect call to a garbage address?
   - Yes: add a vtable guard or identify why the vtable is corrupted
   - No: check the crash address against the table above
3. **Find the calling function**. Use `_ReturnAddress()` or the debugger's call stack, cross-reference with the `.map` file to find the Xbox VA.
4. **Read the generated code**. Look at the recompiled C for that function. What is it trying to do?
5. **Check prerequisites**. Does the function read globals that should have been set earlier? Are those globals NULL or garbage?
6. **Apply the minimal fix**:
   - Stub the function (if it's non-essential)
   - Guard the bad call (if one specific ICALL is the problem)
   - Initialize the missing global (if the root cause is clear)
   - Override the function (if it needs a complete reimplementation)
7. **Rebuild and repeat**.

## Progress Tracking

Keep a log of every crash fix. Each entry should record:
- The symptom (crash address, exception type)
- The root cause (which function, which pointer, why)
- The fix applied
- What new behavior appeared after the fix

This log becomes invaluable as the project progresses. Patterns emerge -- you'll notice that 80% of crashes come from 5-6 root causes (uninitialized RW objects, corrupted vtables, missing kernel functions). Knowing the common patterns lets you fix new crashes in minutes instead of hours.

## The Payoff

Debugging is the hardest part of static recompilation, but it's also the most rewarding. The moment the game transitions from a black screen with crashes to showing actual gameplay -- loading real assets, rendering the world, responding to input -- makes every hour of crash diagnosis worthwhile. And once you reach that point, progress accelerates dramatically because most of the foundational infrastructure is working.
