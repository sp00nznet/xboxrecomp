# SEH and Exception Handling

Structured Exception Handling in statically recompiled Xbox code, and our own use of Vectored Exception Handlers.

## Background: SEH on Xbox

Xbox games are compiled with the MSVC compiler, which uses Windows Structured Exception Handling (SEH) for `__try`/`__except` blocks. The compiler emits calls to CRT helper functions:

- `__SEH_prolog` (VA 0x00244784): Sets up an exception frame on the stack, installs it in the TIB exception chain (fs:[0x00]), and initializes ebp for the calling function.
- `__SEH_epilog` (VA 0x00244797): Tears down the exception frame, restores the previous handler, and cleans up ebp.

In the original binary, these are standard CRT functions that manipulate the thread's exception handler chain. In recompiled code, they are translated just like any other function -- but with a critical twist.

## The ebp Bridge Problem

`__SEH_prolog` sets up the caller's frame pointer (ebp). On real x86:

```asm
; Caller:
push    ebp                 ; save old ebp
call    __SEH_prolog        ; prolog sets ebp = esp - N
; After return, ebp is set by prolog
mov     eax, [ebp+8]        ; access parameter via new ebp
```

In recompiled code, ebp is a local variable in each function (see [register-model.md](register-model.md)). When the caller calls `__SEH_prolog`, it is invoking a separate C function. That function's local `ebp` is independent from the caller's local `ebp`. The prolog cannot modify the caller's local variable.

### Solution: g_seh_ebp

A global bridge variable connects the two functions:

```c
extern uint32_t g_seh_ebp;
```

The flow:

```c
// In __SEH_prolog (sub_00244784):
void sub_00244784(void) {
    uint32_t ebp;
    // ... set up exception frame on Xbox stack ...
    ebp = esp - N;       // compute frame pointer
    g_seh_ebp = ebp;     // write to bridge
}

// In the caller (generated code):
void sub_001234AB(void) {
    uint32_t ebp;
    PUSH32(esp, ebp);           // push old ebp
    // push other SEH setup data...
    PUSH32(esp, 0xDEAD0000);    // dummy return address
    sub_00244784();              // call __SEH_prolog
    ebp = g_seh_ebp;            // READ BACK the computed ebp

    // ... function body using local ebp as frame pointer ...

    g_seh_ebp = ebp;            // write for epilog
    sub_00244797();              // call __SEH_epilog
    POP32(esp, ebp);            // restore old ebp
}
```

This bridge is the only way to communicate ebp values between functions in the recompiled code, because ebp is deliberately kept local to prevent corruption by FPO functions.

## Exception Frame on the Xbox Stack

`__SEH_prolog` installs an exception frame in the TIB exception chain:

```c
// Simplified __SEH_prolog implementation
void __SEH_prolog_impl(void) {
    // The exception frame is on the Xbox stack:
    // [esp+0] = previous exception handler (from TIB fs:[0])
    // [esp+4] = exception handler function pointer
    // [esp+8] = try level (-1 = no active __try block)

    // Read previous handler from fake TIB
    uint32_t prev = MEM32(0x00);  // fs:[0] → MEM32(0x00)

    // Write new frame
    MEM32(esp + 0) = prev;
    MEM32(esp + 4) = handler_va;
    MEM32(esp + 8) = 0xFFFFFFFF;  // try level: -1 (outside any __try)

    // Install in TIB
    MEM32(0x00) = esp;  // fs:[0] = new frame

    // Set up ebp for caller
    ebp = esp + frame_offset;
    g_seh_ebp = ebp;
}
```

In our recompiled code, the exception frames exist on the Xbox stack but are never actually used for exception dispatch (Windows does not know about our simulated stack). They exist for correctness of the stack layout and ebp computation.

## VEH: Our Own Exception Handling

We use Windows Vectored Exception Handlers (VEH) for our own purposes, separate from the game's SEH:

### NV2A Register Access

The NV2A GPU has memory-mapped registers at 0xFD000000+. Game code and XDK libraries read/write these addresses directly. These pages are not mapped by default, so access causes an access violation. The VEH allocates pages on demand:

```c
LONG WINAPI xbox_veh_handler(EXCEPTION_POINTERS *info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    uintptr_t fault_addr = info->ExceptionRecord->ExceptionInformation[1];

    // NV2A registers: 0xFD000000 - 0xFEFFFFFF
    if (fault_addr >= 0xFD000000 && fault_addr < 0xFF000000) {
        uintptr_t page = fault_addr & ~0xFFF;
        if (VirtualAlloc((LPVOID)page, 4096,
                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
            return EXCEPTION_CONTINUE_EXECUTION;  // retry the instruction
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;  // not our problem
}

// Install during init:
AddVectoredExceptionHandler(1, xbox_veh_handler);  // first handler
```

### DO NOT Handle Division by Zero

Early in development, we added DIV0 handling to the VEH:

```c
// WRONG - do not do this:
if (info->ExceptionRecord->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO) {
    // Skip the faulting instruction
    info->ContextRecord->Eip += instruction_length;
    info->ContextRecord->Eax = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
```

This interfered with the game's own SEH recovery. Several functions in the game intentionally divide by values that can be zero, relying on their `__except` handler to catch the fault and take an alternate code path. When our VEH intercepts the DIV0 first, the game's SEH handler never fires, and the function continues with a bogus result instead of taking the recovery path.

**Rule**: Only handle exceptions in VEH that cannot possibly be handled by the game's SEH (like NV2A register faults, which are entirely our invention).

## Corrupted Vtables and Missing SEH Protection

On the real Xbox, when the game calls through a corrupted vtable pointer, the access violation is caught by an SEH handler up the call chain, and execution continues at the `__except` block. The game was designed to tolerate these faults.

In recompiled code, these vtable calls go through RECOMP_ICALL, which:
1. Reads the vtable pointer from Xbox memory (may be garbage).
2. Reads the function pointer from the vtable (access violation if the vtable address is unmapped).
3. Calls the function (if the function address is garbage, dispatch fails).

Problem: Step 2 can crash in native code (reading from an unmapped address), and our VEH does not handle this because it is in the [0x400000, 0xFE000000) range where we do not allocate pages.

### Solutions

**Centralized ICALL early-out** (preferred): Check the VA range before dereferencing:

```c
if (_va >= 0x00400000 && _va < 0xFE000000) {
    g_esp += 4; eax = 0; break;  // garbage VA, skip
}
```

**Per-function guards** (for specific known-bad call sites):

```c
// Before a vtable ICALL in sub_001B4170:
uint32_t vtable = MEM32(esi);
uint32_t method = MEM32(vtable + 0x10);
if (method >= 0x00400000 && method < 0xFE000000) {
    goto skip_this_call;  // corrupted vtable, skip
}
PUSH32(esp, 0);
RECOMP_ICALL(method);
```

**SEH emulation** (not yet implemented): Wrap vtable calls in `__try`/`__except` blocks in the generated C code. This would be the most faithful approach but adds significant complexity and performance overhead.

## The Exception Handler Chain

The fake TIB at address 0x00 maintains an exception handler chain:

```
MEM32(0x00) = pointer to top-most exception frame
                  |
                  v
            [frame N]
            prev → [frame N-1]
                        prev → [frame N-2]
                                    prev → 0xFFFFFFFF (end of chain)
```

`__SEH_prolog` pushes new frames. `__SEH_epilog` pops them. The chain is maintained for stack layout correctness but is never actually walked for exception dispatch (we use VEH or ICALL guards instead).

## Summary

| Mechanism | Purpose | Status |
|-----------|---------|--------|
| g_seh_ebp bridge | Communicate ebp between __SEH_prolog and callers | Working |
| Fake TIB at VA 0x0 | Exception chain anchor, stack bounds, TLS | Working |
| VEH for NV2A pages | Allocate GPU register pages on demand | Working |
| VEH for DIV0 | DO NOT DO THIS -- interferes with game's SEH | Removed |
| ICALL range check | Skip garbage vtable pointers | Working (centralized + per-function) |
| Full SEH emulation | Wrap ICALL in __try/__except | Not implemented |

The key lesson: in a static recompilation, you inherit the game's exception handling strategy but cannot use the same mechanism (real SEH on a real x86 stack). You must find alternative approaches -- VEH for hardware faults, range checks for corrupted pointers, and bridge variables for inter-function communication.
