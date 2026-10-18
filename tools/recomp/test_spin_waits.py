"""Spin-wait lowering (--spin-waits): which loops match, how the back edge is
emitted, the file's selection modes, and that nothing changes without it.

Every image is synthetic: a one-function .text at BASE and a data word at
DATA that the loops poll."""

import json
import os
import re
import shutil
import subprocess
import tempfile

import pytest

from tools.recomp import config
from tools.recomp.translator import FunctionTranslator
from tools.recomp.spin_waits import SpinWaitConfig, SpinWaitReport

BASE = 0x10000
DATA = 0x20000
D32 = DATA.to_bytes(4, "little").hex()

# L: cmp [DATA], eax ; jl L ; ret
CMP_MEM_REG = f"3905{D32}7cf8c3"
# L: cmp dword [DATA], 5 ; jne L ; ret
CMP_MEM_IMM = f"833d{D32}0575f7c3"
# L: mov eax, [DATA] ; test eax, eax ; jz L ; ret
LOAD_TEST = f"a1{D32}85c074f7c3"
# L: pause ; cmp [DATA], eax ; jl L ; ret
PAUSE = f"f3903905{D32}7cf6c3"
# L: test dword [eax+0x100410], 0x10000 ; jne L ; ret -- the XDK KickOff's wait
# for PFB's kick bit, as two XDK titles compile it; the base register is
# set before the loop and never written in it.
TEST_REG_DISP = "f780100410000000010075f4c3"

# L: mov [DATA], eax ; cmp [DATA], eax ; jl L ; ret
STORE = f"8905{D32}3905{D32}7cf2c3"
# L: mov esi, [DATA] ; cmp dword [esi], 0 ; jne L ; ret
ADDR_REG = f"8b35{D32}833e0075f5c3"
# L: lock cmpxchg [DATA], ecx ; jne L ; ret
LOCKED = f"f00fb10d{D32}75f6c3"
# L: cmp eax, ebx ; jl L ; ret
REG_ONLY = "39d87cfcc3"
# L: cmp [DATA], eax ; jge out ; jmp L ; out: ret
TWO_BLOCK = f"3905{D32}7d02ebf8c3"
# L: inc esi ; cmp dword [esi], 0 ; jne L ; ret
INC_ADDR = "46833e0075fac3"


def _translate(hexcode, spin=None):
    image = bytes.fromhex(hexcode)
    config._install([config.Section(".text", BASE, len(image), 0, len(image), True),
                     config.Section(".data", DATA, 0x1000, len(image), 0, False)],
                    entry_point=BASE, kernel_thunk_addr=BASE,
                    origin="spin-wait-test")
    db = {BASE: {"start": hex(BASE), "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    t = FunctionTranslator(image, db)
    report = None
    if spin is not None:
        report = SpinWaitReport(spin)
        t.spin_waits = report
    code = t.translate_function(BASE, db[BASE])
    return code, report


LISTED = SpinWaitConfig(auto=False, sites={BASE})
AUTO = SpinWaitConfig(auto=True)


@pytest.mark.parametrize("hexcode", [CMP_MEM_REG, CMP_MEM_IMM, LOAD_TEST, PAUSE,
                                     TEST_REG_DISP],
                         ids=["cmp-mem-reg", "cmp-mem-imm", "load-test", "pause",
                              "test-reg-disp-imm"])
def test_wait_shapes_are_lowered(hexcode):
    code, report = _translate(hexcode, LISTED)
    assert f"RECOMP_SPIN_WAIT(0x{BASE:08X}u); goto loc_{BASE:08X}; }}" in code, code
    assert code.count("RECOMP_SPIN_WAIT") == 1
    assert report.finish() == []
    site = report.to_json()["sites"][0]
    assert site["state"] == "lowered" and site["lowered_bodies"] == 1


def test_only_the_back_edge_waits():
    """The first test of the condition never waits: the label is followed by
    the compare, and the wait sits inside the taken branch."""
    code, _ = _translate(CMP_MEM_REG, LISTED)
    lines = [l.strip() for l in code.splitlines()]
    i = lines.index(f"loc_{BASE:08X}: ;")
    assert "RECOMP_SPIN_WAIT" not in lines[i + 1]
    wait = next(l for l in lines if "RECOMP_SPIN_WAIT" in l)
    assert wait.startswith("if (")


@pytest.mark.parametrize("hexcode,reason", [
    (STORE, "writes memory"),
    (ADDR_REG, "address register"),
    (LOCKED, "not a load or compare"),
    (REG_ONLY, "reads no memory"),
    (INC_ADDR, "not a load or compare"),
], ids=["store", "address-register", "locked", "register-only", "inc-address"])
def test_unsafe_shapes_are_refused(hexcode, reason):
    code, report = _translate(hexcode, AUTO)
    assert "RECOMP_SPIN_WAIT" not in code
    site = report.to_json()["sites"][0]
    assert site["state"] == "rejected"
    assert any(reason in r for r in site["reasons"]), site


def test_listed_unsafe_site_is_an_error():
    _, report = _translate(STORE, LISTED)
    errors = report.finish()
    assert errors and f"0x{BASE:08X}" in errors[0] and "writes memory" in errors[0]


def test_two_block_loop_is_not_a_candidate():
    code, report = _translate(TWO_BLOCK, AUTO)
    assert "RECOMP_SPIN_WAIT" not in code
    assert report.to_json()["sites"] == []


def test_listed_site_with_no_loop_is_an_error():
    _, report = _translate(TWO_BLOCK, LISTED)
    errors = report.finish()
    assert errors and "no body has a self-loop" in errors[0]


def test_auto_lowers_and_exclude_keeps():
    code, _ = _translate(CMP_MEM_REG, AUTO)
    assert "RECOMP_SPIN_WAIT" in code
    code, report = _translate(
        CMP_MEM_REG, SpinWaitConfig(auto=True, exclude={BASE}))
    assert "RECOMP_SPIN_WAIT" not in code
    assert report.to_json()["sites"][0]["state"] == "excluded"


def test_unlisted_candidate_is_reported_not_lowered():
    code, report = _translate(CMP_MEM_REG, SpinWaitConfig(auto=False))
    assert "RECOMP_SPIN_WAIT" not in code
    assert report.to_json()["sites"][0]["state"] == "candidate (not listed)"


@pytest.mark.parametrize("hexcode", [CMP_MEM_REG, STORE, TWO_BLOCK, REG_ONLY])
def test_no_file_and_empty_list_change_nothing(hexcode):
    """Without --spin-waits the matcher does not run; with a file that
    selects nothing, the emitted text is the same byte for byte."""
    plain, _ = _translate(hexcode, None)
    empty, _ = _translate(hexcode, SpinWaitConfig(auto=False))
    assert plain == empty
    assert "RECOMP_SPIN_WAIT" not in plain


def test_config_file_parsing(tmp_path):
    p = tmp_path / "spin.json"
    p.write_text(json.dumps({"auto": False,
                             "sites": [{"va": "0x00012340", "note": "vblank"}],
                             "exclude": ["0x00045670"]}))
    c = SpinWaitConfig.load(str(p))
    assert c.sites == {0x12340} and c.exclude == {0x45670}
    assert c.notes[0x12340] == "vblank"
    assert c.wants(0x12340) and not c.wants(0x45670)
    p.write_text(json.dumps({"auto": False, "site": []}))
    with pytest.raises(ValueError):
        SpinWaitConfig.load(str(p))


_RUNTIME = os.path.join(os.path.dirname(__file__), "..", "..", "templates",
                        "runtime", "recomp_types.h")


def test_lowered_back_edge_compiles():
    """The macro as the runtime header defines it, around the statement the
    lowering emits, is valid C11."""
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        pytest.skip("no C compiler")
    hdr = open(_RUNTIME, encoding="utf-8").read()
    m = re.search(r"#define RECOMP_SPIN_WAIT\(va\)[^\n]*\\\n[^\n]*\n", hdr)
    assert m, "RECOMP_SPIN_WAIT not defined in recomp_types.h"
    code, _ = _translate(CMP_MEM_REG, LISTED)
    stmt = next(l for l in code.splitlines() if "RECOMP_SPIN_WAIT" in l)
    src = ("#include <stdint.h>\n"
           "volatile int g_xbox_spin_sleep;\n"
           "volatile int g_xbox_irq_pending;\n"
           "volatile int g_xbox_guest_cpu_waiters;\n"
           "void xbox_SpinWait(uint32_t va) { (void)va; }\n"
           + m.group(0) +
           "#define CMP_L(a, b) ((a) < (b))\n"
           "int f(int32_t _fas, int32_t _fbs) {\n"
           f"loc_{BASE:08X}: ;\n" + stmt + "\n return 0; }\n")
    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, "t.c")
        open(c, "w").write(src)
        r = subprocess.run([cc, "-std=c11", "-Wall", "-Werror", "-c", c, "-o",
                            os.path.join(d, "t.o")], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr + src
