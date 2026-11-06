"""disassemble_cfg with a decode cache returns what it returns without one.

The sweep's callback probes (DisasmEngine.probes_as_callback_body) decode a
CFG for every gap they test, tens of thousands per title, and decode the same
bytes again and again: neighbouring gaps share code, and a probe re-decodes its
gap each time a switch adds arms. One decoder with cache_cfg_decodes=True
decodes each instruction once. The cache key is the address and the 15 bytes
there, so a slice that ends inside an instruction, or bytes Capstone cannot
decode, behave as they do without the cache.
"""
import random

from .disasm import Disassembler

BASE = 0x10000
# 0: push ebp | 1: mov ebp, esp | 3: je 11 | 5: mov eax, 1 | 10: ret
# 11: xor eax, eax | 13: ret
CODE = bytes.fromhex("55" "8bec" "7406" "b801000000" "c3" "31c0" "c3")


def _shape(insns):
    return [(i.address, i.size, i.mnemonic, i.op_str, i.bytes_hex,
             i.jump_target, i.call_target, tuple(i.regs_written))
            for i in insns]


def _both(cached, raw, start, end, entries, **kw):
    want = Disassembler().disassemble_cfg(raw, start, end, entries, **kw)
    got = cached.disassemble_cfg(raw, start, end, entries, **kw)
    assert _shape(got) == _shape(want)
    return got


def test_follows_both_arms_of_a_branch():
    insns = _both(Disassembler(cache_cfg_decodes=True), CODE, BASE,
                  BASE + len(CODE), {BASE})
    assert [i.address - BASE for i in insns] == [0, 1, 3, 5, 10, 11, 13]


def test_an_instruction_past_the_bound_ends_the_stream():
    cached = Disassembler(cache_cfg_decodes=True)
    _both(cached, CODE, BASE, BASE + len(CODE), {BASE})   # warm the cache
    # The bytes run on past the bound, so the cached window at 5 holds all of
    # mov eax, 1 (5..10); the bound at 8 still ends the stream before it.
    insns = _both(cached, CODE, BASE, BASE + 8, {BASE})
    assert [i.address - BASE for i in insns] == [0, 1, 3]


def test_random_bytes_match_without_the_cache():
    rng = random.Random(0x5EED)
    cached = Disassembler(cache_cfg_decodes=True)     # one decoder for all
    image = bytes(rng.randrange(256) for _ in range(4096))
    for _ in range(400):
        lower = rng.randrange(0, len(image) - 64)
        upper = lower + rng.randrange(1, 64)
        # Half the time the bytes run on past the bound, as a section's do.
        raw = image[lower:upper + rng.choice((0, 16))]
        entries = {lower + rng.randrange(upper - lower) for _ in range(3)}
        _both(cached, raw, BASE + lower, BASE + upper,
              {BASE + e for e in entries},
              stop_mnemonics=("int3", "ud2", "hlt"))
