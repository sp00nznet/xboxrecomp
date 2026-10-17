"""Where the lifter fell back to `_flags`, and why.

A jcc, setcc or cmovcc whose flag state is unknown compiles against `_flags`,
a variable nothing assigns: the branch is never taken and the setcc writes 0.
Most such sites are harmless -- data decoded as code, alias bodies entered at
a branch target -- but one inside a function the title really runs is a
branch that silently never fires. One title's periodic stutter was one:
a `cmp` and a `test` met at a join, the merge refused them, and a state
machine's wait compiled as never-waiting.

This module records every such site as the translator emits it, with the
reason the state was lost, so the report says which ones are joins worth
fixing. It is a counted diagnostic, not an error: most sites cannot be fixed
because there is nothing to know, and a gate on the total could never pass.
Sites are keyed by guest address, so the alias bodies that re-emit one guest
branch count it once.
"""
import re

from . import lifter as _L
from .lifter import _operand_width

# A consumer statement on the fallback. The setcc and cmovcc forms write the
# mnemonic as `_flags /* sete */`; the jcc form as `_flags /* je: ... */`.
# `_flags = (` is a real assignment (repe cmps) and the declaration carries
# "fallback flag var"; neither is a consumer.
_CONSUMER = re.compile(r"\b_flags /\* (\w+)")

_SETTERS = (set(_L.FLAG_SETTERS) | set(_L._EFLAGS_SETTERS)
            | {"popfd", "sahf", "stc", "clc", "cmc", "fcomi", "fcomip",
               "fucomi", "fucomip"}
            | set(_L._BARE_STRING_COMPARES))


def _is_consumer(insn):
    m = insn.mnemonic
    return insn.is_cond_jump or m.startswith("set") or m.startswith("cmov")


def state_kind(state):
    """A short name for a flag state: `cmp32z` (a 32-bit compare against
    zero), `test8`, `dec:eax`, or the setter's mnemonic."""
    if state is None:
        return "none"
    kind, ops = state
    if kind in ("cmp", "test") and len(ops) == 2:
        w = _operand_width(ops[0]) or _operand_width(ops[1]) or 4
        zero = getattr(ops[1], "type", None) == "imm" and not ops[1].imm
        return f"{kind}{w * 8}" + ("z" if zero else "")
    if (kind in _L.ZF_FROM_DEST and ops
            and getattr(ops[0], "type", None) == "reg"):
        return f"{kind}:{ops[0].reg}"
    return kind


def _incoming_reason(cond, sources, known, is_entry, merged):
    if is_entry:
        return "entry block"
    if not sources:
        return "no predecessors"
    unknown = sum(1 for p in sources if known.get(p) is None)
    if unknown:
        return ("all predecessors unknown" if unknown == len(sources)
                else "some predecessor unknown")
    if merged is None:
        kinds = sorted({state_kind(known[p]) for p in sources})
        return "join: " + " + ".join(kinds)
    return f"incoming {state_kind(merged)} cannot answer {cond}"


def _in_block_reason(insns, k, cond):
    """Why the consumer at insns[k] has no state, if the block itself says:
    a setter in front of it that cannot answer the condition, or an
    instruction that clears the state. None means the state came in empty."""
    for j in range(k - 1, -1, -1):
        m = insns[j].mnemonic
        if m in _SETTERS or m.startswith("rep"):
            return f"in-block {m} cannot answer {cond}"
        if m in _L._FLAGS_UNDEFINED:
            return f"in-block undefined after {m}"
        if (m in _L._EFLAGS_PRESERVE or m.startswith("f")
                or m.startswith("j") or m.startswith("set")
                or m.startswith("cmov") or m in _L._SSE_CMP_NAMED):
            continue
        return f"in-block untracked {m}"
    return None


class FlagFallbacks:
    """The sites one translation emitted, by guest address and condition."""

    def __init__(self):
        self.sites = {}      # (insn address, cond) -> record
        self.emitted = 0     # every emitted consumer, alias copies included

    def note_block(self, func_addr, func_name, bb, stmts, sources, known,
                   is_entry, merged):
        hits = [m.group(1) for s in stmts for m in [_CONSUMER.search(s)]
                if m and "_flags = (" not in s]
        if not hits:
            return
        insns = bb.instructions
        consumers = [k for k, ins in enumerate(insns) if _is_consumer(ins)]
        ci = 0
        for cond in hits:
            self.emitted += 1
            k = None
            while ci < len(consumers):
                cand = consumers[ci]
                ci += 1
                if insns[cand].mnemonic == cond:
                    k = cand
                    break
            if k is None:
                addr, reason = bb.start, "unmatched consumer"
            else:
                addr = insns[k].address
                reason = (_in_block_reason(insns, k, cond)
                          or _incoming_reason(cond, sources, known,
                                              is_entry, merged))
            rec = self.sites.get((addr, cond))
            if rec is None:
                self.sites[(addr, cond)] = {
                    "address": f"0x{addr:08X}", "cond": cond,
                    "reason": reason, "function": func_name,
                    "function_start": f"0x{func_addr:08X}", "bodies": 1,
                }
            else:
                rec["bodies"] += 1
                # Prefer the owner body's reason over an alias's: an alias
                # starting at the jcc reports "entry block" for a site its
                # owner lost at a join.
                if rec["reason"] == "entry block" and reason != "entry block":
                    rec.update(reason=reason, function=func_name,
                               function_start=f"0x{func_addr:08X}")

    def summary(self, observed=()):
        by_reason = {}
        for rec in self.sites.values():
            key = rec["reason"]
            if key.startswith("in-block "):
                key = "in-block"
            elif key.startswith("incoming "):
                key = "incoming cannot answer"
            elif key.startswith("join: "):
                key = "join"
            by_reason[key] = by_reason.get(key, 0) + 1
        observed = set(observed)
        in_observed = sorted({rec["function"] for rec in self.sites.values()
                              if int(rec["function_start"], 16) in observed})
        return {
            "emitted": self.emitted,
            "unique_sites": len(self.sites),
            "by_reason": dict(sorted(by_reason.items(),
                                     key=lambda kv: (-kv[1], kv[0]))),
            "joins": len([r for r in self.sites.values()
                          if r["reason"].startswith("join: ")]),
            "observed_functions": in_observed,
        }

    def to_json(self, observed=()):
        doc = self.summary(observed)
        doc["sites"] = sorted(self.sites.values(),
                              key=lambda r: (r["address"], r["cond"]))
        return doc
