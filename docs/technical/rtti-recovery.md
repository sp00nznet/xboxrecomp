# MSVC RTTI Recovery

> Part of the [Documentation Index](../INDEX.md)

`py -3 -m tools.rtti <xbe>` — recovers C++ class names, vtables, virtual method
addresses and the inheritance graph from a title's MSVC RTTI, and emits the
method addresses as `tools.disasm --seed-functions` input.

## Why it is worth a pipeline stage

Two payoffs, and the second is the one that changes the output.

**Names.** Where RTTI is present it is the richest symbol source on the
platform, because unlike a string table it points at *code*: a class name
attached to a concrete vtable full of concrete function addresses.

**Function starts.** A vtable slot is *proof* of a function entry point. A
method that is only ever called virtually is invisible to both linear sweep and
call-target scanning — nothing in the image ever names its address except the
vtable. Seeding those addresses back into `tools.disasm` therefore finds
functions no amount of sweeping will.

On Half-Life 2:

| | |
|---|---|
| RTTI methods already found by disasm | 4,286 |
| **landed in unclaimed bytes — missed functions** | **7,992** |
| landed inside a detected function | 10 (adjustor thunks) |
| functions, without seeds → with seeds | **33,140 → 41,215 (+24%)** |

disasm additionally reported *"Realigned 19 seeded addresses the sweep stepped
over"* — RTTI corrects decode boundaries as well as adding functions.

## Run it before disasm

```bash
py -3 -m tools.rtti game/title.xbe -o build/rtti.json --seeds build/seeds.json
py -3 -m tools.disasm game/title.xbe --seed-functions build/seeds.json -o build/disasm
```

It reads the XBE directly and needs no analysis JSON, so it can be the first
step after `xbe_parser`.

## How it works

MSVC emits, for each polymorphic class, a `CompleteObjectLocator` and stores a
pointer to it at **`vtable[-1]`** — the dword immediately before the first
method. So vtable discovery is mechanical: find the COLs, find every word
pointing at one, and the vtable starts 4 bytes later. Walk forward while entries
are addresses in an executable section.

```
TypeDescriptor            { void *vfptr; void *spare; char name[]; }
CompleteObjectLocator     { u32 sig; u32 offset; u32 cdOffset;
                            TypeDescriptor *pTD; ClassHierarchyDescriptor *pCD; }
ClassHierarchyDescriptor  { u32 sig; u32 attributes; u32 numBaseClasses;
                            BaseClassDescriptor **pBaseClassArray; }
BaseClassDescriptor       { TypeDescriptor *pTD; u32 numContainedBases;
                            PMD where; u32 attributes; }
```

All fields are plain VAs. This is 32-bit MSVC, so none of the image-relative
offset indirection that x64 RTTI uses applies.

Only file-backed bytes are addressable: a section's `virtual_size` runs past
`raw_size` for BSS, and reading there walks off the buffer. `Image` bounds every
access by `raw_size` for that reason.

## Naming the generated C

`--names` writes a `{address: name}` map in the format
`tools/ghidra_naming/merge_names.py --apply` consumes, so the recompiled output
carries `CBaseEntity__000162C2` instead of `sub_000162C2`:

```bash
py -3 -m tools.rtti game/title.xbe --names build/rtti_names.json
py -3 tools/ghidra_naming/merge_names.py --apply     --names-json     build/rtti_names.json     --functions-json build/disasm/functions.json
```

RTTI carries class names, not member names, so the method's own name is not
recoverable. The address is kept to stay unique and the owning class is
prepended -- enough to read generated code and to make a crash stack mean
something.

**Which class owns a method that appears in several vtables** is well defined
and worth computing: the vtables share one implementation because they
*inherit* it, so the declaring class is the one that is an ancestor of every
other class in the set. That needs only the hierarchy sets, not layout
modelling. On Half-Life 2:

| | |
|---|---|
| in exactly one vtable | 8,992 |
| shared, resolved to a common ancestor | 2,528 |
| shared, no common ancestor (omitted) | 768 |
| **ambiguous** | **0** |

11,520 of 12,288 (94%) get an owner, and the rule never returns two candidates.
The 768 omissions are multiple inheritance and compiler-generated thunks shared
between unrelated classes; they stay `sub_*` rather than being guessed at.

## What it will not tell you

**Which ancestor declared a given slot.** The `BaseClassDescriptor` array is a
depth-first **preorder**, so trailing entries are secondary inheritance
branches, not the least-derived base — reading the last entry as the root gives
the wrong answer (on `CNPC_Alyx` it yields `CAI_ExpresserSink` instead of
`IHandleEntity`). With multiple inheritance the question needs real MSVC layout
modelling. `methods_by_class()` reports how many classes' vtables hold a method
instead; one is uniquely attributable, more than one is shared and inherited.

## Coverage across the corpus

RTTI is uncommon. Measured over the titles on hand:

| Title | Type descriptors | Usable |
|---|---|---|
| **Half-Life 2 (Xbox)** | **2,336** | **2,932 vtables, 12,288 methods** |
| Crimson Skies | 4 | no vtables — CRT `type_info` only |
| Xbox dashboard | 4 | no vtables — CRT `type_info` only |
| Blood Wake, Burnout 3, Wreckless, Ghost Recon | 0 | none |

So this is a capability that pays off enormously on C++ titles and does nothing
on C ones. **Absence is a normal result, not a failure**: the tool prints a note
and exits 0, and callers get empty dicts. `regen.sh` scripts can run it
unconditionally.

## API

```python
from tools.rtti import recover, seeds, methods_by_class, demangle

r = recover("game/title.xbe")
r["type_descriptors"]  # {va: ".?AVCFoo@@"}
r["vtables"]           # [(vtable_va, name, subobject_offset, [method VAs])]
r["hierarchy"]         # {name: [base names, MSVC preorder]}
r["primary_len"]       # {name: slot count of the offset-0 vtable}

seeds(r)               # sorted method addresses for --seed-functions
methods_by_class(r)    # {method_va: {class names holding it}}
demangle(".?AVCFoo@@") # "CFoo"
```

Per-title expected values belong in that title's repo, not here — see
`hl2/tools/rtti_check.py` for the pattern.
