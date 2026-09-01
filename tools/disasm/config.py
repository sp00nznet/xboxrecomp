"""
Configuration constants for Burnout 3 disassembly tool.

Defines address ranges, section boundaries, instruction classification,
and other constants used throughout the disassembler.
"""

# ============================================================
# XBE Memory Layout
# ============================================================

XBE_BASE_ADDRESS = 0x00010000
XBE_IMAGE_SIZE = 7766528  # 0x768800

# Entry point (retail, XOR-decoded)
ENTRY_POINT = 0x001D2807

# Kernel thunk table start (in .rdata)
KERNEL_THUNK_ADDR = 0x0036B7C0

# ============================================================
# Section Definitions
# ============================================================

# Executable code sections (name, va_start, va_size)
# These are the sections we'll disassemble
EXECUTABLE_SECTIONS = [
    (".text",   0x00011000, 2863616),
    ("XMV",     0x002CC200, 163124),
    ("DSOUND",  0x002F3F40, 52668),
    ("WMADEC",  0x00300D00, 105828),
    ("XONLINE", 0x0031AA80, 124764),
    ("XNET",    0x003391E0, 78056),
    ("D3D",     0x0034C2E0, 83828),
    ("XGRPH",   0x00360A60, 8300),
    ("XPP",     0x00362AE0, 36052),
    ("DOLBY",   0x0076B940, 29056),
]

# Data sections
DATA_SECTIONS = [
    (".rdata",  0x0036B7C0, 289684),
    (".data",   0x003B2360, 3904988),
    ("XON_RD",  0x00772AC0, 5416),
    (".data1",  0x00774000, 224),
]

# All sections by name for quick lookup
ALL_SECTIONS = {
    s[0]: {"va": s[1], "size": s[2]}
    for s in EXECUTABLE_SECTIONS + DATA_SECTIONS
}

# ============================================================
# Instruction Classification
# ============================================================

# x86 control flow instructions (mnemonic sets)
CALL_MNEMONICS = {"call"}
RET_MNEMONICS = {"ret", "retn", "retf"}
JMP_MNEMONICS = {"jmp"}
COND_JMP_MNEMONICS = {
    "jo", "jno", "jb", "jnb", "jnae", "jae", "jc", "jnc",
    "jz", "je", "jnz", "jne", "jbe", "jna", "ja", "jnbe",
    "js", "jns", "jp", "jpe", "jnp", "jpo",
    "jl", "jnge", "jge", "jnl", "jle", "jng", "jg", "jnle",
    "jcxz", "jecxz",
    "loop", "loope", "loopz", "loopne", "loopnz",
}
BRANCH_MNEMONICS = JMP_MNEMONICS | COND_JMP_MNEMONICS

# NOP-like instructions
NOP_MNEMONICS = {"nop"}

# Instructions that terminate a basic block
TERMINATOR_MNEMONICS = RET_MNEMONICS | JMP_MNEMONICS | COND_JMP_MNEMONICS

# ============================================================
# Function Detection
# ============================================================

# Standard MSVC x86 function prologue patterns (byte sequences)
# push ebp; mov ebp, esp
PROLOGUE_PUSH_EBP_MOV = bytes([0x55, 0x8B, 0xEC])
# push ebp; mov ebp, esp (with rex/other encoding)
PROLOGUE_PUSH_EBP_MOV_ALT = bytes([0x55, 0x89, 0xE5])

# CC padding byte (int 3 / debug break)
CC_PADDING = 0xCC

# Minimum CC padding run length to consider as function boundary
MIN_CC_RUN = 1

# ============================================================
# Function Detection Confidence Scores
# ============================================================

# Alignment required before decode_at will manufacture an instruction at a
# direct call target the linear sweep stepped over (see functions.py
# _pass_call_targets, engine.py decode_at).
#
# Only applies to targets with no decoded instruction. Realigning there is
# creating evidence rather than reading it, so it demands corroboration; a
# target that already decoded is accepted as before, whatever its alignment.
#
# 16 because that is what MSVC emits for a function start, and because two
# independent measurements landed on it: seeding unaligned indirect-branch
# targets made Halo 2276 crash earlier (see tools/recomp/icall_feedback.py
# cmd_seeds), and on Steel Battalion LOC only 61 of 103 realigned targets were
# 16-aligned while the rest carried the same signature as that garbage --
# misdecoded call operands inside data, not functions.
#
# Set to 1 to disable the check.
CALL_TARGET_REALIGN_ALIGNMENT = 16

CONFIDENCE_KNOWN = 1.0       # Entry point, known addresses
CONFIDENCE_PROLOGUE = 0.95   # Standard prologue pattern
CONFIDENCE_CALL_TARGET = 0.90  # Destination of a call instruction
CONFIDENCE_TAIL_JUMP = 0.88   # Target of a jmp that leaves its function
CONFIDENCE_CC_BOUNDARY = 0.85  # After CC padding run following ret
CONFIDENCE_CODE_POINTER = 0.80  # Code address stored as an immediate

# ============================================================
# Disassembly Engine Settings
# ============================================================

# Chunk size for linear sweep (64 KB)
SWEEP_CHUNK_SIZE = 0x10000

# x86-32 mode
CS_MODE = 32

# ============================================================
# Output Settings
# ============================================================

# Default output directory (relative to tool root)
DEFAULT_OUTPUT_DIR = "tools/disasm/output"

# Maximum string length to extract from .rdata
MAX_STRING_LENGTH = 256

# Minimum string length to consider valid
MIN_STRING_LENGTH = 4

# ============================================================
# Cache Settings
# ============================================================

CACHE_FILENAME = ".disasm_cache.json"
CACHE_VERSION = 1
