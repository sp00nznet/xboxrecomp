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

Joins no longer fall back. One whose edges leave no single setter reads its
flags from g_eflags, and every edge with a setter writes them there first
(translator._join_edges). An edge with nothing to write -- the function
entry, a mul, data decoded as code, a switch arm -- is listed under
`join_eflags` with its reason, since the read behind it can still see a
value no edge wrote. `live_unassigned` counts both kinds of read: the
`_flags` sites and the joins with such an edge.
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


def _reason_class(reason):
    """A coarse class for a site's or an edge's reason: what it would take to
    give that read a value."""
    for prefix, cls in (("entry block", "function-entry"),
                        ("function entry", "function-entry"),
                        ("no predecessors", "unreached"),
                        ("unreached", "unreached"),
                        ("in-block undefined", "isa-undefined"),
                        ("undefined after", "isa-undefined"),
                        ("in-block untracked", "untracked-instruction"),
                        ("untracked", "untracked-instruction"),
                        ("switch", "unseen-edge"),
                        ("read after a call", "across-call"),
                        ("no line", "no-line"),
                        ("unmatched", "unmatched")):
        if reason.startswith(prefix):
            return cls
    if "cannot answer" in reason:
        return "setter-gap"
    return "join"


def _flag_names(bits):
    return ",".join(name for bit, _, name in _L.FLAG_BITS if bits & bit)


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
        self.joins = {}      # g_eflags join block address -> record

    @staticmethod
    def edge_reason(bb, is_entry, has_preds):
        """Why a predecessor leaves no flag state to write into a join."""
        reason = _in_block_reason(bb.instructions, len(bb.instructions), "")
        if reason and ("undefined after" in reason or "untracked" in reason):
            return reason[len("in-block "):]
        if is_entry:
            return "function entry"
        if not has_preds:
            return "unreached predecessor"
        return f"no state through 0x{bb.start:08X}"

    def note_join(self, func_addr, func_name, bb, need, missing):
        """A block reading its flags from g_eflags: `need` is every bit it
        (and the blocks it passes them to) reads, `missing` the edges that
        cannot write some of them, as (pred or None, bits, reason)."""
        edges = [{"from": (f"0x{p:08X}" if isinstance(p, int)
                           else p or "unseen"),
                  "flags": _flag_names(bits), "reason": why,
                  "class": _reason_class(why)}
                 for p, bits, why in missing]
        rec = self.joins.get(bb.start)
        if rec is None or (edges and not rec["edges"]):
            self.joins[bb.start] = {
                "block": f"0x{bb.start:08X}", "reads": _flag_names(need),
                "function": func_name, "function_start": f"0x{func_addr:08X}",
                "bodies": rec["bodies"] + 1 if rec else 1, "edges": edges,
            }
        else:
            rec["bodies"] += 1

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
                    "reason": reason, "class": _reason_class(reason),
                    "function": func_name,
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
                    rec["class"] = _reason_class(reason)

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
        partial = [r for r in self.joins.values() if r["edges"]]
        live = list(self.sites.values()) + partial
        in_observed = sorted({rec["function"] for rec in live
                              if int(rec["function_start"], 16) in observed})
        by_class = {}
        for rec in self.sites.values():
            by_class[rec["class"]] = by_class.get(rec["class"], 0) + 1
        edge_classes = {}
        for rec in partial:
            for cls in sorted({e["class"] for e in rec["edges"]}):
                edge_classes[cls] = edge_classes.get(cls, 0) + 1

        def ranked(d):
            return dict(sorted(d.items(), key=lambda kv: (-kv[1], kv[0])))
        return {
            "emitted": self.emitted,
            "unique_sites": len(self.sites),
            "by_reason": ranked(by_reason),
            "by_class": ranked(by_class),
            "joins": len([r for r in self.sites.values()
                          if r["reason"].startswith("join: ")]),
            "join_eflags": {
                "blocks": len(self.joins),
                "assigned": len(self.joins) - len(partial),
                "partial": len(partial),
                "partial_by_edge_class": ranked(edge_classes),
            },
            "live_unassigned": len(live),
            "observed_functions": in_observed,
        }

    def to_json(self, observed=()):
        doc = self.summary(observed)
        doc["sites"] = sorted(self.sites.values(),
                              key=lambda r: (r["address"], r["cond"]))
        doc["join_eflags"]["partial_joins"] = sorted(
            (r for r in self.joins.values() if r["edges"]),
            key=lambda r: r["block"])
        return doc
