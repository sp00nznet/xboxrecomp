# Step 2: Disassembly and Function Detection

## Why a Custom Disassembler?

Tools like IDA Pro and Ghidra are excellent for interactive reverse engineering, but they are not designed for pipeline automation. The static recompiler needs structured, machine-readable output -- JSON files containing every function boundary, every cross-reference, and every string location. This output feeds directly into the lifter (Step 4) which translates x86 instructions to C code.

A custom disassembler also lets us tune heuristics specifically for Xbox executables. We know the compiler (MSVC from the XDK), the calling conventions (cdecl, stdcall, thiscall), and the optimization settings (LTCG for library code, standard for game code). These constraints dramatically improve function detection accuracy.

## Disassembly Engine: Capstone

The disassembler uses [Capstone](https://www.capstone-engine.org/), a lightweight multi-architecture disassembly framework. For Xbox recompilation, we use Capstone in x86-32 mode:

```python
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True  # Enable operand details

for insn in md.disasm(code_bytes, start_va):
    print(f"0x{insn.address:08X}  {insn.mnemonic}  {insn.op_str}")
```

Capstone gives us:
- Instruction mnemonic and operands
- Operand types (register, immediate, memory)
- Memory operand decomposition (base + index*scale + displacement)
- Instruction groups (call, jump, return, interrupt)

## Disassembly Strategies

### Linear Sweep

The simplest approach: start at the beginning of .text and decode every byte sequentially. This works because MSVC-compiled code is densely packed with no gaps between functions (the linker fills alignment padding with `int 3` / `0xCC` bytes).

```
Linear sweep:
  0x00011000: push ebp           -- start of function
  0x00011001: mov ebp, esp
  0x00011003: sub esp, 0x10
  ...
  0x0001104A: ret
  0x0001104B: int3               -- padding
  0x0001104C: int3
  0x0001104D: int3
  0x0001104E: int3
  0x0001104F: int3
  0x00011050: push ebp           -- next function
```

**Advantage**: Simple, covers all code.
**Problem**: Data embedded in code (jump tables, constants) will be misinterpreted as instructions, and the resulting garbage can desynchronize the decoder for subsequent real instructions.

### Recursive Descent

Start at known entry points (the XBE entry point, kernel thunk targets) and follow the control flow graph:

1. Decode the instruction at the current address
2. If it's a `call`, add the target to the work queue
3. If it's a conditional branch, add both the taken and not-taken paths
4. If it's an unconditional jump, follow only the target
5. If it's a `ret` or `int 3`, stop this path
6. Continue until the work queue is empty

```python
work_queue = {entry_point}
visited = set()

while work_queue:
    addr = work_queue.pop()
    if addr in visited or not in_text_section(addr):
        continue
    visited.add(addr)

    insn = decode(addr)
    if insn.is_call():
        work_queue.add(insn.target)
        work_queue.add(addr + insn.size)
    elif insn.is_cond_branch():
        work_queue.add(insn.target)
        work_queue.add(addr + insn.size)
    elif insn.is_uncond_jump():
        work_queue.add(insn.target)
    elif insn.is_ret():
        pass  # end of path
    else:
        work_queue.add(addr + insn.size)
```

**Advantage**: Only visits reachable code, never misinterprets data as code.
**Problem**: Misses unreachable functions (dead code, functions only called via vtables or function pointers).

### Combined Approach

The production disassembler uses both:

1. Recursive descent from the entry point to find all reachable code
2. Linear sweep to find functions missed by recursive descent
3. Cross-validation: if linear sweep finds a valid-looking function prologue that recursive descent missed, add it

This combined approach achieves near-complete coverage. For Burnout 3, it identifies 22,095 functions across the 2.73 MB .text section.

## Function Detection Strategies

Finding where functions start and end is harder than it sounds. The disassembler uses multiple heuristics:

### Call Target Analysis

Every `call` instruction (opcode `0xE8` for near calls) provides a direct function address. This is the most reliable method. For Burnout 3, call target analysis alone finds roughly 15,000 functions.

```python
for insn in all_instructions:
    if insn.mnemonic == 'call' and insn.operand_type == IMMEDIATE:
        functions.add(insn.target)
```

### Prologue Pattern Matching

MSVC generates predictable function prologues:

**Standard frame:**
```asm
push ebp
mov ebp, esp
sub esp, <frame_size>
```

**FPO (Frame Pointer Omission):**
```asm
sub esp, <frame_size>
push ebx            ; callee-saved registers
push esi
push edi
```

**SEH (Structured Exception Handling):**
```asm
push <handler_offset>
call __SEH_prolog    ; 0x00244784 in Burnout 3
```

**Naked/leaf functions:**
```asm
mov eax, [esp+4]     ; no prologue at all, just starts working
```

The disassembler scans for these patterns at addresses not already identified as mid-function. Prologue matching catches functions that are never called directly (only through vtables or function pointers).

### Jump Table Analysis

Switch statements compiled by MSVC produce jump tables -- arrays of addresses in .rdata that an indirect jump uses:

```asm
cmp eax, 15              ; switch variable range check
ja default_case
jmp [eax*4 + 0x0036C000] ; jump table in .rdata
```

The jump table entries point to case handlers within the same function. These addresses must NOT be treated as function starts. The disassembler detects jump table patterns and marks the target addresses as intra-function labels rather than function entries.

For the lifter (Step 4), jump tables are converted to C `switch` statements. The disassembler records the table address, entry count, and all target addresses.

### Cross-Reference Building

Cross-references (xrefs) track every place a given address is referenced -- called, jumped to, or used as data. For Burnout 3, the disassembler builds 163,787 cross-references.

Xrefs are categorized:
- **Code→Code call**: `call 0x001234` at address X (X calls the function at 0x001234)
- **Code→Code jump**: `jmp 0x001234` (control flow within or between functions)
- **Code→Data read**: `mov eax, [0x004D5370]` (reads from a global variable)
- **Code→Data write**: `mov [0x004D5370], eax` (writes to a global variable)
- **Data→Code**: a function pointer stored in .rdata (vtable entry, callback)
- **Data→Data**: a pointer stored in a data section (linked list, pointer array)

Xrefs are essential for function identification (Step 3) and debugging (Step 6). When you see a crash at address X, the xref database tells you every function that calls X and every global variable X touches.

## Challenges

### Variable-Length Instructions

x86 instructions range from 1 to 15 bytes. If the disassembler starts decoding at the wrong offset (even by 1 byte), it will produce a completely different and wrong instruction stream. This is why data embedded in code is so dangerous -- it throws off the alignment.

MSVC mitigates this by aligning functions to 16-byte boundaries and filling gaps with `int 3` (0xCC), which is a single-byte instruction. The disassembler uses 0xCC bytes as synchronization points.

### Statically Linked Libraries

The .text section contains not just game code but also:
- C Runtime (CRT): ~200 functions (memcpy, malloc, printf, etc.)
- D3D8 LTCG: Direct3D 8 wrapper compiled with Link-Time Code Generation
- RenderWare 3.7: Criterion's custom engine fork (~3,000+ functions)
- XDK utilities: XMV decoder, DSOUND, XONLINE, etc.

All of this code is baked into the .text section with no clear boundaries. The function identification step (Step 3) addresses categorizing these, but the disassembler must first correctly detect all function boundaries regardless of origin.

### LTCG (Link-Time Code Generation)

Some XDK libraries are compiled with LTCG, which allows the compiler to inline and reorganize code across translation unit boundaries. This produces:
- Functions that don't follow standard prologue patterns
- Code interleaved from different source files
- Aggressive register allocation that doesn't match standard calling conventions

LTCG code requires more aggressive heuristics -- relying on call targets and return instructions rather than prologue patterns.

### Computed Jumps

Beyond switch statement jump tables, some code uses computed jumps for optimization:

```asm
lea eax, [func_table]
add eax, ecx
jmp eax
```

These must be analyzed to determine all possible targets. If the range is unknown, the disassembler logs a warning and the lifter may need a manual override.

## Output Files

The disassembler produces four JSON files:

### functions.json

```json
{
  "functions": [
    {
      "address": "0x00011000",
      "size": 74,
      "end": "0x0001104A",
      "type": "standard",
      "prologue": "push_ebp",
      "calls": ["0x00012340", "0x000156F0"],
      "callers": ["0x001D2807"],
      "has_jump_table": false,
      "num_basic_blocks": 5
    },
    ...
  ],
  "count": 22095
}
```

### labels.json

Labels are intra-function branch targets -- addresses that are jumped to but are not function entries:

```json
{
  "labels": {
    "0x00011020": {"function": "0x00011000", "type": "branch_target"},
    "0x00011035": {"function": "0x00011000", "type": "jump_table_case"},
    ...
  }
}
```

### xrefs.json

```json
{
  "xrefs": [
    {"from": "0x001D2810", "to": "0x00011000", "type": "call"},
    {"from": "0x00015600", "to": "0x004D5370", "type": "data_read"},
    ...
  ],
  "count": 163787
}
```

### strings.json

All ASCII and Unicode strings found in .rdata, with their cross-references:

```json
{
  "strings": [
    {
      "address": "0x0036C100",
      "value": "RwEngineInit",
      "encoding": "ascii",
      "xrefs": ["0x00145670", "0x00145690"]
    },
    ...
  ]
}
```

## Invocation

```bash
py -3 -m tools.disasm "path/to/default.xbe" --text-only
```

The `--text-only` flag restricts disassembly to the .text section (game code). Without it, the tool also processes the named library sections (XMV, DSOUND, etc.), which adds time but increases coverage for games that call library code via indirect jumps.

Additional options:
- `--output-dir <dir>`: where to write JSON files (default: `output/`)
- `--verbose`: print progress and statistics
- `--dump-asm`: also write a human-readable assembly listing (.asm file)

## Verification

After disassembly, sanity-check the results:

1. **Function count**: should be in the thousands for a large game. Burnout 3 has 22,095 functions in 2.73 MB of code -- roughly one function per 130 bytes on average.
2. **Coverage**: the total bytes covered by identified functions should be close to the section size. Gaps are expected (alignment padding, embedded data), but large unexplained gaps suggest missed functions.
3. **Entry point**: the decoded entry point from Step 1 should appear as a function in the output.
4. **No overlaps**: no two functions should claim the same address range. If they do, the function boundary detection has a bug.
