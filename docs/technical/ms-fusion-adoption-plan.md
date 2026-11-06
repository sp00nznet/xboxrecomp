# Adopting the Crimson Skies Findings — Plan and Status

> Part of the [Documentation Index](../INDEX.md)

Working plan for folding everything learned from Microsoft's own recompiler into
xboxrecomp. Source analysis: [ms-fusion-recompiler.md](ms-fusion-recompiler.md)
and [ms-fusion-codegen-teardown.md](ms-fusion-codegen-teardown.md).

Ordered by (value / risk), not by how impressive it sounds. Every item states
what it is, why it is worth doing, and what would make it wrong. Items that were
investigated and rejected are kept here with the evidence, because "we looked and
it is not the problem" is a result worth not re-deriving.

Status legend: **done** · **in progress** · **planned** · **rejected (evidence)**

---

## Landed

### 1. Push the real guest return address — **done** (`7fbcc79`)

Both Microsoft translators write the true guest return address even though
neither uses it for control transfer, because guest code reads it. We pushed a
literal `0`. `ret` never reads the slot, so it was invisible from the return
side, but `__SEH_prolog` locates its scope table through `[esp]`, `_alloca`
probes walk back to it, and `mov eax, [esp]` is the standard CRT "who called me".

Also fixed a coupled ordering bug in `_fixup_icall_esp_save` that the change
would otherwise have broken silently.

Guard: `tools/recomp/test_call_retaddr.py`.

### 2. Indirect-branch target feedback — **done** (`e4e95a7`, `10277e9`, `19a2ec8`, `d06af63`)

Local equivalent of `VirtualDispatchTraceFiles` + `UpdateEnlightenments: true`.
Record what the title actually branches to, merge into a cumulative database,
feed back as function-detection seeds.

Measured on Halo 2276 over two rounds: 67 → 71 distinct targets observed,
unresolved-in-run 25 → 5, gaps that were not known function starts 24 → 4. The
pre-existing diagnostic had reported 3 of those 24 — it rate-limits at 16.

Also produced the **alignment finding**: seeding the raw measured set made the
title crash *earlier*, and the 4 offenders were the only unaligned ones. Seeds
are now filtered (`seeds --align 16`) while the database keeps everything.

Guard: `tools/recomp/test_icall_feedback.py`.

### 3. Decode direct call targets the sweep stepped over — **done** (`fd71a0e`)

Not on the original list; found while validating item 4 below. `linear_sweep`
decodes each section as one stream and resyncs only by skipping a byte, so it
comes out of phase wherever it crosses data. `_pass_call_targets` then dropped
any target the sweep had stepped over — discarding the strongest evidence a
function start can have.

Halo 2276: 34 call targets realigned, functions 8,869 → 8,903, and 31 of the 46
unresolved-stub addresses became real functions.

Guard: `tools/disasm/test_decode_at.py`.

---

## Next

### 4. Flat, directly-indexed dispatch — **in progress**

Microsoft resolves an indirect branch with `jmp qword ptr [r9 + r8*8]`: one
indexed load, no compare, no miss path. We do up to three calls, one of which is
a **binary search over ~8,900 entries** (`_write_dispatch_table` in
`translator.py`), on every indirect call:

```c
recomp_func_t _fn = recomp_lookup_manual(_va);   /* linear/dict scan */
if (!_fn) _fn = recomp_lookup(_va);              /* ~13-iteration bsearch */
if (!_fn) _fn = recomp_lookup_kernel(_va);       /* kernel thunk range  */
```

Replace with one bounds check and one load from a flat array indexed by
`va - code_base`, populated once at init by walking the existing tables in
priority order. The generated `recomp_dispatch.c` does not need to change shape —
it becomes the *builder* rather than the lookup.

Cost: 8 bytes per byte of guest code span. Halo's span is `0x12000..0x253063`,
so ~19 MB. Microsoft spends 20 MB on the same trade without comment.

Why it is safe: priority order is baked in at init instead of being re-evaluated
per call, so behaviour is identical by construction. What would make it wrong: a
title whose code span is large enough that the table is not worth the RAM, or
manual overrides registered *after* init — both detectable at build time.

### 5. Keep the sweep in phase at source — **planned**

Item 3 patches the symptom at call targets. The cause is that `linear_sweep`
never restarts from a known-good boundary. Sweeping forward from every known
function start (entry point, exports, prologue matches) would keep the stream in
phase instead of relying on downstream repair. Higher risk because it changes the
primary decode pass; wants an A/B against the current function count.

### 7. Per-title configuration file — **planned**

Microsoft has `xefu.lua` / `xefu.ctrl.json` per title; we have
`seed_functions.json`, `icall_seeds.json`, `icall_targets.json`,
`manual_functions.json`, `trace_functions.json` and `cachebeta_analysis.json`
threaded through `regen.sh` as six separate flags. One per-title file naming the
rest would make `regen.sh` a two-line script and make a new title's setup
copyable. Pure ergonomics, no behaviour change.

---

## Investigated and rejected

### Block-granular entry points — **rejected for now (evidence)**

This was going to be the headline change: emit an entry-dispatch prologue so any
basic block can be entered directly, matching Microsoft's byte-granular address
map. Checked the premise against the data first:

| gap set | mid-function | in a gap between functions |
|---|---|---|
| 46 unresolved direct-call stubs | **0** | 46 (all 16-aligned) |
| 25 unresolved indirect targets | **1** (at offset +0 of a known function) | 24 |

It would have fixed approximately one address. The dominant failure mode in this
codebase is not entering a function partway through, it is whole functions never
being found — which is items 3, 5 and 6. Revisit when a title actually shows
mid-function indirect targets; the design (a `switch (_entry)` goto prologue,
`void(void)` wrappers preserved so manual overrides and the table are unaffected)
is worked out and recorded here so it does not need re-deriving.

### Full block-threading — **rejected (cost)**

One C function per basic block plus a dispatcher loop is the closest C analogue
of Microsoft's flat arena and is correct by construction. It also costs an
indirect call and a loop iteration per ~10–20 guest instructions. Their model
works because it compiles to `jmp`; ours would not. Not worth it while the
measured problem is function *discovery*.

### Materialise flag outcomes as booleans — **rejected (no measured defect)**

Microsoft decomposes PowerPC CR0 into four separate bytes at `[rbx-64..-61]`, in
the *optimized* tier. The transferable lesson is that materialising boolean
outcomes is cheap enough that a production compiler chooses it. Our lifter
already snapshots compare operands (`_fa/_fb/_fas/_fbs`) and threads `flag_state`
across blocks, which is equivalent in effect. No open bug points at it. Revisit
if a flag-correctness bug appears that the snapshot model cannot express.

### Interpreter fallback instead of unresolved stubs — **deferred (disproportionate)**

Their `Fb` tier is the honest answer to "we could not prove this statically", and
a `g_esp += 4` stub is not. But a real x86 interpreter is a large subsystem, and
item 3 just cut the residual from 46 to 15 — of which most are unaligned and
therefore probably not code at all. The gap is no longer big enough to justify
the machinery. Reconsider if a title shows a large irreducible stub count.

### Register/flag hardware mapping — **rejected (inherent)**

Guest registers in host registers, and guest flags as host flags, are the single
biggest performance factor in their x86 translator. Both are unreachable from C
and would require a native x64 backend, which would cost the debuggability that
makes this project tractable. Known ceiling; not chased.

### Caching TLS registers into locals — **planned, blocked on measurement**

The reachable part of the register-model gap: load the live set at function
entry, write back at exits and before calls. Whether it is a win is not obvious —
correctness requires a write-back before *every* call and a re-read after, and
that traffic may cost more than the TLS indirections it removes. Needs a
benchmark harness before it is worth attempting; doing it blind risks a whole
class of silent corruption for an unmeasured gain.

### `movbe` / endianness, `r15`-relative addressing — **rejected (not applicable)**

Same-endian guest, so no swap machinery is needed. And we map guest memory at its
original VA with pointer casts, which is strictly better than their base-register
form. Already optimal.

### GPU: async PSO preload, per-title format override table — **planned, different subsystem**

`DX12PSOPreloader<AsyncCachedPSO>` precompiles pipeline states from the persisted
shader cache so first-encounter draws do not hitch; `OverrideGuestTextureFormats`
/ `GetOverrideMap` is a data-driven per-title texture-format override table.
Both are good ideas for `src/d3d` and neither depends on the recompiler work.
The preloader presupposes a persisted shader cache we do not have yet.
