"""
Cross-check the kernel bridge's ordinal routing against the export table.

Run: py -3 tools/kernel_audit/test_bridge_ordinals.py

src/kernel/kernel_bridge.c maps ordinals to bridge_<Name> wrappers by hand.
Nothing tied those numbers to the real kernel export list, and a block of them
had drifted by one -- so ordinal 24 (ExQueryNonVolatileSetting) dispatched to
ExQueryPoolBlockSize, and 67 (IoCreateSymbolicLink) to IoCreateFile. Both
"worked" in that they returned a plausible value, which is what made it hard
to see: the game got a pool size where it wanted EEPROM settings, and a
file-not-found where it wanted a drive mount.

KERNEL_EXPORTS in tools/xbe_parser is the reference. It is ordinal-indexed and
matches the published Xbox kernel export table.
"""

import os
import re
import sys

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
sys.path.insert(0, ROOT)

BRIDGE_C = os.path.join(ROOT, "src", "kernel", "kernel_bridge.c")
PARSER_PY = os.path.join(ROOT, "tools", "xbe_parser", "xbe_parser.py")


def load_exports():
    with open(PARSER_PY, encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    m = re.search(r"KERNEL_EXPORTS\s*[:=][^{]*\{(.*?)\n\}", src, re.S)
    assert m, "could not locate KERNEL_EXPORTS in tools/xbe_parser"
    pairs = re.findall(r"(\d+)\s*:\s*[\"']([A-Za-z_][A-Za-z0-9_]*)", m.group(1))
    exports = {int(o): n for o, n in pairs}
    assert len(exports) > 300, f"only parsed {len(exports)} exports"
    return exports


def load_routes():
    with open(BRIDGE_C, encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    routes = re.findall(
        r"case\s+(\d+):\s*return\s+bridge_([A-Za-z_][A-Za-z0-9_]*)\s*;", src)
    assert routes, "no bridge routes found"
    return [(int(o), n) for o, n in routes]


def test_every_route_matches_its_ordinal():
    exports = load_exports()
    bad = []
    for ordinal, name in load_routes():
        expected = exports.get(ordinal)
        if expected is None:
            bad.append(f"ordinal {ordinal} -> bridge_{name}, but no such export")
        elif expected != name:
            bad.append(
                f"ordinal {ordinal} -> bridge_{name}, but ordinal {ordinal} "
                f"is {expected}")

    assert not bad, "misrouted kernel ordinals:\n  " + "\n  ".join(bad)
    print(f"ok  every_route_matches_its_ordinal ({len(load_routes())} routes)")


def test_no_duplicate_ordinals():
    seen, dupes = set(), []
    for ordinal, name in load_routes():
        if ordinal in seen:
            dupes.append(f"ordinal {ordinal} routed twice (last: bridge_{name})")
        seen.add(ordinal)
    assert not dupes, "\n  ".join(dupes)
    print("ok  no_duplicate_ordinals")


def test_no_duplicate_arg_sizes():
    """The same ordinal twice in the arg-size table is a compile error.

    test_no_duplicate_ordinals covers the routing switch only. Argument sizes
    are a second switch over the same values, and a duplicate there is a C2196
    that surfaces only when something rebuilds the kernel library -- which is
    not necessarily the build you are running when you add the entry.
    """
    with open(BRIDGE_C, encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    match = re.search(r"stdcall_args_for_ordinal.*?\n\}", src, re.S)
    assert match, "could not locate stdcall_args_for_ordinal"
    seen, dupes = set(), []
    for ordinal in re.findall(r"case\s+(\d+):\s*return\s+-?\d+;", match.group(0)):
        ordinal = int(ordinal)
        if ordinal in seen:
            dupes.append("ordinal %d has two arg-size entries" % ordinal)
        seen.add(ordinal)
    assert not dupes, "\n  ".join(dupes)
    print("ok  no_duplicate_arg_sizes (%d sized)" % len(seen))


def test_arg_size_comments_match_their_ordinal():
    """The stdcall arg table names each function in a comment; check it.

    Routing and argument sizes are two separate tables, and fixing one does not
    fix the other. Ordinal 67 was routed correctly to IoCreateSymbolicLink while
    its arg entry still said `40; /* IoCreateFile(10) */` - so every call popped
    40 bytes instead of 8 and walked esp 32 bytes off. That does not fail where
    it happens: it surfaces later as a function returning with ebx/esi/edi
    restored from the wrong stack slots, which is about as far from "wrong
    stdcall size" as a symptom can look.
    """
    exports = load_exports()
    with open(BRIDGE_C, encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    m = re.search(r"static int stdcall_args_for_ordinal.*?\n\}", src, re.S)
    assert m, "could not locate stdcall_args_for_ordinal"

    bad = []
    for ordinal, _bytes, named in re.findall(
            r"case\s+(\d+):\s*return\s+(\d+);\s*/\*\s*([A-Za-z_][A-Za-z0-9_]*)",
            m.group(0)):
        ordinal = int(ordinal)
        expected = exports.get(ordinal)
        if expected and expected != named:
            bad.append(f"ordinal {ordinal} arg entry is commented {named}, "
                       f"but ordinal {ordinal} is {expected}")

    assert not bad, "arg-size table disagrees with the export table:\n  " + \
                    "\n  ".join(bad)
    print("ok  arg_size_comments_match_their_ordinal")


def test_every_routed_ordinal_has_an_arg_size():
    """A bridged ordinal with no arg entry falls through to `default: return 0`.

    The comment-matching test above cannot see this: there is no entry, so
    there is no comment to disagree with. Adding a bridge and forgetting the
    arg size is one edit, and it leaks the whole argument list on every call.

    Ordinal 47 (HalRegisterShutdownNotification, 2 args) was added this way.
    D3D's CMiniport::InitHardware calls it once, so esp came back 8 bytes low
    and the epilogue's pop edi/esi/ebx each read one slot too far down: `this`
    for the next call became 1, the DMA channel was initialised against
    garbage, and the title hung in a push-buffer wait several calls later with
    nothing to connect it back to a missing stdcall size.

    An ordinal that genuinely takes no arguments needs an explicit `return 0;`
    entry, so that "zero" is a decision on the record rather than a fallthrough.
    """
    exports = load_exports()
    with open(BRIDGE_C, encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    m = re.search(r"static int stdcall_args_for_ordinal.*?\n\}", src, re.S)
    assert m, "could not locate stdcall_args_for_ordinal"
    sized = {int(o) for o in
             re.findall(r"case\s+(\d+):\s*return\s+\d+;", m.group(0))}

    missing = [
        f"ordinal {o} routes to bridge_{n} but has no arg-size entry "
        f"({exports.get(o, '?')})"
        for o, n in load_routes() if o not in sized
    ]
    assert not missing, ("bridged ordinals fall through to `default: return 0` "
                         "and leak their args:\n  " + "\n  ".join(missing))
    print(f"ok  every_routed_ordinal_has_an_arg_size ({len(sized)} sized)")


def test_arg_sizes_are_dword_multiples():
    """stdcall cleanup pops whole dwords; an odd size corrupts the stack."""
    with open(BRIDGE_C, encoding="utf-8", errors="replace") as fh:
        src = fh.read()
    m = re.search(r"static int stdcall_args_for_ordinal.*?\n\}", src, re.S)
    assert m, "could not locate stdcall_args_for_ordinal"
    bad = [
        f"ordinal {o} pops {b} bytes"
        for o, b in re.findall(r"case\s+(\d+):\s*return\s+(\d+);", m.group(0))
        if int(b) % 4
    ]
    assert not bad, "\n  ".join(bad)
    print("ok  arg_sizes_are_dword_multiples")


def load_data_exports():
    """(ordinal, KDATA slot) for every entry in kernel_data_va_for_ordinal."""
    src = open(BRIDGE_C, encoding="utf-8", errors="replace").read()
    m = re.search(r"kernel_data_va_for_ordinal.*?\n\}", src, re.S)
    assert m, "could not locate kernel_data_va_for_ordinal"
    return [(int(o), k) for o, k in re.findall(
        r"case\s+(\d+):\s*return\s+XBOX_KERNEL_DATA_BASE\s*\+\s*(KDATA_[A-Z_0-9]+)",
        m.group(0))]


# Which export each KDATA slot is the storage for. The slot names are ours, so
# the mapping cannot be derived from the export table and has to be stated.
KDATA_OWNER = {
    "KDATA_EVENT_OBJ_TYPE": "ExEventObjectType",
    "KDATA_MUTANT_OBJ_TYPE": "ExMutantObjectType",
    "KDATA_SEMAPHORE_OBJ_TYPE": "ExSemaphoreObjectType",
    "KDATA_TIMER_OBJ_TYPE": "ExTimerObjectType",
    "KDATA_DISK_MODEL_STR": "HalDiskModelNumber",
    "KDATA_DISK_SERIAL_STR": "HalDiskSerialNumber",
    "KDATA_DISK_CACHE_PARTS": "HalDiskCachePartitionCount",
    "KDATA_IO_COMPLETION_TYPE": "IoCompletionObjectType",
    "KDATA_IO_DEVICE_TYPE": "IoDeviceObjectType",
    "KDATA_FILE_OBJ_TYPE": "IoFileObjectType",
    "KDATA_TICK_COUNT": "KeTickCount",
    "KDATA_TIME_INCREMENT": "KeTimeIncrement",
    "KDATA_LAUNCH_DATA_PAGE": "LaunchDataPage",
    "KDATA_THREAD_OBJ_TYPE": "PsThreadObjectType",
    "KDATA_HARDWARE_INFO": "XboxHardwareInfo",
    "KDATA_HD_KEY": "XboxHDKey",
    "KDATA_KRNL_VERSION": "XboxKrnlVersion",
    "KDATA_SIGNATURE_KEY": "XboxSignatureKey",
    "KDATA_XE_IMAGE_FILENAME": "XeImageFileName",
    "KDATA_LAN_KEY": "XboxLANKey",
    "KDATA_ALT_SIGNATURE_KEYS": "XboxAlternateSignatureKeys",
    "KDATA_XE_PUBLIC_KEY": "XePublicKeyData",
    "KDATA_BOOT_SMC_VIDEO": "HalBootSMCVideoMode",
    "KDATA_IDEX_CHANNEL": "IdexChannelObject",
}


def test_data_exports_match_their_ordinal():
    """kernel_data_va_for_ordinal is consulted BEFORE function routing, so a
    wrong ordinal here does not merely mis-read a variable -- it hands the title
    a data address for a real kernel function, which it then calls. A whole
    block of this table had shifted: 17 (ExFreePool), 65 (IoCreateDevice), 327
    (XeLoadSection) and 328 (XeUnloadSection) were all being turned into data
    pointers, and the real exports at 16/353/354/355/356/357 got no thunk."""
    exports = load_exports()
    entries = load_data_exports()
    assert entries, "no data-export entries found"
    bad = []
    for ordinal, slot in entries:
        actual = exports.get(ordinal)
        owner = KDATA_OWNER.get(slot)
        if actual is None:
            bad.append(f"ordinal {ordinal} -> {slot}, but no such export")
        elif owner is None:
            bad.append(f"{slot} has no entry in KDATA_OWNER (add one)")
        elif owner != actual:
            bad.append(f"ordinal {ordinal} -> {slot} ({owner}), but ordinal "
                       f"{ordinal} is {actual}")
    assert not bad, "misrouted kernel data exports:\n  " + "\n  ".join(bad)
    print(f"ok  data_exports_match_their_ordinal ({len(entries)} exports)")


def test_no_data_export_ordinal_is_also_a_function_route():
    """The two tables must not overlap: data wins at thunk-build time, so an
    ordinal in both is a function the title can never actually call."""
    data = {o for o, _ in load_data_exports()}
    both = sorted(data & {o for o, _ in load_routes()})
    assert not both, ("ordinals routed as BOTH data and function: %s" % both)
    print("ok  no_data_export_ordinal_is_also_a_function_route")

if __name__ == "__main__":
    # Discovered rather than listed: this file has been appended to before, and
    # a test defined after a hand-written call list is silently never run.
    _tests = [v for k, v in sorted(globals().items())
              if k.startswith("test_") and callable(v)]
    for _t in _tests:
        _t()
    print("all passed (%d checks)" % len(_tests))
