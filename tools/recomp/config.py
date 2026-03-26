"""
Recompiler configuration - section mappings and constants.

NOTE: These mappings must match the target XBE. Currently configured for
Xbox Dashboard build 3944 (xboxdash.xbe).
"""

# Section virtual address → file offset mappings
# (name, va_start, va_size, raw_addr, raw_size)
SECTIONS = [
    # (name, va_start, va_size, raw_addr)
    (".text",      0x00012000, 636684,  0x00002000),
    ("D3D",        0x000AD720, 72432,   0x0009E000),
    ("D3DX",       0x000BF220, 153536,  0x000AD000),
    ("XGRPH",      0x000E49E0, 7624,    0x000D3000),
    ("DSOUND",     0x000E67C0, 31344,   0x000D5000),
    ("WMADECXM",   0x000EE240, 4912,    0x000DD000),
    ("WMADEC",     0x000EF580, 101720,  0x000DF000),
    ("DVDTHUNK",   0x001082E0, 1116,    0x000F8000),
    ("XPP",        0x00108740, 31436,   0x000F9000),
    (".data",      0x00110220, 140392,  0x00101000),
    ("DOLBY",      0x001326A0, 28056,   0x00113000),
    (".data1",     0x00139440, 7536,    0x0011A000),
    ("XIPS",       0x0013B1C0, 20760,   0x0011C000),
    ("EnglishXlate",  0x001402E0, 32428, 0x00122000),
    ("JapaneseXlate", 0x001481A0, 27052, 0x0012A000),
    ("GermanXlate",   0x0014EB60, 34148, 0x00131000),
    ("FrenchXlate",   0x001570E0, 34772, 0x0013A000),
    ("SpanishXlate",  0x0015F8C0, 34798, 0x00143000),
    ("ItalianXlate",  0x001680C0, 34164, 0x0014C000),
]

TEXT_VA_START = 0x00012000
TEXT_VA_END = 0x000AD720   # end of .text = start of D3D section
RDATA_VA_START = 0x00110220  # .data section (dashboard has no .rdata)
RDATA_VA_END = 0x00170000
DATA_VA_START = 0x00110220
DATA_VA_END = 0x00170000
KERNEL_THUNK_ADDR = 0x00012000
ENTRY_POINT = 0x00052A81


def va_to_file_offset(va):
    """Convert virtual address to XBE file offset."""
    for _, sec_va, sec_size, sec_raw in SECTIONS:
        if sec_va <= va < sec_va + sec_size:
            return va - sec_va + sec_raw
    return None


def is_code_address(va):
    """Check if VA is in an executable section (.text or XDK library sections)."""
    if TEXT_VA_START <= va < TEXT_VA_END:
        return True
    # XDK library sections also contain executable code.
    # XIPS contains the XAP parser and IS code despite XBE marking it non-executable.
    for name, sec_va, sec_size, _ in SECTIONS:
        if name in (".data", ".data1", "EnglishXlate", "JapaneseXlate",
                     "GermanXlate", "FrenchXlate", "SpanishXlate", "ItalianXlate"):
            continue  # skip data/resource sections (but NOT XIPS — it's code)
        if sec_va <= va < sec_va + sec_size:
            return True
    return False


def is_data_address(va):
    """Check if VA is in a data section."""
    return DATA_VA_START <= va <= DATA_VA_END
