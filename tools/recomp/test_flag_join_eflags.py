"""Joins with no agreed flag state read g_eflags, written on every edge."""
from tools.recomp.test_flag_fallbacks import BASE, translator_for


def lift(hexcode):
    ft, db = translator_for(bytes.fromhex(hexcode), [BASE])
    return ft.translate_function(BASE, db[BASE]), ft.flag_fallbacks


def body(code):
    return code.split('{', 1)[1]


def test_a_successor_of_the_join_reads_what_the_edges_wrote():
    # test ecx,ecx; jz alt; cmp eax,0; jmp join; alt: dec eax;
    # join: jmp next; next: je out; nop; out: ret. The join has no reader of
    # its own, so no per-edge local: both edges write ZF to g_eflags.
    code, ff = lift('85c97405' '83f800' 'eb01' '48' 'eb00' '7401' '90' 'c3')
    assert '_flags /*' not in body(code), code
    assert body(code).count('g_eflags = (g_eflags & ~0x40u)') == 2, code
    assert '(g_eflags & 0x40u) /* join eflags */' in code
    assert ff.summary()['live_unassigned'] == 0


def test_a_single_reader_join_keeps_the_per_edge_local():
    # test ecx,ecx; jz alt; cmp eax,0; jmp join; alt: dec eax;
    # join: je out; nop; out: ret -- the cheaper _jf form stays.
    code, ff = lift('85c97405' '83f800' 'eb01' '48' '7401' '90' 'c3')
    assert 'g_eflags' not in body(code), code
    assert '_jf_' in code
    assert ff.summary()['live_unassigned'] == 0


def test_a_join_with_two_readers_writes_only_the_bits_read():
    # test ecx,ecx; jz alt; cmp eax,0; jmp join; alt: dec eax;
    # join: sete dl; js out; nop; out: ret. Both ZF and SF reach g_eflags;
    # CF, PF and OF are left alone.
    code, ff = lift('85c97405' '83f800' 'eb01' '48' '0f94c2' '7801' '90' 'c3')
    assert '_flags /*' not in body(code), code
    assert body(code).count('g_eflags = (g_eflags & ~0xC0u)') == 2, code
    assert '((g_eflags >> 7) & 1u) /* join eflags */' in code
    assert ff.summary()['live_unassigned'] == 0


def test_cf_at_a_join_comes_from_the_carry_local():
    # test ecx,ecx; jz alt; cmp eax,1; jmp join; alt: stc; mul ecx;
    # join: jb out; nop; out: ret. mul leaves no lazy state, but every edge
    # keeps _cf current, so jb reads it and nothing is written to g_eflags.
    code, ff = lift('85c97405' '83f801' 'eb03' 'f9f7e1' '7201' '90' 'c3')
    assert 'if (_cf /* join eflags */)' in code, code
    assert 'g_eflags' not in body(code).replace('/* join eflags */', '')
    assert ff.summary()['live_unassigned'] == 0


def edge_writes(code):
    return [ln for ln in body(code).splitlines()
            if 'flags for the join' in ln or 'flags of this edge' in ln]


def test_the_game_rejoin_shape_sets_the_local_on_both_edges():
    # and eax,0x80000003; jns join; dec eax; or eax,-4; inc eax;
    # join: mov edi,0x54; js out; nop; out: ret -- BLiNX 2 sub_000CE740.
    code, ff = lift('2503000080' '7905' '48' '83c8fc' '40' 'bf54000000'
                    '7801' '90' 'c3')
    assert '_flags /*' not in body(code), code
    assert 'if (_jf_0001000C /* js' in code
    assert len(edge_writes(code)) == 2, code
    assert ff.summary()['live_unassigned'] == 0


def test_a_back_edge_into_a_two_reader_join_writes_on_every_edge():
    # test ecx,ecx; jz join; cmp eax,0; join: sete dl; js out;
    # add eax,1; jmp join; out: ret
    code, ff = lift('85c9' '7403' '83f800' '0f94c2' '7805' '83c001' 'ebf6' 'c3')
    assert '_flags /*' not in body(code), code
    assert len(edge_writes(code)) == 3, code     # entry jz, cmp, back edge
    assert ff.summary()['live_unassigned'] == 0


def test_a_predecessor_ending_in_a_jcc_writes_before_it():
    # test ecx,ecx; jz join; dec eax; jne join; join: sete dl; js out;
    # nop; out: ret
    code, ff = lift('85c9' '7403' '48' '7500' '0f94c2' '7801' '90' 'c3')
    assert '_flags /*' not in body(code), code
    lines = body(code).splitlines()
    k = next(i for i, ln in enumerate(lines) if '/* jne' in ln)
    assert 'flags for the join' in lines[k - 1], code
    assert len(edge_writes(code)) == 2
    assert ff.summary()['live_unassigned'] == 0


def test_a_merge_whose_later_reader_it_cannot_answer_becomes_a_join():
    # test ecx,ecx; jz alt; cmp eax,1; jmp join; alt: cmp bl,1;
    # join: call next; next: js out; nop; out: ret. The merged compare
    # cannot give js (the widths differ), and the js sits past a call, so
    # the merge block reads g_eflags and both edges write SF. The read after
    # the call is listed: the callee may rewrite g_eflags.
    code, ff = lift('85c97405' '83f801' 'eb03' '80fb01' 'e800000000'
                    '7801' '90' 'c3')
    assert '_flags /*' not in body(code), code
    assert len(edge_writes(code)) == 2, code
    (join,) = ff.to_json()['join_eflags']['partial_joins']
    assert [e['class'] for e in join['edges']] == ['across-call']


def test_stc_after_a_join_keeps_the_join_state():
    # test ecx,ecx; jz alt; cmp eax,0; jmp join; alt: dec eax;
    # join: stc; jmp next; next: sete dl; je out; nop; out: ret
    code, ff = lift('85c97405' '83f800' 'eb01' '48' 'f9' 'eb00' '0f94c2'
                    '7401' '90' 'c3')
    assert '_flags /*' not in body(code), code
    assert len(edge_writes(code)) == 2, code
    assert ff.summary()['live_unassigned'] == 0
