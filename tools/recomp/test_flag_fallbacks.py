"""The `_flags` fallback report: each site once, with why its state was lost."""
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator

BASE = 0x10000

# test ecx,ecx; jz alt; cmp eax,0; jmp join; alt: dec eax;
# join: je out; nop; out: ret -- a compare and an arithmetic setter (the
# follow-up join shape in recomp-test-flag-join D5). Each edge now evaluates
# the je itself (upstream v0.13.1's per-edge joins), so it is no fallback.
STAGE1_JOIN = bytes.fromhex('85c97405' '83f800' 'eb01' '48'
                            '7401' '90' 'c3')

# The same join with mul on the alternate edge: mul leaves ZF undefined, so
# that edge has nothing to write. The join reads g_eflags (the compare edge
# writes ZF there) and is listed with the mul edge unassigned.
MUL_JOIN = bytes.fromhex('85c97405' '83f800' 'eb02' 'f7e1'
                         '7401' '90' 'c3')
JOIN_JE = BASE + 2 + 2 + 3 + 2 + 2


def translator_for(image, starts):
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE,
                    origin='flag-fallback-test')
    end = BASE + len(image)
    db = {s: {'start': hex(s), 'end': end, '_addr': s, 'size': end - s}
          for s in starts}
    return FunctionTranslator(image, db), db


def test_a_join_each_edge_answers_is_not_a_fallback():
    ft, db = translator_for(STAGE1_JOIN, [BASE])
    code = ft.translate_function(BASE, db[BASE])
    assert '_flags /* je' not in code, code
    assert ft.flag_fallbacks.summary()['unique_sites'] == 0


def test_a_join_with_an_unknown_edge_is_reported():
    ft, db = translator_for(MUL_JOIN, [BASE])
    code = ft.translate_function(BASE, db[BASE])
    assert '_flags /* je' not in code, code
    assert '(g_eflags & 0x40u) /* join eflags */' in code, code
    assert ft.flag_fallbacks.sites == {}
    doc = ft.flag_fallbacks.to_json()
    assert doc['live_unassigned'] == 1
    assert doc['join_eflags']['partial'] == 1
    (join,) = doc['join_eflags']['partial_joins']
    assert join['block'] == f'0x{JOIN_JE:08X}' and join['reads'] == 'ZF'
    assert join['edges'] == [{'from': f'0x{BASE + 9:08X}', 'flags': 'ZF',
                              'reason': 'undefined after mul',
                              'class': 'isa-undefined'}]


def test_an_alias_body_counts_the_site_once():
    ft, db = translator_for(MUL_JOIN, [BASE, JOIN_JE])
    ft.translate_function(JOIN_JE, db[JOIN_JE])     # alias: enters at the je
    ft.translate_function(BASE, db[BASE])
    summary = ft.flag_fallbacks.summary()
    # The alias body still falls back at its entry (out of scope: no edge
    # of that body exists to write the flags); the owner joins in g_eflags.
    assert summary['unique_sites'] == 1, ft.flag_fallbacks.sites
    site = ft.flag_fallbacks.sites[(JOIN_JE, 'je')]
    assert site['reason'] == 'entry block' and site['class'] == 'function-entry'
    assert summary['live_unassigned'] == 2


def test_an_in_block_clobber_is_reported_as_such():
    # mul ecx; je out; nop; out: ret
    ft, db = translator_for(bytes.fromhex('f7e1' '7401' '90' 'c3'), [BASE])
    ft.translate_function(BASE, db[BASE])
    site = ft.flag_fallbacks.sites[(BASE + 2, 'je')]
    assert site['reason'] == 'in-block undefined after mul'


def test_observed_seeds_name_their_functions():
    ft, db = translator_for(MUL_JOIN, [BASE])
    ft.translate_function(BASE, db[BASE])
    assert ft.flag_fallbacks.summary({BASE})['observed_functions'] == [
        ft.flag_fallbacks.joins[JOIN_JE]['function']]
    assert ft.flag_fallbacks.summary(set())['observed_functions'] == []
