"""
Guest busy-wait loops, lowered to a sleeping wait (opt-in: --spin-waits).

A title that waits for its next frame often does it in a loop that only
reads memory and branches back:

    L: cmp  [counter], eax
       jl   L

Lifted as is, that loop holds a host core for as long as it waits (a title's
vblank wait can sit at most of one core). With a spin-wait file the generator
finds such loops and emits, on the taken back edge only,

    if (cond) { RECOMP_SPIN_WAIT(0x...u); goto L; }

RECOMP_SPIN_WAIT (templates/runtime/recomp_types.h) does nothing unless the
runtime's pacing mode is "sleep"; then the thread blocks until the kernel
signals a state change or 1 ms passes. The first test of the condition never
waits, so a loop that is already done costs nothing more.

The matcher is deliberately narrow. A candidate is one basic block that ends
in a conditional jump to its own start, and whose other instructions are
only:
  - cmp / test reading memory, or a register the block loads from memory;
  - mov / movzx / movsx of memory or an immediate into a register that no
    address in the block uses;
  - pause.
Anything that writes memory, calls, pushes or pops, uses a string, locked,
port, cpuid, rdtsc or FPU instruction, changes a register an address uses,
or reads no memory at all is not a wait the runtime can sleep through.

The file (JSON):
    {"auto": false,
     "sites": [{"va": "0x00012340", "note": "..."}],
     "exclude": []}
"auto": true lowers every candidate not excluded; false lowers only "sites".
A listed site that fails the matcher in any body that contains it is an
error: a shape the matcher rejects is never forced through.
"""

import json

_COMPARE = frozenset({"cmp", "test"})
_LOADS = frozenset({"mov", "movzx", "movsx"})
_BACK_EDGE_EXCLUDED = frozenset({"loop", "loope", "loopne"})

# Sub-registers to the 32-bit register they live in, so "writes al" and
# "addresses through eax" are seen as the same register.
_ALIAS = {}
for _r32, _subs in (("eax", ("ax", "al", "ah")), ("ebx", ("bx", "bl", "bh")),
                    ("ecx", ("cx", "cl", "ch")), ("edx", ("dx", "dl", "dh")),
                    ("esi", ("si",)), ("edi", ("di",)), ("ebp", ("bp",)),
                    ("esp", ("sp",))):
    _ALIAS[_r32] = _r32
    for _s in _subs:
        _ALIAS[_s] = _r32


def _canon(reg):
    return _ALIAS.get(reg, reg) if reg else reg


def _parse_va(v):
    return int(v, 16) if isinstance(v, str) else int(v)


class SpinWaitConfig:
    """The parsed spin-wait file."""

    def __init__(self, auto=False, sites=(), exclude=(), notes=None):
        self.auto = bool(auto)
        self.sites = set(sites)
        self.exclude = set(exclude)
        self.notes = dict(notes or {})

    @classmethod
    def load(cls, path):
        with open(path) as f:
            doc = json.load(f)
        if not isinstance(doc, dict):
            raise ValueError(f"{path}: expected a JSON object")
        unknown = set(doc) - {"auto", "sites", "exclude", "about"}
        if unknown:
            raise ValueError(f"{path}: unknown keys {sorted(unknown)}")
        sites, notes = set(), {}
        for s in doc.get("sites", []):
            va = _parse_va(s["va"] if isinstance(s, dict) else s)
            sites.add(va)
            if isinstance(s, dict) and s.get("note"):
                notes[va] = s["note"]
        exclude = {_parse_va(s["va"] if isinstance(s, dict) else s)
                   for s in doc.get("exclude", [])}
        return cls(doc.get("auto", False), sites, exclude, notes)

    def wants(self, va):
        if va in self.exclude:
            return False
        return self.auto or va in self.sites


def match(bb):
    """(True, None) when the block is a spin-wait candidate, else
    (False, reason). A block that is not a self-loop gives (False, None):
    it is not a loop at all, so it is not reported."""
    insns = bb.instructions
    if not insns:
        return False, None
    last = insns[-1]
    if (not last.is_cond_jump or last.mnemonic in _BACK_EDGE_EXCLUDED
            or last.jump_target != bb.start):
        return False, None

    body = insns[:-1]
    if not body:
        return False, "no instruction before the branch"
    addr_regs, loaded, written = set(), set(), set()
    reads_memory = False

    for insn in body:
        m = insn.mnemonic
        ops = insn.operands
        for op in ops:
            if op.type == "mem":
                for r in (op.mem_base, op.mem_index):
                    if r:
                        addr_regs.add(_canon(r))
        if m == "pause":
            continue
        if m in _COMPARE:
            if len(ops) != 2:
                return False, f"{m} with {len(ops)} operands"
            if any(op.type == "mem" for op in ops):
                reads_memory = True
                continue
            regs = {_canon(op.reg) for op in ops if op.type == "reg"}
            if not (regs & loaded):
                return False, f"{m} at 0x{insn.address:08X} reads no memory"
            continue
        if m in _LOADS:
            if len(ops) != 2 or ops[0].type != "reg":
                return False, f"{m} at 0x{insn.address:08X} writes memory"
            if ops[1].type not in ("mem", "imm"):
                return False, f"{m} at 0x{insn.address:08X} copies a register"
            dst = _canon(ops[0].reg)
            written.add(dst)
            if ops[1].type == "mem":
                reads_memory = True
                loaded.add(dst)
            continue
        return False, f"{m} at 0x{insn.address:08X} is not a load or compare"

    if written & addr_regs:
        return False, ("writes an address register ("
                       + ", ".join(sorted(written & addr_regs)) + ")")
    if not reads_memory:
        return False, "reads no memory"
    return True, None


_GOTO_RE = "goto loc_{:08X};"


def lower(stmts, va):
    """The block's statements with the back edge to va calling
    RECOMP_SPIN_WAIT first. Raises ValueError if the branch is not found."""
    goto = _GOTO_RE.format(va)
    for i in range(len(stmts) - 1, -1, -1):
        s = stmts[i]
        if goto in s and s.lstrip().startswith("if ("):
            out = list(stmts)
            out[i] = s.replace(
                goto, "{ RECOMP_SPIN_WAIT(0x%08Xu); %s }" % (va, goto), 1)
            return out
    raise ValueError(f"no conditional back edge to loc_{va:08X} in the "
                     f"lifted block")


class SpinWaitReport:
    """Every candidate seen, per VA, across all bodies."""

    def __init__(self, config):
        self.config = config
        self.sites = {}      # va -> {"bodies", "lowered", "rejected": {body: reason}}
        self.errors = []

    def _rec(self, va):
        return self.sites.setdefault(va, {"bodies": [], "lowered": 0,
                                          "rejected": {}})

    def consider(self, func_addr, bb, stmts):
        """Lower bb's back edge if it is a wanted candidate. Returns the
        statements to emit."""
        ok, why = match(bb)
        if not ok and why is None:
            return stmts
        va = bb.start
        rec = self._rec(va)
        rec["bodies"].append(func_addr)
        if not ok:
            rec["rejected"][func_addr] = why
            if va in self.config.sites:
                self.errors.append(
                    f"spin-wait site 0x{va:08X} in sub_{func_addr:08X}: {why}")
            return stmts
        if not self.config.wants(va):
            return stmts
        try:
            stmts = lower(stmts, va)
        except ValueError as e:
            self.errors.append(f"spin-wait site 0x{va:08X} in "
                               f"sub_{func_addr:08X}: {e}")
            return stmts
        rec["lowered"] += 1
        return stmts

    def finish(self):
        """Listed sites never seen as a self-loop are errors too."""
        for va in sorted(self.config.sites):
            if va not in self.sites:
                self.errors.append(f"spin-wait site 0x{va:08X}: no body has a "
                                   f"self-loop block starting there")
        return self.errors

    def to_json(self):
        out = []
        for va in sorted(self.sites):
            r = self.sites[va]
            if r["lowered"]:
                state = "lowered"
            elif r["rejected"]:
                state = "rejected"
            elif va in self.config.exclude:
                state = "excluded"
            else:
                state = "candidate (not listed)"
            entry = {"va": f"0x{va:08X}", "bodies": len(r["bodies"]),
                     "lowered_bodies": r["lowered"], "state": state}
            if r["rejected"]:
                reasons = sorted(set(r["rejected"].values()))
                entry["rejected_in"] = len(r["rejected"])
                entry["reasons"] = reasons
            if va in self.config.notes:
                entry["note"] = self.config.notes[va]
            out.append(entry)
        return {"auto": self.config.auto,
                "candidates": sum(1 for e in out if e["state"] != "rejected"),
                "lowered_sites": sum(1 for e in out if e["state"] == "lowered"),
                "lowered_bodies": sum(e["lowered_bodies"] for e in out),
                "errors": list(self.errors),
                "sites": out}
