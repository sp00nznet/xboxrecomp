# Recovering a function split into false entries

The detector can split one function at an interior branch target. Translating
the pieces separately can lose a comparison before a conditional branch or
turn a local loop into recursive host calls. When independent analysis has
established the original owner, use `--coalesce-functions` before regenerating:

```sh
python -m tools.recomp game.xbe --all --coalesce-functions bounds.json
```

The file is a JSON array. This example describes synthetic code from the test:

```json
[
  {
    "start": "0x00010000",
    "end": "0x0001001b",
    "coalesce_starts": ["0x0001000d", "0x00010014", "0x00010018"]
  }
]
```

`end` is exclusive. `coalesce_starts` must list every current detected entry
strictly inside the owner, sorted without duplicates. The flag is repeatable;
files and entries apply in order to the updated function database. Keep actual
title-specific bounds beside that title's private analysis, outside this repo.

Recovery runs after static callback discovery and before runtime helper detection,
automatic CFG ownership and translation. It removes the named fragments and emits
the complete owner. Without the option, translation is unchanged.
Callback discovery repeats after each repair. Newly visible callbacks protect
existing entries; a callback into an already coalesced interior aborts the batch.

The operation rejects conflicting extents, independent entry evidence (prologue,
callers, `external_entry`, or detector-recorded entries, seeds and code pointers),
manual/wrapped/referenced project entries, calls to an interior entry from the
owner, and code that does not cover the requested extent. Traps (`int3`, `ud2`,
`hlt`) terminate the validation walk and the recovered generated path; later
blocks need another incoming edge. The Xbox debug-service sequence `int 0x2d;
int3` is the exception: the kernel skips that `int3`, so recovery preserves its
fallthrough. Only proven alignment no-ops, an exact validated jump-table byte
range, or those two combined around the same gap may close a decode gap.
Register-jump continuations are accepted only when the jumped register can be
tracked from an in-range immediate load, including simple register copies. A
local jump table using a sole base or index register can be read beyond the owned end,
bounded by the next detected function, if any, and the backed code section. This
validates supplied bounds; it does not infer them or prove that unknown indirect
callers cannot exist. Review those callers before supplying a recovery file.
Invalid input aborts the batch before generated output is written; rerun with
corrected inputs.

Regression checks need no game files:

```sh
python -m pytest tools/recomp/test_function_coalescence.py
```
