"""A function whose only flag setter is `lock xadd` still declares _fa/_fas.

The lift publishes the sum into _fa/_fas, but the declaration check matched
the mnemonic against _RESULT_SNAPSHOT_SETTERS, which holds "xadd" -- and the
disassembler spells the locked form "lock xadd". HL2's sub_00581380
(`mov eax, edx; lock xadd [ecx], eax; ret`) failed to compile on it.
"""
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator


def test_lock_xadd_alone_declares_the_snapshot():
    image = bytes.fromhex('8bc2' 'f00fc101' 'c3')
    base = 0x10000
    config._install([config.Section('.text', base, len(image), 0, len(image), True)],
                    entry_point=base, kernel_thunk_addr=base, origin='lock-xadd-decl-test')
    db = {base: {'start': hex(base), 'end': base + len(image), '_addr': base, 'size': len(image)}}
    code = FunctionTranslator(image, db).translate_function(base, db[base])
    assert '_fa = _old + _add' in code, code
    assert 'uint32_t _fa = 0' in code, code
