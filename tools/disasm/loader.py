"""
Binary image loader for XBE files.

Loads the raw XBE binary and its analysis JSON, providing a unified
BinaryImage interface for the disassembly engine.
"""

import json
import struct
from pathlib import Path
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

# Sections that hold data even when the XBE marks them executable. Excluded
# from disassembly by name; --extra-sections overrides this per title.
#
# Only the conventional PE data sections are listed. XDK library sections are
# NOT excluded even when their name suggests data (DSOUND_RD, D3D_RD, XON_RD):
# the linker maps class those as CODE, and they are exactly the sections we
# want disassembled.
DATA_SECTION_NAMES = frozenset({
    ".data", ".data1", ".rdata", ".idata", ".edata", ".reloc", ".tls",
})


# The longest x86 instruction the decoder accepts. tools/recomp/config.py
# keeps its own copy for the lifter's readable end; the two must agree.
MAX_INSN_LEN = 15


@dataclass
class SectionInfo:
    """Parsed section metadata."""
    name: str
    virtual_addr: int
    virtual_size: int
    raw_addr: int
    raw_size: int
    writable: bool
    executable: bool
    flags: str


@dataclass
class KernelImport:
    """A kernel import thunk entry."""
    ordinal: int
    name: str
    thunk_addr: int


@dataclass
class BinaryImage:
    """
    Represents the loaded XBE binary with all metadata.

    Provides methods for reading bytes at virtual addresses,
    translating between VA and file offsets, and querying sections.
    """
    filepath: str
    raw_data: bytes
    base_address: int
    image_size: int
    entry_point: int
    kernel_thunk_addr: int

    sections: List[SectionInfo] = field(default_factory=list)
    kernel_imports: List[KernelImport] = field(default_factory=list)

    # Lookup tables built after loading
    _section_by_name: Dict[str, SectionInfo] = field(default_factory=dict, repr=False)
    _thunk_to_import: Dict[int, KernelImport] = field(default_factory=dict, repr=False)

    def __post_init__(self):
        self._section_by_name = {s.name: s for s in self.sections}
        self._thunk_to_import = {ki.thunk_addr: ki for ki in self.kernel_imports}

    def get_section(self, name: str) -> Optional[SectionInfo]:
        """Get section by name."""
        return self._section_by_name.get(name)

    def get_section_at_va(self, va: int) -> Optional[SectionInfo]:
        """Find the section containing a virtual address."""
        for sec in self.sections:
            if sec.virtual_addr <= va < sec.virtual_addr + sec.virtual_size:
                return sec
        return None

    def va_to_file_offset(self, va: int) -> Optional[int]:
        """Convert virtual address to file offset."""
        # Header area
        if va < self.base_address + 0x1000:
            off = va - self.base_address
            if 0 <= off < len(self.raw_data):
                return off
            return None

        # Search sections
        for sec in self.sections:
            if sec.virtual_addr <= va < sec.virtual_addr + sec.virtual_size:
                offset_in_sec = va - sec.virtual_addr
                if offset_in_sec < sec.raw_size:
                    return sec.raw_addr + offset_in_sec
                # In BSS (beyond raw data)
                return None
        return None

    def read_bytes_at_va(self, va: int, size: int) -> Optional[bytes]:
        """Read bytes at a virtual address."""
        off = self.va_to_file_offset(va)
        if off is None:
            return None
        end = off + size
        if end > len(self.raw_data):
            return None
        return self.raw_data[off:end]

    def read_u32_at_va(self, va: int) -> Optional[int]:
        """Read a 32-bit unsigned integer at a virtual address."""
        data = self.read_bytes_at_va(va, 4)
        if data is None:
            return None
        return struct.unpack_from('<I', data)[0]

    def get_section_data(self, section: SectionInfo) -> bytes:
        """Get the bytes for a section, as the loader maps them.

        The XBE loader zero-fills a section from raw_size up to virtual_size,
        and linkers rely on it: Spartan's D3DX section (XDK 5849) has a raw
        size that stops one byte short of its last instruction, the
        `ret 0x10` of D3DXMatrixLookAtLH, whose immediate's high byte is that
        zero fill. Cut at raw_size, the ret fails to decode, the function
        ends at `leave`, and the translated body returns without popping its
        return address and arguments -- the caller's callee-saved registers
        then come back as stack garbage.

        Code sections get just enough of the fill to finish an instruction
        that starts in the file bytes (an x86 instruction is at most 15
        bytes); more would hand the sweeps zeros to decode as code.
        """
        data = self.raw_data[section.raw_addr:section.raw_addr + section.raw_size]
        # A BSS-only section (no file bytes) has nothing to finish.
        if (section.executable and section.raw_size > 0
                and section.virtual_size > section.raw_size):
            data += bytes(min(section.virtual_size - section.raw_size,
                              MAX_INSN_LEN - 1))
        return data

    def get_kernel_import_at_thunk(self, thunk_addr: int) -> Optional[KernelImport]:
        """Look up a kernel import by its thunk address."""
        return self._thunk_to_import.get(thunk_addr)

    def get_executable_sections(self) -> List[SectionInfo]:
        """Return all executable sections."""
        return [s for s in self.sections if s.executable]

    def get_code_sections(self) -> List[SectionInfo]:
        """Return sections suitable for disassembly.

        The executable flag alone is not a usable signal: Xbox linkers mark
        nearly every section executable, including .data. Disassembling those
        yields phantom functions built out of zero-fill -- runs of 00 00 decode
        as `add [eax], al`, and the lifter happily emits C for them (one such
        phantom referenced a nonexistent `cr7` and broke the build).

        So data sections are excluded by name regardless of their flags. A
        title that genuinely hides code in one of them can put it back with
        --extra-sections, which is exactly what that flag is for.
        """
        return [s for s in self.sections
                if s.executable and s.raw_size > 0
                and s.name not in DATA_SECTION_NAMES]


def _parse_hex(s: str) -> int:
    """Parse a hex string like '0x00011000' to int."""
    return int(s, 16)


def _find_analysis_json(xbe_path: Path) -> Optional[Path]:
    """Auto-detect the analysis JSON for this XBE.

    Written by ``tools.xbe_parser --json``, so the name is per-game rather than
    hardcoded to one title.

    The exact ``<stem>_analysis.json`` always wins. A bare ``*_analysis.json``
    glob is only accepted when the directory holds a single XBE: a disc that
    ships one binary per region keeps them side by side -- Wreckless has seven
    in one folder -- and taking the first sorted match there hands DSTEAL_JP.xbe
    the analysis written for default.xbe. Different entry point, different
    section layout, no warning. Data from the wrong binary is worse than none,
    because everything downstream still runs.
    """
    search_dirs = [
        # Same directory as XBE
        xbe_path.parent,
        # In the xbe_parser tool directory
        Path("tools/xbe_parser"),
        # Relative to repo root
        xbe_path.parent.parent / "tools" / "xbe_parser",
    ]
    for d in search_dirs:
        exact = d / (xbe_path.stem + "_analysis.json")
        if exact.exists():
            return exact
    for d in search_dirs:
        try:
            ambiguous = len(list(d.glob("*.xbe"))) > 1
        except OSError:
            continue
        if ambiguous:
            continue
        # sorted() so the pick is deterministic when a dir holds several
        for p in sorted(d.glob("*_analysis.json")):
            return p
    return None


def load_image(xbe_path: str, analysis_json: Optional[str] = None) -> BinaryImage:
    """
    Load an XBE binary and its analysis metadata.

    Args:
        xbe_path: Path to the .xbe file.
        analysis_json: Optional path to burnout3_analysis.json.
                       If None, auto-detected from standard locations.

    Returns:
        A fully initialized BinaryImage instance.
    """
    xbe_file = Path(xbe_path)
    if not xbe_file.exists():
        raise FileNotFoundError(f"XBE file not found: {xbe_path}")

    # Read raw binary
    raw_data = xbe_file.read_bytes()

    # Validate XBE magic
    if raw_data[:4] != b'XBEH':
        raise ValueError(f"Invalid XBE magic: {raw_data[:4]!r}")

    # Find and load analysis JSON
    json_path = Path(analysis_json) if analysis_json else _find_analysis_json(xbe_file)
    if json_path is None or not json_path.exists():
        # Name the file it wants and the command that writes it. The old
        # message said only "run the XBE parser first", which left the reader
        # to guess both the filename and where it goes -- one did, passed
        # `--json JSON`, and then had to pass `--analysis-json JSON` here to
        # get past it. Auto-detection needs the exact name.
        wanted = xbe_file.parent / (xbe_file.stem + "_analysis.json")
        raise FileNotFoundError(
            f"Analysis JSON not found: {wanted}\n"
            f"  Write it with:  py -3 -m tools.xbe_parser {xbe_file} "
            f"--json {wanted}\n"
            f"  The name matters -- this step looks for "
            f"<xbe stem>_analysis.json beside the XBE. A file somewhere else, "
            f"or under another name, needs --analysis-json <path>."
        )

    with open(json_path) as f:
        analysis = json.load(f)

    # Parse sections
    sections = []
    for sec_data in analysis["sections"]:
        sections.append(SectionInfo(
            name=sec_data["name"],
            virtual_addr=_parse_hex(sec_data["virtual_addr"]),
            virtual_size=sec_data["virtual_size"],
            raw_addr=_parse_hex(sec_data["raw_addr"]),
            raw_size=sec_data["raw_size"],
            writable=sec_data["writable"],
            executable=sec_data["executable"],
            flags=sec_data["flags"],
        ))

    # Parse kernel imports
    kernel_imports = []
    for imp_data in analysis["kernel_imports"]:
        kernel_imports.append(KernelImport(
            ordinal=imp_data["ordinal"],
            name=imp_data["name"],
            thunk_addr=_parse_hex(imp_data["thunk_addr"]),
        ))

    return BinaryImage(
        filepath=str(xbe_file),
        raw_data=raw_data,
        base_address=_parse_hex(analysis["base_address"]),
        image_size=analysis["image_size"],
        entry_point=_parse_hex(analysis["entry_point"]),
        kernel_thunk_addr=_parse_hex(analysis["kernel_thunk_addr"]),
        sections=sections,
        kernel_imports=kernel_imports,
    )
