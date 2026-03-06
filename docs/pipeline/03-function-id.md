# Step 3: Function Identification

## Why Identify Functions?

A typical Xbox game produces tens of thousands of recompiled functions. Burnout 3 has 22,095 functions in its .text section -- a mix of game code, C runtime (CRT), RenderWare engine, and XDK library code all baked together with no clear boundaries.

Without identification, you're staring at a flat list of `sub_XXXXXXXX` names with no idea what each one does. Identification assigns names, categories, and confidence scores, letting you:

- **Prioritize work**: focus on game-specific code, not reimplementing `memcpy`
- **Find bugs faster**: when `sub_00145670` crashes, knowing it's `RwTextureRead` immediately tells you the problem domain
- **Stub intelligently**: CRT math functions can be replaced with native Win32 calls; RenderWare initialization can be redirected to your custom renderer
- **Track coverage**: know what percentage of game vs. library code is working

## CRT Signature Matching

The Xbox XDK ships a C runtime (MSVC-based) that gets statically linked. These functions have well-known byte patterns because they're compiled from the same source with the same compiler version.

### Byte Pattern Database

Build a database of known CRT function byte sequences. The first 16-32 bytes of a function are usually sufficient for a unique match:

```python
CRT_SIGNATURES = {
    # memcpy: push ebp; mov ebp, esp; push edi; push esi; mov esi, [ebp+0C]...
    bytes.fromhex("558BEC57568B750C8B4D10"): "memcpy",
    # strlen: push ecx; mov ecx, [esp+8]; test cl, 3...
    bytes.fromhex("518B4C240885C974"): "strlen",
    # malloc: push ebp; mov ebp, esp; push ecx; cmp dword ptr [...]
    bytes.fromhex("558BEC51833D"): "_malloc",
    # free: push ebp; mov ebp, esp; push ecx...
    bytes.fromhex("558BEC518B4508"): "_free",
}
```

### XDK Version Sensitivity

CRT signatures vary by XDK version. A function compiled with XDK 5849 (Burnout 3's version) may have slightly different register allocation than the same function from XDK 5455. The library version stamps extracted in Step 1 tell you which signature database to use.

For broad coverage, maintain signature databases for common XDK versions: 4627, 5233, 5455, 5558, 5849, 5933.

### Match Process

For each function in the disassembly output:
1. Read the first 32 bytes of the function body
2. Compare against all signatures using prefix matching (not exact match, since code after the prologue may differ due to inlining)
3. If a match is found with sufficient uniqueness (no collisions), assign the CRT name
4. Record the match length and confidence

Typical CRT identification coverage: 150-250 functions out of 22K, but these are heavily referenced (memcpy alone may have 2,000+ call sites).

## RenderWare Identification

Burnout 3 uses Criterion's custom fork of RenderWare (~3.7). RenderWare functions can be identified through:

### String References

RenderWare code is rich with debug strings. When a function references a string like `"RwEngineInit"` or `"RpWorldCreate"`, the function is almost certainly the named RW function or closely related:

```python
for func in functions:
    for xref in func.data_xrefs:
        string = strings.get(xref.target)
        if string and string.startswith(("Rw", "Rp", "Rt", "Rs")):
            func.rw_name = string
            func.category = "renderware"
```

Common RenderWare string prefixes:
- `Rw` -- core engine (RwEngine, RwCamera, RwTexture, RwStream)
- `Rp` -- plugins (RpWorld, RpAtomic, RpGeometry, RpLight)
- `Rt` -- toolkits (RtBMP, RtPNG, RtAnim)
- `Rs` -- schemes (RsEventHandler)

### Address Zone Heuristics

In MSVC-linked executables, functions from the same source file tend to be placed contiguously. If you identify a cluster of RenderWare functions at addresses 0x00140000-0x00170000, other unidentified functions in that range are likely also RenderWare.

This is a soft heuristic -- use it to increase confidence on functions that have partial indicators, not to blindly label everything in a range.

### Known Structure Sizes

RenderWare objects have known sizes and field layouts from public documentation and open-source RW 3.3 headers. When a function allocates a specific size or accesses specific field offsets, it narrows the possibilities:

```
RwCamera: allocates 0x60 bytes, field at +0x00 = type, +0x40 = frame
RpWorld:  allocates 0xC0+ bytes, field at +0x68 = numTriangles
RwTexture: field at +0x04 = raster, +0x08 = dictionary
```

## Vtable Scanning

C++ classes with virtual methods have vtable pointers in .rdata. A vtable is a contiguous array of function pointers:

```
.rdata:0x003B1200:  0x00145670  ; vtable[0] = destructor
.rdata:0x003B1204:  0x00145700  ; vtable[1] = method1
.rdata:0x003B1208:  0x00145780  ; vtable[2] = method2
.rdata:0x003B120C:  0x001457E0  ; vtable[3] = method3
```

### Detecting Vtables

Scan .rdata for arrays of values that are all valid .text addresses:

```python
for addr in range(rdata_start, rdata_end, 4):
    entries = []
    scan = addr
    while is_text_address(MEM32(scan)):
        entries.append(MEM32(scan))
        scan += 4
    if len(entries) >= 2:
        vtables.append({"address": addr, "entries": entries})
```

### Vtable-to-Class Mapping

Once vtables are found, look for constructor functions that write the vtable address:

```asm
mov dword ptr [ecx], 0x003B1200   ; store vtable pointer at this->vptr
```

The constructor function and the vtable address together identify a class. Cross-referencing the vtable entries reveals all virtual methods of that class.

In Burnout 3, vtable-based identification is particularly important for RenderWare plugin classes and game-specific entity types.

## Stub and Thunk Detection

Many functions are trivial wrappers:

### Direct Thunks
```asm
jmp sub_00012340    ; single instruction: tail call
```

### Getter Stubs
```asm
mov eax, [ecx+0x10] ; return this->field
ret
```

### Trivial Returns
```asm
xor eax, eax        ; return 0
ret
```

Detecting these lets you collapse the call graph. If `sub_A` is just `jmp sub_B`, then every caller of `sub_A` is really calling `sub_B`. This reduces the number of functions you need to analyze by 10-15%.

## Label Propagation

After direct identification (CRT signatures, string references, vtables), propagate labels through the call graph:

1. **Known callers**: if a function's only caller is `RwTextureRead`, and it allocates a `RwTexture`-sized block, it's likely `RwTextureCreate`.
2. **Known callees**: if a function calls `memcpy`, `malloc`, and `NtCreateFile`, it's likely a file loading function.
3. **Cluster proximity**: functions at adjacent addresses that call each other are likely from the same source module.
4. **Parameter flow**: if function A passes a specific global address to function B, and that global is known to be an RwEngine structure, function B is likely an RwEngine method.

### Propagation Algorithm

```python
changed = True
while changed:
    changed = False
    for func in unidentified_functions:
        # Score based on neighbors in the call graph
        caller_votes = Counter()
        for caller in func.callers:
            if caller.category:
                caller_votes[caller.category] += 1

        callee_votes = Counter()
        for callee in func.callees:
            if callee.category:
                callee_votes[callee.category] += 1

        # Combine votes
        combined = caller_votes + callee_votes
        if combined.most_common(1):
            category, count = combined.most_common(1)[0]
            if count >= 3:  # confidence threshold
                func.category = category
                changed = True
```

## Confidence Scoring

Each identification gets a confidence score from 0.0 to 1.0 based on the evidence:

| Evidence | Confidence |
|----------|------------|
| Exact byte signature match (32+ bytes) | 0.95 |
| String reference matches function name | 0.90 |
| Vtable entry with known class | 0.80 |
| Multiple consistent call graph signals | 0.70 |
| Single call graph propagation | 0.50 |
| Address zone heuristic only | 0.30 |

Functions below 0.50 confidence are marked as "tentative" and should be manually verified if they become relevant during debugging.

## Output: identified_functions.json

```json
{
  "functions": [
    {
      "address": "0x00011000",
      "name": "memcpy",
      "category": "crt",
      "confidence": 0.95,
      "method": "byte_signature",
      "size": 74
    },
    {
      "address": "0x00145670",
      "name": "RwTextureRead",
      "category": "renderware",
      "confidence": 0.90,
      "method": "string_reference",
      "size": 312
    },
    {
      "address": "0x000636D0",
      "name": null,
      "category": "game",
      "confidence": 0.60,
      "method": "call_graph",
      "size": 1580,
      "notes": "Physics force application. Called from main loop."
    },
    ...
  ],
  "statistics": {
    "total": 22095,
    "identified": 4200,
    "crt": 187,
    "renderware": 3100,
    "xdk": 420,
    "game": 493,
    "unidentified": 17895
  }
}
```

## Invocation

```bash
py -3 -m tools.func_id "path/to/default.xbe" -v
```

Options:
- `-v` / `--verbose`: print progress and per-function decisions
- `--sig-db <path>`: path to custom signature database
- `--xdk-version <ver>`: override XDK version detection (uses embedded library versions by default)
- `--output <path>`: output file path (default: `identified_functions.json`)
- `--min-confidence <float>`: minimum confidence to include in output (default: 0.3)

## Practical Tips

1. **Don't aim for 100%**: identifying 20-30% of functions is enough to make the project manageable. The remaining 70% will be identified organically during debugging.

2. **Focus on what crashes**: when the recompiled game crashes in `sub_00145670`, that's when you identify it. Just-in-time identification is more efficient than trying to name everything upfront.

3. **Build a function map file**: the MSVC linker can produce a `.map` file for the recompiled binary, mapping native addresses to function names. Cross-reference this with the Xbox VA dispatch table to quickly locate crash sites.

4. **Track categories, not names**: knowing a function is "RenderWare / texture loading" is more useful than its exact RW API name. Category tells you which subsystem to look at; the exact name is a bonus.
