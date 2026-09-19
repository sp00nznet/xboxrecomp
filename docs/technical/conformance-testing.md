# Conformance Testing

The unit tests in `tools/recomp/test_lifter_*.py` compare the lifter's **output
text** against an expected string. That catches a rewrite that changes the
emitted form, and it catches nothing else. It cannot tell you whether the C it
emitted computes what the instruction computes, because nothing in the loop ever
executes either one.

Every bug in the v0.6.0 changelog was of that second kind: the generated C
compiled, linked, ran, and was quietly wrong. `repe cmpsb` reporting "equal"
regardless of input, `movaps` moving 4 of 16 bytes, a signed compare evaluated
at the wrong width — all of them pass a string-comparison test suite.

`tools/conformance` closes that gap by executing both sides.

## The idea, borrowed

This is [ps3recomp](https://github.com/sp00nznet/ps3recomp)'s methodology, which
is worth stating plainly since the two projects share a maintainer and the
approach transfers almost intact:

1. **Check the decoder against an oracle you did not write.** ps3recomp runs the
   SDK's `ppu-lv2-objdump` over a binary and diffs mnemonic-per-address against
   its own decoder, with an explicit alias table for the simplified mnemonics
   objdump prefers.
2. **Check the lifter by executing it.** `lift_selftest.py` compiles a corpus of
   C with the real PPU compiler, lifts the resulting PowerPC, compiles the
   lifted C on the host, runs both, and requires bit-exact agreement.
3. **Whitelist known divergences** so the audit sits at zero noise and any *new*
   divergence is a failure rather than another line in a long report.
4. **Assert the observable thing.** Its CI checks the presented pixel, not the
   process exit code.

## What changes for x86

Pillar 1 mostly does not apply to us: we do not have our own decoder to audit.
We use Capstone, which is already the independent implementation. The layer
worth auditing here is our *operand model* on top of it, not the decode.

Pillar 2 gets much stronger, and this is the important difference. ps3recomp has
to reason about a PowerPC while running on an x86 host, so its oracle is a
*model* — the SDK toolchain, an independent decoder, a hand-built PPC
interpreter. A model can be wrong, and when it is, it is wrong in the same
places your lifter is.

We target x86 and run on x86. **The host CPU is the reference implementation.**
There is no model to disagree with: we execute the actual instruction bytes on
actual silicon and compare that against the lifted C. Nothing else in the
recompilation pipeline gets an oracle that good.

## How it works

Each case in `tools/conformance/cases.py` is a short x86 sequence whose net
effect lands in `eax`, written as assembly *text*:

```python
Case("neg_sbb_separated",
     "neg's carry into a separated sbb -- the real codegen shape",
     ["neg eax", "push edx", "mov edx, 1", "pop edx",
      "sbb ecx, ecx", "mov eax, ecx"], _PAIRS),
```

Per case, the runner:

1. Emits that text into a C function as an MSVC `__asm` block, bracketed by
   three-`nop` markers, and compiles with `/FAc`. **MSVC is the assembler** —
   we never hand-encode, so the bytes cannot drift from the intent.
2. Reads the exact bytes back out of the `.cod` listing, between the markers.
3. Lifts those bytes through the real `Disassembler` + `lift_basic_block`.
4. Generates a harness holding both the native function and the lifted C,
   runs each over the same input vectors, and compares `eax`.

Inputs cluster on the boundaries that separate a correct implementation from a
plausible one: `0x7F`/`0x80`/`0xFF` at byte width, `0x7FFF`/`0x8000` at word
width, `0x80000000`, `0xFFFFFFFF`, zero. A bug that evaluates an 8-bit operand
at 32 bits is invisible at `0x00000005` and obvious at `0x000000FF`.

```
py -3 -m tools.conformance          # everything
py -3 -m tools.conformance -k neg   # one family
py -3 -m tools.conformance --keep   # keep the generated C to read
```

It also reports separately on any instruction that lifted to a bare comment.
Those are **invisible to the comparison** — an instruction that lifts to nothing
usually leaves `eax` untouched, so the two sides can agree while the lift is
doing nothing at all. That listing is the one part of the output that is not a
pass/fail.

## Lifting the way recomp lifts

Two details matter, and both were got wrong on the first attempt:

- Lift through **`lift_basic_block`**, not `lift_instruction`. The peephole that
  turns `cmp` + `jcc`/`setcc`/`cmovcc` into a real condition lives at block
  level. Lifting one instruction at a time reports a stale `_flags` that the
  block pass never emits, and produces a spectacular run of false failures.
- Set **`needs_cf`** the way `FunctionTranslator` does. The lifter only produces
  carry when something downstream consumes it, so a snippet lifted without it is
  not the code recomp would generate.

The rule underneath both: if the harness does not drive the lifter exactly as
the recompiler does, it is testing a path that does not ship.

## Comparing x87 and SSE

The integer cases compare `eax`. The other two kinds compare architectural
state, because that is where their bugs live.

**x87** compares the **stack depth** as well as the values. Most x87 bugs this
project has had were pop-count bugs rather than arithmetic bugs — a handler
that forgets to pop leaks a slot, everything after it reads `st(i)` one off,
and the *first* value still looks right. Depth comes from the status word's
TOP field on the native side and from `g_fp_top` on the lifted side; both
start at zero after `finit`, and both decrement on push, so they track.

The x87 precision control is set to **double (53-bit)** rather than the
hardware default of extended (64-bit). Our model holds the stack as C
`double`, so at extended precision the hardware carries more bits than the
model can and add/sub/mul/div/sqrt disagree in the last place for reasons that
have nothing to do with lifting. At PC=53 those five are correctly rounded on
both sides and must match **bit-exactly**.

That leaves the transcendentals, where the x87's polynomial and libm's are
genuinely different implementations. Those cases carry an explicit `tol`. This
is the whitelist pillar: the divergence is real, it is named per case rather
than hidden behind a blanket tolerance, and a wrong *answer* still fails.

**SSE** compares all eight XMM registers as raw bytes, lane by lane — the
whole point, since modelling XMM as a scalar float made `movaps` move 4 of 16
bytes and nothing noticed. Comparison is bit-exact, including `-0.0` vs `0.0`,
which is precisely the difference a mis-implemented `MINPS` tie-break gets
wrong.

## What it found

**`stc` / `clc` / `cmc` were unhandled** and lifted to `/* TODO: stc */`, so
the carry a following `adc`/`sbb` read kept whatever the last arithmetic left
in it. MSVC emits these around multi-word arithmetic and the "return a bool in
CF" idiom.

**`fxch st(i)` was a silent no-op.** Capstone reports `fxch` with *both*
operands — `(st(0), st(i))` — and it is the only x87 form that does; `fadd`,
`fcom`, `fld` and `fstp` all arrive with the explicit register alone. The
handler read operand 0, got the implicit `st(0)`, and emitted a swap of st0
with itself. Every `fxch` in every title did nothing.

**`fnstsw` did not model TOP.** The status word carries TOP in bits 11–13,
which land in AH bits 3–5, and we model TOP — it is `g_fp_top`. Leaving it out
made `fnstsw ax` disagree with the hardware on every read taken while the
stack was non-empty. The `ax` form was also only writing AH, while the
instruction writes all of AX.

It also found two bugs in *itself* before finding that one, which is worth
recording because both would have produced confident false reports:

- Lifting per-instruction instead of per-block, as above.
- The `.cod` byte column wraps after five bytes onto a continuation line with no
  address. The first parser missed the continuation, silently dropping every
  instruction longer than five bytes — that is, every 32-bit immediate. A
  dropped instruction is precisely the failure this tool exists to catch, so
  having it in the harness was worse than not having the harness.

## Phase two: whole functions

Snippets test instructions someone thought to write down. `corpus.py` tests
what the optimiser actually emits.

Each entry is a C function. The runner compiles the corpus with **`/O2 /GS-
/arch:IA32`**, **links it** into a DLL at a fixed base with relocations
stripped, lifts each function out of the linked image through the real
`FunctionTranslator` — not just the lifter, so frame handling, labels and block
layout are exercised too — and runs the lifted C against the original compiled
function over the same arguments.

Linking is what makes this work on real code. In an unlinked `.obj` every
reference outside the function is still zero, waiting for the linker, so
lifting those bytes gives confident nonsense. In the linked image the addresses
are final: a jump table, a float constant in `.rdata` and a call to a CRT
helper are all just numbers. That is also the shape a real XBE arrives in, so
the pipeline is the same one a port uses rather than a special case.

The harness maps the image at the address it was linked for with
`VirtualAlloc`, so a guest address is a host address, `g_xbox_mem_offset` stays
zero, and the lifted code reads the real `.rdata` bytes.

**Callees are lifted too.** A corpus function that calls `__allmul` needs
`__allmul` lifted, so the runner follows every direct call *and tail jump* out
of a function and lifts what it reaches — which is exactly what a real port
does with the CRT rather than a special case here. Anything still referenced
and undefined gets a stub that reports itself and fails the run, so a call that
went nowhere can never pass silently.

`/arch:IA32` matters. The Xbox CPU is a Pentium III: SSE1, no SSE2. Modern MSVC
defaults to SSE2 and puts doubles in XMM, which no real Xbox binary contains,
so without it the corpus tests instructions the target cannot execute and skips
the x87 paths every Xbox title actually uses.

The lifted function is `void f(void)` and takes its arguments off the guest
stack, right to left, under a return address — `__cdecl`, which is what the
recompiler's generated code already assumes. Setting that up is part of the
point: it exercises the real calling path.

Every corpus function is `__declspec(dllexport)`, or `/O2` would inline it into
its only caller and leave nothing to lift.

The runner reports any instruction that lifted to a bare comment. Those are
**invisible to the comparison** — an unhandled instruction usually leaves the
result register alone, so the two sides can agree while the lift does nothing.
That report is what diagnosed the float-to-int gap below.

## What phase two found

**Flag state followed address order, not control flow.** This is the big one.
`FunctionTranslator` threaded the `cmp`/`test` state from one block to the next
in *address* order. That is only right for fall-through. An optimising compiler
routinely lets a `jcc` consume a comparison from a block that is not its
neighbour in memory — and the block that *is* its neighbour often ends in an
`add`, which clobbers flags. The condition then came out as a plausible-looking
test of the wrong thing. Flag state now propagates along predecessor edges, and
only when every predecessor agrees; a block whose predecessors disagree, or
whose predecessor is a not-yet-walked back edge, gets no inherited state and
falls back rather than guessing.

**`js` / `jns` evaluated the sign at 32 bits.** SF is the sign bit of the result
at the *operand's* width. `test dl, dl; jns` asks about bit 7, but the lifted
condition tested the zero-extended byte as an `int32`, so `0x80`–`0xFF` looked
positive and the branch went the same way every time. Exactly the defect the
width-aware `CMP_L`/`CMP_G` fixed for the signed compares — `js`/`jns` were
simply missed at the time. `cmp` has the same issue, since it must truncate the
difference before taking its sign.

Both were found by a single corpus function: an eight-iteration loop over
`((a>>i)&1)`, which `/O2` unrolls into `test dl, 1<<i` + `je`, with the last
iteration becoming `test dl, dl` + `jns`. No snippet in phase one produced that
shape, because nobody thought to write it.

**`bt` / `btr` / `bts` / `btc` were unhandled.** 386 instructions, so real Xbox
code has them, and they lifted to a comment — the bit was silently left alone.
Found once the corpus started lifting the CRT's float-to-int helper, which uses
`btr` to clear a rounding-control bit of the x87 control word. That is the
`_control87` shape, not an exotic one.

## A divergence worth naming

Float-to-int is deliberately **not** in the corpus. MSVC lowers it to
`__ftol2`, whose modern LIBCMT implementation branches on `__isa_available`
and, on the fast path, uses `fisttp` — an **SSE3** instruction. The Xbox is a
Pentium III: no SSE3, and its XDK CRT's `__ftol` is plain x87.

Comparing against the host's helper would measure this machine's CPU dispatch
rather than the lifter, and the two sides cannot even agree on which branch to
take: the native side has a CRT that ran its startup, while the lifted side
reads the image's initial value for that variable.

This is recorded rather than papered over, and the unlifted-instruction report
will say so if such a function is ever pulled in again. `__allmul` — 64-bit
multiply — has no such problem and *is* lifted and verified.

## Phase three: a real title

```
py -3 -m tools.conformance --xbe "path/to/default.xbe"
```

The corpus tests code *we* compiled. This tests the code a title actually
shipped — and the oracle gets better again, because Xbox code is 32-bit x86 and
this harness is a 32-bit x86 process. The game's machine code is not merely
liftable, it is **executable**. Map the XBE where it was linked for, call one
of its functions directly, run the lifted C over the same arguments, compare.

No game files are needed to run the rest of the suite, and none are included
here; the phase only runs when you point it at an XBE you own.

No title to hand? `py -3 tools/conformance/mkxbe.py` writes a small synthetic
one to `tools/conformance/test.xbe` — enough to exercise the phase, not a
substitute for a real title.

```
py -3 -m tools.conformance --xbe tools/conformance/test.xbe
```

### Choosing what to call

A function qualifies if it ends in a plain `ret` — **not** `ret imm16`, since a
stdcall callee cleans argument bytes we would have to guess — has no indirect
branch, no `fs:` access, no string operation (they walk esi/edi for a count in
ecx, and a garbage count is not a fault, it is a very long memcpy), nothing
privileged, and nothing that lifts to a bare comment.

**Pointer arguments are supplied, not refused.** Some arguments are pointers
into a scratch buffer both sides see identically. Refusing every function that
dereferences an argument was what kept the comparable set tiny; handing them a
buffer makes them testable, and the buffer is compared afterwards so a function
that *writes* is checked on what it wrote, not just what it returned.

**Callees are lifted too.** Seeding only from `push ebp; mov ebp, esp` finds the
framed functions and stops; an FPO callee has no such prologue, so its caller
used to be dropped for calling into the unknown — which is most of them. A
direct `call` target is a function start by definition, so those are followed,
and a function is admissible only once its whole call tree is. Extending a
function's extent past its first `ret` matters too: stopping there truncates
anything with a block below the exit, which then looks like it jumps outside
itself.

On Burnout 3 that took the comparable set from **17 to 37**, out of 1,617
functions decoded.

### Surviving hostile code

Executing a title's code with arguments it never expected cannot be made safe
in-process — a function can corrupt whatever it likes before any handler sees
it. Several things bound the damage:

- both calls run under an exception guard, and the native side is wrapped in
  `pushad`/`popad`;
- the native call runs below a 32 KB gap in the stack, walked a page at a time.
  Jumping straight past leaves the guard page unhit and the first write below
  it faults in a way Windows cannot deliver — the process dies before any
  handler runs, which looks exactly like the lifted code crashing. Switching to
  a private stack buffer does not work either: an exception whose `esp` is
  outside the thread's stack cannot be unwound, and the process dies with
  `STATUS_BAD_STACK`;
- the image is restored from a pristine copy before every run, so a write
  cannot leak into the next run or into the other side of this one;
- and when something still brings the harness down, the runner reads the
  progress markers, quarantines the casualty, and re-runs. That converges in a
  few passes and costs no recompile.

### Both sides start from the same state

Four things had to be equalised, and every one of them produced a convincing
false failure first:

- **The integer registers.** A `void` function never writes `eax`, so the native
  side returned whatever the caller left there. The call now enters with every
  GPR zeroed, which needs an indirect call so `eax` itself can be cleared.
- **The x87 stack.** Many of this era's maths helpers take their argument in
  `st(0)`, and an empty stack is not neutral: the hardware yields the indefinite
  value where the model yields `0.0`.
- **The x87 control word.** `finit` resets it to the hardware default, while the
  model holds `0x027F`. Any function that reads it — the `_control87` family —
  then reports a different answer for reasons unrelated to lifting.
- **The stacks themselves**, both filled with `0xCD`. A function reading an
  uninitialised local would otherwise see this process's leftovers on one side
  and ours on the other. Poisoning both means such a function faults on the
  poison and is skipped, rather than producing a mismatch that looks real.

The guest register names also have to be uncovered around the inline assembly:
`RECOMP_GENERATED_CODE` makes `eax` mean `g_eax` in that translation unit, and
the rewrite reaches into `__asm` blocks too.

### Both sides start from the same state

Two things had to be equalised before the comparison meant anything, and both
were the harness's fault rather than the lifter's:

- **The integer registers.** A `void` function never writes `eax`, so the
  native side returned whatever the caller happened to leave there while the
  lifted side began at zero. The call now enters with every GPR zeroed, which
  needs an indirect call through a global so `eax` itself can be cleared.
- **The x87 stack.** Plenty of this era's maths helpers take their argument in
  `st(0)` rather than on the call stack, and an empty stack is *not* neutral:
  the hardware yields the indefinite value where the model yields `0.0`. Both
  sides are now given the same value in `st(0)`. Without it, a Burnout 3
  float-to-int helper "failed" on every vector over an input neither side had
  been handed.

The register names also have to be uncovered around the inline assembly:
`RECOMP_GENERATED_CODE` makes `eax` mean `g_eax` in that translation unit, and
the rewrite reaches into `__asm` blocks too.

### Results

| Title | Comparable | Vectors | Mismatches |
|---|---|---|---|
| Burnout 3: Takedown | 37 | 161 | 0 |
| Blood Wake | 39 | 44 | 0 |
| Crimson Skies | 35 | 227 | 2 functions, open |

**What it found.** `fnstsw` did not model **C2**, the unordered bit. An x87
compare against a NaN sets C3, C2 and C0 together, and `fucompp; fnstsw ax;
test ah, 44h; jp` is how this era's CRT asks "is this a NaN". Reporting
"equal" answered *no* every time, so every float classification in a title took
the wrong branch. The lifter's own comment had assumed non-NaN maths; Crimson
Skies' float classification is the counter-example, and it found itself.

Two Crimson Skies functions still diverge and are **not yet explained** — one on
a byte it writes into the scratch buffer, one on its return value. They are
real signals, not known-benign, and are the next thing to chase.

## Extending it

Add a case to `cases.py`, or a function to `corpus.py`. Worth going after next:

- **The two open Crimson Skies divergences**, above.
- **The x87 status word's low byte** — the exception flags. A NaN compare sets
  the invalid flag, which the model does not track, so any case comparing the
  whole of `ax` rather than just the condition codes diverges on bits unrelated
  to what it is testing.
- **Indirect calls through data** — vtables and function-pointer tables, which
  is where a real bring-up spends most of its time.
