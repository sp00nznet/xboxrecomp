"""With --back-edge-yield every loop back edge calls RECOMP_BACK_EDGE, the guest
CPU's quantum check (templates/runtime/recomp_types.h), and forward jumps do
not; by default the edges stay bare."""

from tools.recomp import config
from tools.recomp.translator import FunctionTranslator

BASE = 0x10000

# +0 L: dec eax ; +1 jnz L ; +3 ret        -- a counted loop
COUNT_DOWN = "4875fdc3"
# +0 cmp eax, 5 ; +3 jge +2 (-> +7) ; +5 nop ; nop ; +7 ret  -- forward only
FORWARD = "83f8057d029090c3"


def _translate(hexcode, back_edge_yield=True):
    """back_edge_yield as --back-edge-yield sets it; None leaves the default."""
    image = bytes.fromhex(hexcode)
    config._install(
        [config.Section(".text", BASE, len(image), 0, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="back-edge-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    t = FunctionTranslator(image, db)
    if back_edge_yield is not None:
        t.back_edge_yield = back_edge_yield
    return t.translate_function(BASE, db[BASE])


def test_a_loop_back_edge_checks_the_quantum():
    code = _translate(COUNT_DOWN)
    assert ("{ RECOMP_BACK_EDGE(0x%08Xu); goto loc_%08X; }" % (BASE + 1, BASE)) in code, code
    assert code.count("RECOMP_BACK_EDGE") == 1


def test_a_forward_jump_does_not():
    code = _translate(FORWARD)
    assert "goto loc_00010007;" in code, code
    assert "RECOMP_BACK_EDGE" not in code


def test_by_default_the_edges_stay_bare():
    code = _translate(COUNT_DOWN, back_edge_yield=None)
    assert "goto loc_00010000; /* jne" in code, code
    assert "RECOMP_BACK_EDGE" not in code


def test_the_batch_translator_defaults_to_bare_edges():
    import inspect

    from tools.recomp.translator import BatchTranslator

    sig = inspect.signature(BatchTranslator.__init__)
    assert sig.parameters["back_edge_yield"].default is False


# +0 L0: dec ecx ; +1 jz J (+6) ; +3 cmp eax, 1 ; +6 J: jne L0 ; +8 ret
# J's predecessors disagree (a dec's flags and a cmp), so its jne reads a
# per-edge variable; the jne is a back edge.
JOIN = "49740383f80175f8c3"
# +0 L0: inc eax ; +1 jmp [eax*4 + TABLE] ; +8 A: ret ; +9..+11 nop ;
# +12 TABLE: L0, A                           -- a switch with a backward arm
TABLE = BASE + 12
SWITCH = ("40" + "ff2485" + TABLE.to_bytes(4, "little").hex() + "c3" + "909090"
          + BASE.to_bytes(4, "little").hex() + (BASE + 8).to_bytes(4, "little").hex())


def test_a_join_edge_variable_keeps_the_wrapped_goto_on_one_line():
    code = _translate(JOIN)
    assert "_jf_00010006" in code, code
    line = next(ln for ln in code.splitlines() if "RECOMP_BACK_EDGE" in ln)
    assert "_jf_00010006" in line and "goto loc_00010000; }" in line, line
    assert code.count("RECOMP_BACK_EDGE") == 1
    # The edge assignments were inserted before the predecessors' jumps.
    assert code.count("/* flags of this edge */") == 2, code


def test_a_switch_arm_going_backward_is_a_back_edge():
    code = _translate(SWITCH)
    assert "uint32_t _jt" in code, code
    assert "if (_jt == 0x00010000u) { RECOMP_BACK_EDGE(0x00010001u); goto loc_00010000; }" in code, code
    assert "if (_jt == 0x00010008u) goto loc_00010008;" in code, code
    assert code.count("RECOMP_BACK_EDGE") == 1
