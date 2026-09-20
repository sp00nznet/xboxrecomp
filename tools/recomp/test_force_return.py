"""--force-return hands a function's callers a constant.

Bring-up keeps arriving at the same shape: a title waits on a service the
runtime does not implement yet, the function that reports "is it finished"
answers no for ever, and everything past it is unreachable and therefore
untestable. Shin Megami Tensei: Nine does this with its XMV decoder -- the
title screen asks whether the intro movie has ended and never gets a yes.

What that costs without this option is a hand edit to the generated C, buried
in hundreds of megabytes of output where nothing names it and no one else can
reproduce the run.

Two properties matter and both are checked here. The value is assigned at the
*ret*, not by skipping the body: the epilogue still runs, so esp is adjusted
by the function's own ret -- 4 for a cdecl, 4+n for a stdcall -- and nothing
has to infer the calling convention. And the emitted code is gated on
g_force_return, so a build carrying a forced function behaves normally until
the variable is set.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000


def _translate(image, force_returns=None, size=None, extra=None):
    """Lift one function. `size` bounds it when the image holds two of them,
    which is what a tail jump needs: a jump inside the function is an
    ordinary branch, and only a jump out of it is a tail call."""
    config._install(
        [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="force-return-test")
    n = size if size is not None else len(image)
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + n,
                 "_addr": BASE, "size": n}}
    for a, ln in (extra or {}).items():
        db[a] = {"start": f"0x{a:08X}", "end": a + ln, "_addr": a, "size": ln}
    return FunctionTranslator(image, db,
                              force_returns=force_returns).translate_function(
        BASE, db[BASE])


def test_forced_function_assigns_at_the_ret():
    #   mov eax, 1
    #   ret
    image = b"\xB8\x01\x00\x00\x00\xC3"
    plain = _translate(image)
    assert "g_force_return" not in plain, plain

    forced = _translate(image, {BASE: 0})
    assert "if (g_force_return) eax = 0x0U;" in forced, forced
    # On the ret line, so the epilogue and the esp adjustment still happen.
    ret_lines = [l for l in forced.splitlines() if "return;" in l]
    assert any("g_force_return" in l and "esp += 4" in l for l in ret_lines), \
        ret_lines


def test_stdcall_keeps_its_own_stack_adjustment():
    #   mov eax, 1
    #   ret 8            <- pops two arguments as well as the return address
    image = b"\xB8\x01\x00\x00\x00\xC2\x08\x00"
    forced = _translate(image, {BASE: 0})
    ret_lines = [l for l in forced.splitlines() if "return;" in l]
    assert any("esp += 12" in l for l in ret_lines), ret_lines
    assert any("g_force_return" in l for l in ret_lines), ret_lines


def test_a_tail_jump_carries_it_too():
    """A tail call leaves through the target, not through a ret.

    The function that motivated this option ends exactly that way -- Shin
    Megami Tensei: Nine's "has the movie finished" query is a tail jump into
    its decoder -- so an implementation that only handles ret would miss its
    own motivating case. The assignment lands after the call and before the
    return: the target still runs, the caller still gets the constant.
    """
    #   +0  jmp +1          <- out of this function, into the next one
    #   +2  nop              (still inside, never reached)
    #   +3  ret              <- a second function starts here
    image = b"\xEB\x01\x90\xC3"
    forced = _translate(image, {BASE: 7}, size=3,
                        extra={BASE + 3: 1})
    tails = [l for l in forced.splitlines() if "tail" in l]
    assert tails, forced
    assert any("g_force_return" in l and "eax = 0x7U" in l for l in tails), \
        tails


def test_other_functions_are_untouched():
    image = b"\xB8\x01\x00\x00\x00\xC3"
    other = _translate(image, {0x00020000: 0})
    assert "g_force_return" not in other, other


if __name__ == "__main__":
    test_forced_function_assigns_at_the_ret()
    test_stdcall_keeps_its_own_stack_adjustment()
    test_a_tail_jump_carries_it_too()
    test_other_functions_are_untouched()
    print("ok")
