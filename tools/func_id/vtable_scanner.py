"""
C++ vtable scanner for function classification.

Scans data sections (.rdata, .data, etc.) for arrays of consecutive
.text pointers that form C++ virtual function tables. Each vtable
corresponds to a class, and its entries are virtual methods.

Key feature: discovers THUNK FUNCTIONS that are only called via vtable
ICALLs. These are valid code entry points between `ret` instructions
that the function detector misses because there's no direct `call`.

Typical MSVC vtable layout in .rdata:
  [vfunc_0_ptr][vfunc_1_ptr][vfunc_2_ptr]...

Thunk pattern (after ret of previous function):
  mov ecx, [ecx+N]   ; adjust this pointer
  test ecx, ecx       ; null check
  jne target           ; jump to real method
  xor eax, eax
  ret N
"""

import struct
from collections import defaultdict

from . import config


# Minimum number of consecutive code pointers to qualify as a vtable
MIN_VTABLE_ENTRIES = 3


def scan_vtables(xbe_data, functions, imm_refs, verbose=False, sections=None):
    """
    Scan data sections for C++ vtables and classify virtual methods.

    Also discovers thunk functions: valid code entry points that are
    referenced by vtables but not in the known function list.

    Args:
        xbe_data: Raw XBE file bytes.
        functions: List of function dicts from disassembly.
        imm_refs: Dict of immediate reference data.
        verbose: Print progress info.
        sections: Optional list of section dicts with keys:
            name, va (int), size (int), raw (int), executable (bool)
            If None, falls back to config-based section detection.

    Returns:
        tuple: (vtable_results, vtables)
    """
    # Build set of known function starts
    func_starts = set()
    for f in functions:
        func_starts.add(int(f["start"], 16))

    # Determine code and data ranges from section info
    if sections:
        # Code ranges: only the main .text section (where lifted functions live)
        text_ranges = [(s["va"], s["va"] + s["size"])
                       for s in sections if s["name"] == ".text"]
        # Scan ALL sections with raw data for vtables.
        # Xbox vtables can be in ANY section (including .text itself!)
        data_sections = [{"name": s["name"], "va": s["va"],
                          "size": min(s["size"], s.get("raw_size", s["size"])),
                          "raw": s["raw"]}
                         for s in sections
                         if s["raw"] > 0 and s.get("raw_size", s["size"]) > 0]
    else:
        text_ranges = _get_code_ranges()
        data_sections = _get_data_sections()

    # Scan all data sections for vtable structures
    raw_vtables = _find_vtables(xbe_data, func_starts, text_ranges, data_sections)

    if verbose:
        print(f"  Raw vtable candidates: {len(raw_vtables)}")

    # Filter false positives
    vtables = _filter_vtables(raw_vtables)

    if verbose:
        total_entries = sum(len(vt["entries"]) for vt in vtables)
        print(f"  Validated vtables: {len(vtables)}")
        print(f"  Total virtual methods: {total_entries}")

    # Find thunks: vtable entries not in func_starts
    discovered_thunks = set()
    for vt in vtables:
        for entry in vt["entries"]:
            if entry not in func_starts:
                discovered_thunks.add(entry)

    if verbose and discovered_thunks:
        print(f"  Discovered vtable thunks (new functions): {len(discovered_thunks)}")
        for addr in sorted(discovered_thunks)[:10]:
            print(f"    0x{addr:08X}")
        if len(discovered_thunks) > 10:
            print(f"    ... and {len(discovered_thunks) - 10} more")

    # Find constructors
    constructors = _find_constructors(vtables, xbe_data, functions)

    if verbose:
        print(f"  Constructors found: {len(constructors)}")

    # Build classification results
    results = {}
    for i, vt in enumerate(vtables):
        cls_id = f"cls_{i:03d}"
        vt["class_id"] = cls_id

        for idx, entry_addr in enumerate(vt["entries"]):
            if entry_addr not in results:
                is_thunk = entry_addr in discovered_thunks
                results[entry_addr] = {
                    "category": "game_vtable",
                    "subcategory": cls_id,
                    "confidence": 0.75 if is_thunk else 0.85,
                    "method": "vtable_thunk" if is_thunk else "vtable_scan",
                    "vtable_addr": vt["address"],
                    "vtable_index": idx,
                }

    # Add constructors
    for func_addr, vt_info in constructors.items():
        if func_addr not in results:
            results[func_addr] = {
                "category": "game_vtable",
                "subcategory": vt_info["class_id"],
                "confidence": 0.80,
                "method": "vtable_ctor",
                "vtable_addr": vt_info["vtable_addr"],
                "vtable_index": -1,
            }

    if verbose:
        print(f"  Functions classified by vtable: {len(results)}")

    return results, vtables


def _get_code_ranges():
    """Get all code section VA ranges from config."""
    ranges = [(config.TEXT_VA_START, config.TEXT_VA_END)]

    # Add any other code sections defined in config
    if hasattr(config, 'SECTIONS'):
        skip_names = {".data", ".data1", "XIPS", "EnglishXlate", "JapaneseXlate",
                      "GermanXlate", "FrenchXlate", "SpanishXlate", "ItalianXlate"}
        for name, va, size, _ in config.SECTIONS:
            if name not in skip_names and name != ".text":
                ranges.append((va, va + size))

    return ranges


def _get_data_sections():
    """Get all data sections to scan for vtables."""
    sections = []

    # Primary scan range: .rdata or .data
    if hasattr(config, 'RDATA_RAW_ADDR'):
        sections.append({
            "name": "rdata",
            "va": config.RDATA_VA_START,
            "size": config.RDATA_VA_SIZE,
            "raw": config.RDATA_RAW_ADDR,
        })

    # Also scan all data sections from SECTIONS list
    if hasattr(config, 'SECTIONS'):
        data_names = {".data", ".data1", "DOLBY"}
        for name, va, size, raw in config.SECTIONS:
            if name in data_names:
                sections.append({
                    "name": name,
                    "va": va,
                    "size": size,
                    "raw": raw,
                })

    return sections


def _is_code_address(va, text_ranges):
    """Check if VA falls within the .text section and looks like a valid function addr."""
    # Reject obviously non-function addresses (misaligned or too small)
    if va < 0x10000:
        return False
    for lo, hi in text_ranges:
        if lo <= va < hi:
            return True
    return False


def _find_vtables(xbe_data, func_starts, text_ranges, data_sections):
    """
    Scan data sections for sequences of consecutive code pointers.

    Accepts any VA in a code section range as a potential vtable entry,
    not just known func_starts. This discovers thunk functions.
    """
    vtables = []

    for section in data_sections:
        sec_raw = section["raw"]
        sec_va = section["va"]
        sec_size = section["size"]

        if sec_raw + sec_size > len(xbe_data):
            sec_size = len(xbe_data) - sec_raw
        if sec_size <= 0:
            continue

        sec_bytes = xbe_data[sec_raw:sec_raw + sec_size]

        i = 0
        while i < len(sec_bytes) - 4:
            # Align to 4 bytes
            if i % 4 != 0:
                i += 4 - (i % 4)
                continue

            val = struct.unpack_from('<I', sec_bytes, i)[0]

            # Check if this looks like a code pointer
            if not _is_code_address(val, text_ranges):
                i += 4
                continue

            # Found a code pointer - scan forward for more
            entries = []
            j = i
            while j < len(sec_bytes) - 4:
                val = struct.unpack_from('<I', sec_bytes, j)[0]
                if _is_code_address(val, text_ranges):
                    entries.append(val)
                    j += 4
                else:
                    break

            if len(entries) >= MIN_VTABLE_ENTRIES:
                vtables.append({
                    "address": sec_va + i,
                    "entries": entries,
                    "section": section["name"],
                })
                i = j
            else:
                i += 4

    return vtables


def _filter_vtables(vtables):
    """
    Filter out false positive vtable candidates.
    """
    filtered = []

    for vt in vtables:
        entries = vt["entries"]

        # Filter: all entries identical
        if len(set(entries)) == 1:
            continue

        # Filter: arithmetic progression (likely data table)
        if len(entries) >= 4:
            diffs = [entries[j+1] - entries[j] for j in range(len(entries) - 1)]
            if len(set(diffs)) == 1 and diffs[0] <= 16:
                continue

        # Filter: mostly sequential small-increment (data table)
        if len(entries) >= 6:
            small_diffs = sum(1 for j in range(len(entries) - 1)
                            if abs(entries[j+1] - entries[j]) <= 8)
            if small_diffs > len(entries) * 0.8:
                continue

        filtered.append(vt)

    return filtered


def _find_constructors(vtables, xbe_data, functions):
    """
    Find constructor functions that embed vtable addresses as immediates.
    """
    vtable_addrs = {}
    vtable_methods = set()
    for vt in vtables:
        addr_bytes = struct.pack('<I', vt["address"])
        vtable_addrs[addr_bytes] = vt
        vtable_methods.update(vt["entries"])

    if not vtable_addrs:
        return {}

    constructors = {}

    for f in functions:
        func_addr = int(f["start"], 16)
        func_size = f.get("size", 0)
        if func_size < 8 or func_size > 8192:
            continue

        file_offset = config.va_to_file_offset(func_addr)
        if file_offset is None or file_offset + func_size > len(xbe_data):
            continue

        func_bytes = xbe_data[file_offset:file_offset + func_size]

        for addr_bytes, vt in vtable_addrs.items():
            pos = func_bytes.find(addr_bytes)
            if pos >= 0:
                if func_addr not in vtable_methods:
                    constructors[func_addr] = {
                        "class_id": vt.get("class_id", "cls_???"),
                        "vtable_addr": vt["address"],
                    }
                break

    return constructors
