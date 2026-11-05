"""disasm_lazily yields what capstone's eager disasm yields, batch by batch."""
import random

import pytest
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

from tools.recomp.disasm import disasm_lazily


@pytest.mark.parametrize('seed', range(40))
@pytest.mark.parametrize('batch', [1, 2, 32])
def test_same_instructions_and_same_stop(seed, batch):
    rng = random.Random(seed)
    # Valid runs (nop, mov eax imm32, a 15-byte add, ud2, ret) and, for every
    # third seed, the invalid 0f 04: some streams stop early, some reach the end.
    parts = [b'\x90', b'\xb8\x01\x02\x03\x04',
             bytes.fromhex('2e3e26646566818480000000000100'),
             b'\x0f\x0b', b'\xc3']
    code = b''.join(rng.choice(parts) for _ in range(rng.randrange(0, 200)))
    if seed % 3 == 0:
        cut = rng.randrange(0, len(code) + 1)
        code = code[:cut] + b'\x0f\x04' + code[cut:]
    cs = Cs(CS_ARCH_X86, CS_MODE_32)
    eager = [(i.address, i.size) for i in cs.disasm(code, 0x1000)]
    lazy = [(i.address, i.size) for i in disasm_lazily(cs, code, 0x1000, batch)]
    assert lazy == eager


def _fields(insns):
    return [(i.address, i.size, i.mnemonic, i.op_str, i.jump_target)
            for i in insns]


@pytest.mark.parametrize('seed', range(40))
def test_cfg_with_a_shared_cache_matches_without(seed):
    """disassemble_cfg with one decode cache over many windows of the same
    image returns what a cold decode of each window returns, including
    windows that end inside an instruction the cache already holds."""
    from tools.recomp.disasm import Disassembler

    rng = random.Random(seed)
    parts = [b'\x90', b'\xb8\x01\x02\x03\x04',
             bytes.fromhex('2e3e26646566818480000000000100'),
             b'\x74\x05', b'\xeb\x03', b'\x0f\x04', b'\xc3']
    image = b''.join(rng.choice(parts) for _ in range(300))
    base = 0x10000
    d = Disassembler()
    cache = {}
    for _ in range(60):
        lo = base + rng.randrange(0, len(image) // 2)
        hi = rng.randrange(lo + 1, base + len(image) + 1)
        raw = image[lo - base:hi - base]
        entries = {rng.randrange(lo, hi) for _ in range(3)}
        cold = d.disassemble_cfg(raw, lo, hi, entries, stop_mnemonics=('ud2',))
        warm = d.disassemble_cfg(raw, lo, hi, entries, stop_mnemonics=('ud2',),
                                 cache=cache)
        assert _fields(warm) == _fields(cold)
