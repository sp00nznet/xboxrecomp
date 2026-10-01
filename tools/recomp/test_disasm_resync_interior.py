"""Realigning at a switch arm drops every misaligned decode it overlaps.

disassemble_function decodes a function linearly, then restarts at each
switch-table target (resync). A jump table's bytes leave the linear stream out
of phase, and it only rejoins the real one a few instructions later. Restarting
at the arm used to drop just the one instruction straddling it; the rest of the
misaligned run -- instructions that start *inside* the real ones -- stayed, and
were lifted beside them.

In MechAssault's CRT memcpy the `44` inside `mov [edi+ecx*4-0x1C], eax`
survived as `inc esp`, so every 28-31 byte copy returned with esp one byte off
and popped garbage into its caller's esi and edi.
"""
from .disasm import Disassembler

# ret ; <one byte of table tail> ; the arm:
#   +2: mov eax, [esi+ecx*4-0x1C]      8b 44 8e e4
#   +6: mov [edi+ecx*4-0x1C], eax      89 44 8f e4
#   +10: ret                           c3
# Linearly, +1 decodes as `add [ebx+...], cl` over the first mov, then +7 as
# `inc esp` and +8 as `pop esp` inside the second, rejoining at +10.
BASE = 0x10000
CODE = bytes.fromhex("c3" "00" "8b448ee4" "89448fe4" "c3")


def _decode():
    return Disassembler().disassemble_function(
        CODE, BASE, BASE + len(CODE), resync=[BASE + 2])


def test_the_arm_decodes_as_itself():
    starts = [i.address - BASE for i in _decode()]
    assert 2 in starts and 6 in starts and 10 in starts


def test_no_misaligned_instruction_survives_inside_the_arm():
    insns = _decode()
    assert [i.address - BASE for i in insns if 2 <= i.address - BASE < 10] == [2, 6]
    mnemonics = {i.mnemonic for i in insns if BASE + 2 <= i.address < BASE + 10}
    assert "inc" not in mnemonics and "pop" not in mnemonics
