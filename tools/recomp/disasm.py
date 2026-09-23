"""
Capstone x86-32 disassembly wrapper with basic block detection.

Disassembles raw bytes from the XBE and organizes instructions
into basic blocks within each function.
"""

from dataclasses import dataclass, field
from typing import Optional
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM, CS_OP_MEM


# x86 conditional jump mnemonics
COND_JUMPS = frozenset({
    "jo", "jno", "jb", "jnb", "jae", "je", "jz", "jne", "jnz",
    "jbe", "ja", "jnae", "jna", "js", "jns", "jp", "jnp",
    "jl", "jge", "jle", "jg", "jcxz", "jecxz",
    "loop", "loope", "loopne",
})


@dataclass
class Instruction:
    """A single disassembled x86 instruction."""
    address: int
    size: int
    mnemonic: str
    op_str: str
    bytes_hex: str

    # Operand details (populated from Capstone)
    operands: list = field(default_factory=list)

    # Resolved targets
    call_target: Optional[int] = None
    jump_target: Optional[int] = None
    memory_refs: list = field(default_factory=list)
    imm_values: list = field(default_factory=list)
    regs_written: list = field(default_factory=list)

    @property
    def is_call(self):
        return self.mnemonic == "call"

    @property
    def is_ret(self):
        return self.mnemonic in ("ret", "retn", "retf")

    @property
    def is_jump(self):
        return self.mnemonic == "jmp"

    @property
    def is_cond_jump(self):
        return self.mnemonic in COND_JUMPS

    @property
    def is_branch(self):
        return self.is_jump or self.is_cond_jump

    @property
    def is_terminator(self):
        return self.is_ret or self.is_jump

    @property
    def end_address(self):
        return self.address + self.size


@dataclass
class Operand:
    """Parsed instruction operand."""
    type: str  # "reg", "imm", "mem"
    # For reg:
    reg: Optional[str] = None
    # For imm:
    imm: Optional[int] = None
    # For mem: [base + index*scale + disp]
    mem_base: Optional[str] = None
    mem_index: Optional[str] = None
    mem_scale: int = 1
    mem_disp: int = 0
    mem_size: int = 0  # operand size in bytes
    # Segment override, when the instruction carries one. Only fs matters on
    # Xbox -- it is how a title reaches the TIB -- but dropping the prefix put
    # fs:[0] at linear address 0, which is also where a null pointer lands.
    mem_seg: str = None


@dataclass
class BasicBlock:
    """A sequence of instructions with one entry and one exit."""
    start: int
    instructions: list = field(default_factory=list)
    successors: list = field(default_factory=list)  # addresses of successor blocks

    @property
    def end(self):
        if self.instructions:
            return self.instructions[-1].end_address
        return self.start

    @property
    def last_insn(self):
        return self.instructions[-1] if self.instructions else None


# Capstone register ID → name mapping (populated on init)
_reg_names = {}


def _init_reg_names(cs):
    """Build register ID → name map from Capstone."""
    global _reg_names
    if _reg_names:
        return
    # Use Capstone's reg_name method for all possible register IDs
    for i in range(256):
        name = cs.reg_name(i)
        if name:
            _reg_names[i] = name


def _parse_operand(cs, cs_op, insn_obj):
    """Convert a Capstone operand to our Operand dataclass."""
    if cs_op.type == CS_OP_IMM:
        return Operand(type="imm", imm=cs_op.imm & 0xFFFFFFFF)
    elif cs_op.type == CS_OP_MEM:
        base = _reg_names.get(cs_op.mem.base) if cs_op.mem.base else None
        index = _reg_names.get(cs_op.mem.index) if cs_op.mem.index else None
        return Operand(
            type="mem",
            mem_base=base,
            mem_index=index,
            mem_scale=cs_op.mem.scale,
            mem_disp=cs_op.mem.disp & 0xFFFFFFFF if cs_op.mem.disp >= 0 else cs_op.mem.disp,
            mem_size=cs_op.size,
            mem_seg=(_reg_names.get(cs_op.mem.segment)
                     if getattr(cs_op.mem, "segment", 0) else None),
        )
    else:
        # Register operand
        return Operand(type="reg", reg=_reg_names.get(cs_op.reg, f"r{cs_op.reg}"))


class Disassembler:
    """Capstone-based x86-32 disassembler."""

    def __init__(self):
        self._cs = Cs(CS_ARCH_X86, CS_MODE_32)
        self._cs.detail = True
        _init_reg_names(self._cs)

    def _decode_instruction(self, cs_insn):
        """Convert one Capstone instruction into the translator model."""
        insn = Instruction(
            address=cs_insn.address,
            size=cs_insn.size,
            mnemonic=cs_insn.mnemonic,
            op_str=cs_insn.op_str,
            bytes_hex=cs_insn.bytes.hex(),
        )

        try:
            ops = cs_insn.operands
        except Exception:
            ops = []
        try:
            _, written = cs_insn.regs_access()
            insn.regs_written = [
                _reg_names.get(reg, self._cs.reg_name(reg))
                for reg in written
                if _reg_names.get(reg, self._cs.reg_name(reg))
            ]
        except Exception:
            insn.regs_written = []
        for cs_op in ops:
            op = _parse_operand(self._cs, cs_op, cs_insn)
            insn.operands.append(op)

            if cs_op.type == CS_OP_IMM:
                val = cs_op.imm & 0xFFFFFFFF
                insn.imm_values.append(val)
                if insn.is_call:
                    insn.call_target = val
                elif insn.is_branch:
                    insn.jump_target = val
            elif cs_op.type == CS_OP_MEM:
                if (cs_op.mem.base == 0 and cs_op.mem.index == 0
                        and cs_op.mem.disp != 0):
                    insn.memory_refs.append(cs_op.mem.disp & 0xFFFFFFFF)

        return insn

    def disassemble_function(self, raw_bytes, start_va, end_va, resync=None):
        """
        Disassemble bytes for a single function.
        Returns list of Instruction objects.

        `resync` are addresses known to be instruction starts even if a linear
        decode disagrees -- switch-table targets, in practice. A jump table
        embedded in .text is data, and decoding it as instructions leaves the
        stream misaligned: the bogus instruction covering the last table entry
        swallows the real one just past it, so no instruction (and therefore no
        basic block, and therefore no label) ever starts at the first case.
        The switch then compiles to a goto with no destination, which the
        translator turns into a no-op, and the case silently falls through to
        an unresolved indirect branch at run time.

        Decoding restarts at each such address and discards whatever instruction
        straddled it. The garbage decoded from the table itself is left alone:
        it is unreachable, because the block before it ends at the indirect
        jump.
        """
        size = end_va - start_va
        if size <= 0 or size > len(raw_bytes):
            return []

        decoded = {}
        for cs_insn in self._cs.disasm(raw_bytes[:size], start_va):
            decoded[cs_insn.address] = self._decode_instruction(cs_insn)

        for point in sorted(resync or ()):
            if not (start_va <= point < end_va) or point in decoded:
                continue
            for addr in [a for a, i in decoded.items()
                         if a < point < a + i.size]:
                del decoded[addr]
            for cs_insn in self._cs.disasm(raw_bytes[point - start_va:size],
                                           point):
                if cs_insn.address in decoded:
                    break          # rejoined a stream we already have
                decoded[cs_insn.address] = self._decode_instruction(cs_insn)

        return [decoded[a] for a in sorted(decoded)]

    def disassemble_cfg(self, raw_bytes, start_va, end_va, entry_points,
                        stop_addresses=None, stop_mnemonics=()):
        """Decode reachable instruction streams from an explicit worklist."""
        size = end_va - start_va
        if size <= 0 or size > len(raw_bytes):
            return []

        decoded = {}
        stop_addresses = set(stop_addresses or ())
        worklist = list(entry_points)
        while worklist:
            entry = worklist.pop()
            if not (start_va <= entry < end_va) or entry in decoded:
                continue
            offset = entry - start_va
            for cs_insn in self._cs.disasm(raw_bytes[offset:size], entry):
                if cs_insn.address != entry and cs_insn.address in stop_addresses:
                    break
                if cs_insn.address >= end_va or cs_insn.address in decoded:
                    break
                insn = self._decode_instruction(cs_insn)
                decoded[insn.address] = insn

                if (insn.is_cond_jump and insn.jump_target is not None
                        and start_va <= insn.jump_target < end_va):
                    worklist.append(insn.jump_target)
                if insn.is_jump:
                    if (insn.jump_target is not None
                            and start_va <= insn.jump_target < end_va):
                        worklist.append(insn.jump_target)
                    break
                if insn.is_ret or insn.mnemonic in stop_mnemonics:
                    break

        return [decoded[addr] for addr in sorted(decoded)]

    def build_basic_blocks(self, instructions, func_start, func_end,
                           extra_leaders=None, stop_mnemonics=()):
        """
        Partition instructions into basic blocks.
        A new block starts at:
        - Function entry
        - Branch targets
        - Instructions after branches/calls
        - Any address in extra_leaders (e.g., switch table targets)
        """
        if not instructions:
            return []

        # Find block leaders (addresses that start a new basic block)
        leaders = {func_start}
        if extra_leaders:
            leaders.update(extra_leaders)
        for insn in instructions:
            if insn.is_branch or insn.is_cond_jump:
                if insn.jump_target and func_start <= insn.jump_target < func_end:
                    leaders.add(insn.jump_target)
                # Instruction after the branch is also a leader
                leaders.add(insn.end_address)
            elif insn.is_call or insn.mnemonic in stop_mnemonics:
                # Instruction after call is a leader (call might not return)
                leaders.add(insn.end_address)

        # Build address → instruction index map
        addr_to_idx = {insn.address: i for i, insn in enumerate(instructions)}

        # Sort leaders that actually have instructions
        sorted_leaders = sorted(l for l in leaders if l in addr_to_idx)

        # Create basic blocks
        blocks = []
        for i, leader in enumerate(sorted_leaders):
            bb = BasicBlock(start=leader)
            idx = addr_to_idx[leader]

            # Add instructions until next leader or terminator
            next_leader = sorted_leaders[i + 1] if i + 1 < len(sorted_leaders) else func_end
            while idx < len(instructions):
                insn = instructions[idx]
                if insn.address >= next_leader and insn.address != leader:
                    break
                bb.instructions.append(insn)
                idx += 1

                # Block ends at terminators
                if insn.is_ret or insn.is_jump or insn.mnemonic in stop_mnemonics:
                    break
                if insn.is_cond_jump:
                    break

            # Determine successors
            if bb.instructions:
                last = bb.last_insn
                if last.is_ret or last.mnemonic in stop_mnemonics:
                    pass  # No successors
                elif last.is_jump:
                    if last.jump_target and func_start <= last.jump_target < func_end:
                        bb.successors.append(last.jump_target)
                elif last.is_cond_jump:
                    # Fallthrough
                    if last.end_address < func_end:
                        bb.successors.append(last.end_address)
                    # Branch target
                    if last.jump_target and func_start <= last.jump_target < func_end:
                        bb.successors.append(last.jump_target)
                else:
                    # Normal instruction, falls through to next block
                    if last.end_address < func_end and last.end_address in addr_to_idx:
                        bb.successors.append(last.end_address)

            blocks.append(bb)

        return blocks
