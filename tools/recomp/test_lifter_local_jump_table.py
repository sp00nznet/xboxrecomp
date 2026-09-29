"""A jump table is not always named by its first slot.

When the translator has recovered no table for a dispatch, the lifter reads
one itself, and it read forward from the displacement only. MSVC's CRT
memcpy/memmove -- in every XDK title -- names its tables three other ways, and
each became an indirect tail jump into the middle of memcpy that the runtime
could not resolve, so the copy did nothing (MechAssault, sub_001ED440).
"""
import struct

import pytest

from . import config
from .lifter import Lifter

BASE = 0x10000
ARMS = [BASE + 0x40, BASE + 0x48, BASE + 0x50]
GARBAGE = 0x90001ED5            # the jmp bytes MSVC parks in front of a table


@pytest.fixture(autouse=True)
def layout(monkeypatch):
    monkeypatch.setattr(config, "_SECTIONS", [
        config.Section(".text", BASE, 0x400, 0, 0x400, True),
    ])


def _lifter(table_at, words):
    data = bytearray(b"\xcc" * 0x400)
    for i, w in enumerate(words):
        struct.pack_into("<I", data, table_at - BASE + 4 * i, w)
    lifter = Lifter()
    lifter.xbe_data = bytes(data)
    lifter.func_start, lifter.func_end = BASE, BASE + 0x100
    return lifter


def test_biased_table_skips_the_unread_slot_zero():
    table = BASE + 0x20          # jmp [eax*4 + table], eax = 1..3
    lifter = _lifter(table, [GARBAGE, *ARMS])
    assert lifter._read_local_table(table) == ARMS


def test_table_counted_down_from_its_last_slot():
    table = BASE + 0x20          # jmp [ecx*4 + LAST]
    lifter = _lifter(table, [*ARMS, GARBAGE])
    assert lifter._read_local_table(table + 8) == ARMS


def test_table_counted_up_to_one_past_its_end():
    table = BASE + 0x20          # jmp [ecx*4 + END], ecx = -3..-1
    lifter = _lifter(table, [*ARMS, GARBAGE])
    assert lifter._read_local_table(table + 12) == ARMS


def test_entries_outside_the_function_are_not_arms():
    table = BASE + 0x20
    lifter = _lifter(table, [BASE + 0x200, *ARMS, BASE + 0x300])
    assert lifter._read_local_table(table + 4) == ARMS
