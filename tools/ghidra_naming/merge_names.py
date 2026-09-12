#!/usr/bin/env python3
"""
Merge exported names into a clean {address: name} map for the recompiler.

Reads the JSONs produced by Ghidra's ExportXbeNames.py, or by IDA's
tools/ida_naming/export_xbe_names.py, which writes the same two files:
  <export-dir>/functions.json
  <export-dir>/symbols.json

Produces:
  tools/ghidra_naming/ghidra_names.json   -> { "0x00352560": "name", ... }

Only MEANINGFUL names are kept. Auto-generated placeholders are excluded:
  FUN_*, LAB_*, DAT_*, SUB_*, UNK_*, EXT_*, OFF_*, thunk_FUN_*, switchD_*,
  caseD_*, j_* (jump thunks), and entirely-numeric / empty names.
Each kept name is sanitized to a valid C identifier (alnum + underscore, no
leading digit). Collisions are de-duplicated by appending _<addr>. Names that
collide with C/C++ keywords are suffixed with _<addr> too.

The script classifies each recovered name by SOURCE for reporting:
  fidb/library  - named by Function ID / Library Identification (IMPORTED/ANALYSIS
                  function symbol whose name is not a placeholder)
  rtti          - class/vftable/RTTI-derived names (namespace or RTTI markers)
  demangled     - names that look demangled (contain :: or were demangled)
  symbol        - any other user/imported/analysis symbol with a real name

--apply (OPTIONAL, NOT run by default): updates
  tools/disasm/output/functions.json in place (writes a .bak first), setting the
  `name` field for entries whose `start` address matches a recovered name.

Usage:
  py -3 tools/ghidra_naming/merge_names.py
  py -3 tools/ghidra_naming/merge_names.py --apply       # (do not run unless asked)
"""
import argparse
import json
import os
import re
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.normpath(os.path.join(_HERE, "..", ".."))

DEFAULT_EXPORT_DIR = os.path.join(_HERE, "export")
DEFAULT_OUT = os.path.join(_HERE, "ghidra_names.json")
DEFAULT_FUNCTIONS_JSON = os.path.join(_REPO, "tools", "disasm", "output",
                                      "functions.json")

# Ghidra default/placeholder name prefixes (case-insensitive prefix match).
# Auto-generated for unnamed code/data; never meaningful function names.
PLACEHOLDER_PREFIXES = (
    "FUN_",          # unnamed function
    "LAB_",          # code label
    "DAT_",          # unnamed data
    "SUB_",          # legacy unnamed sub
    "UNK_",          # unknown
    "EXT_",          # external
    "OFF_",          # offset reference
    "PTR_",          # pointer data label
    "JUMPTABLE_",
    "DATA_",
    "SWITCHD_",      # switchD_<addr>::caseD_n (jump table)
    "SWITCHDATAD_",  # switchdataD_<addr> (jump table data)
    "CASED_",
    "FID_",          # FunctionID conflict / hash marker
    "BYTE_", "WORD_", "DWORD_", "QWORD_", "UINT_", "INT_",
    "FLOAT_", "DOUBLE_", "BOOL_", "CHAR_", "UNICODE_",
    # IDA's own, for tools/ida_naming/export_xbe_names.py. Its autonames do
    # not all overlap Ghidra's: loc_ is IDA's LAB_, and jpt_/algn_/asc_/stru_
    # have no Ghidra equivalent at all. Without these an IDA export merges
    # thousands of loc_1A2B3C-shaped non-names into the recompiler.
    "LOC_", "NULLSUB_", "JPT_", "ALGN_", "ASC_", "STRU_",
    "XMMWORD_", "YMMWORD_", "TBYTE_", "FLT_", "DBL_", "PACKREAL_",
)
# Exact placeholder names (no address suffix) that must be dropped.
PLACEHOLDER_EXACT = {
    "DEFAULT", "SWITCHD", "SWITCHDATAD", "CASED", "JUMPTABLE",
}
# Names that are placeholders even with a leading thunk/j marker.
PLACEHOLDER_CONTAINS = (
    "_FUN_",   # thunk_FUN_00xxxxxx
    "_CASED_", # switchD_xxxx::caseD_n
)
# Ghidra data-label conventions: <p>_<text>_<8 hex>. We match these precisely so
# we don't accidentally drop a real function/symbol that merely starts with the
# same letter. Examples: s_Hello_00abc123, u_wide_00abc123, a_arr_00abc123,
# default_00014c20, switchdataD_00014c50.
DATA_LABEL_RE = re.compile(
    r"^(?:s|u|a|default|switchd|switchdatad|cased)_.*_[0-9a-fA-F]{6,8}$",
    re.IGNORECASE,
)

# C / C++ keywords we must not emit as identifiers.
C_KEYWORDS = set("""
auto break case char const continue default do double else enum extern float
for goto if inline int long register restrict return short signed sizeof static
struct switch typedef union unsigned void volatile while _Bool _Complex
_Imaginary bool true false class new delete this namespace template typename
operator public private protected virtual friend using try catch throw
""".split())

# Standard library identifiers we must not emit either.
#
# Recovered names are real names, and a title's own CRT routines are genuinely
# called strchr, memcpy, sqrt and so on. Generated code includes <string.h>,
# <math.h> and <stdio.h>, so emitting "void strchr(void);" collides with the
# real declaration and the translation unit will not compile. Porting names out
# of a linker MAP surfaces hundreds of these at once.
#
# Anything here gets the same _<addr> suffix as a keyword collision.
C_STDLIB = set("""
memcpy memmove memset memcmp memchr
strcpy strncpy strcat strncat strcmp strncmp strcoll strxfrm strchr strrchr
strspn strcspn strpbrk strstr strtok strlen strnlen strerror strdup
sprintf snprintf vsprintf vsnprintf sscanf printf fprintf vfprintf scanf
fopen fclose fread fwrite fseek ftell rewind feof ferror fflush fgets fputs
fgetc fputc getc putc ungetc setvbuf setbuf remove rename tmpfile
malloc calloc realloc free abort exit atexit system getenv
abs labs div ldiv rand srand qsort bsearch atoi atol atof strtol strtoul strtod
sqrt sqrtf pow powf exp expf log logf log10 sin sinf cos cosf tan tanf
asin acos atan atan2 sinh cosh tanh ceil ceilf floor floorf fabs fabsf
fmod fmodf frexp ldexp modf hypot
isalnum isalpha iscntrl isdigit isgraph islower isprint ispunct isspace
isupper isxdigit tolower toupper
time clock difftime mktime localtime gmtime asctime ctime strftime
setjmp longjmp signal raise assert
main
nextafter nextafterf nexttoward copysign round roundf trunc truncf rint cbrt
log2 log1p expm1 asinh acosh atanh erf erfc lgamma tgamma fmin fmax fma
""".split()) | set("""
wcscpy wcsncpy wcscat wcsncat wcscmp wcsncmp wcscoll wcsxfrm wcschr wcsrchr
wcsspn wcscspn wcspbrk wcsstr wcstok wcslen wcsnlen wcsdup wcserror
wcsicmp wcsnicmp wcslwr wcsupr wcsrev wcsset wcsnset wcstol wcstoul wcstod
wmemcpy wmemmove wmemset wmemcmp wmemchr
mbstowcs wcstombs mbtowc wctomb btowc wctob
swprintf vswprintf swscanf wprintf fwprintf wscanf
iswalnum iswalpha iswdigit iswspace iswupper iswlower towlower towupper
""".split()) | set("""
_errno errno _iob _fileno _isnan _finite _hypot _strdup _stricmp _strnicmp
_strlwr _strupr _itoa _ltoa _ultoa _fltused _CIsqrt _CIpow _CIlog _CIexp
strnicmp stricmp strcmpi stricoll strlwr strupr strrev strset strnset
itoa ltoa ultoa ecvt fcvt gcvt swab
logb logbf scalb scalbn ilogb significand drem j0 j1 jn y0 y1 yn gamma
_ftol _ftol2 _alldiv _aulldiv _allmul _allrem _aullrem _allshl _allshr _aullshr
""".split())

RESERVED = C_KEYWORDS | C_STDLIB

HEX_RE = re.compile(r"^0x[0-9A-Fa-f]+$")


def norm_addr(a):
    """Normalize an address string to '0x%08X'."""
    if isinstance(a, int):
        return "0x%08X" % a
    s = str(a).strip()
    try:
        if s.lower().startswith("0x"):
            v = int(s, 16)
        else:
            v = int(s, 16)  # functions.json uses hex strings without/with 0x
        return "0x%08X" % v
    except ValueError:
        return s


def is_placeholder(name):
    if not name:
        return True
    up = name.upper()
    if up in PLACEHOLDER_EXACT:
        return True
    for p in PLACEHOLDER_PREFIXES:
        if up.startswith(p):
            return True
    for c in PLACEHOLDER_CONTAINS:
        if c in up:
            return True
    # Ghidra data-label conventions (string/unicode/array/jumptable labels).
    if DATA_LABEL_RE.match(name):
        return True
    # Pure hex / pure digits.
    if HEX_RE.match(name):
        return True
    if name.isdigit():
        return True
    return False


def sanitize(name):
    """Turn a (possibly demangled/mangled) symbol into a valid C identifier."""
    if name is None:
        return None
    # Drop common Ghidra decorations.
    n = name.strip()
    # Templated / namespaced C++: keep the leaf-ish readable bits.
    # Replace namespace separators and any non [A-Za-z0-9_] with '_'.
    n = n.replace("::", "_")
    n = re.sub(r"[^A-Za-z0-9_]", "_", n)
    # Collapse runs of underscores.
    n = re.sub(r"_+", "_", n)
    n = n.strip("_")
    if not n:
        return None
    # No leading digit.
    if n[0].isdigit():
        n = "_" + n
    # Bound length (some demangled names are enormous).
    if len(n) > 100:
        n = n[:100].rstrip("_")
    return n


def classify(name, sym_type, namespace, source):
    """Bucket a recovered name by likely source for reporting."""
    if "::" in (name or "") or (namespace and namespace not in ("", "Global")):
        # RTTI/class members tend to carry a namespace.
        if namespace and ("RTTI" in namespace or "class" in namespace.lower()):
            return "rtti"
        if "::" in (name or ""):
            return "demangled"
        return "rtti"
    st = (sym_type or "").lower()
    src = (source or "").upper()
    if st == "function" and src in ("IMPORTED", "ANALYSIS"):
        # FunctionID / Library Identification assign function names via analysis.
        return "fidb/library"
    return "symbol"


def load_json(path):
    if not os.path.exists(path):
        return None
    with open(path, "r") as f:
        return json.load(f)


def build_map(export_dir):
    funcs = load_json(os.path.join(export_dir, "functions.json"))
    syms = load_json(os.path.join(export_dir, "symbols.json"))

    if funcs is None and syms is None:
        print("ERROR: neither functions.json nor symbols.json found in %s"
              % export_dir, file=sys.stderr)
        sys.exit(2)

    # addr -> (raw_name, source_bucket). Prefer function names over generic
    # symbols; prefer non-placeholder; first writer with a bucket wins by
    # priority order below.
    chosen = {}          # addr -> dict(name, bucket, raw)
    bucket_counts = {}

    def consider(addr, raw_name, sym_type, namespace, source):
        if is_placeholder(raw_name):
            return
        clean = sanitize(raw_name)
        if not clean:
            return
        bucket = classify(raw_name, sym_type, namespace, source)
        prev = chosen.get(addr)
        # Priority: fidb/library > demangled > rtti > symbol.
        prio = {"fidb/library": 0, "demangled": 1, "rtti": 2, "symbol": 3}
        if prev is None or prio[bucket] < prio[prev["bucket"]]:
            chosen[addr] = {"name": clean, "bucket": bucket, "raw": raw_name}

    # Functions first (these are the prime targets for the recompiler).
    if funcs:
        for f in funcs:
            addr = norm_addr(f.get("address"))
            # IDA's exporter sets `source` to IMPORTED for a FLIRT-identified
            # library function, which is the same claim Ghidra's FidDb makes
            # through the symbol table. Ghidra's function export has no such
            # field, so it keeps the old behaviour.
            consider(addr, f.get("name"), "Function",
                     f.get("namespace", ""), f.get("source") or "ANALYSIS")

    # Then symbols (covers labels promoted to data/functions, RTTI, demangled).
    if syms:
        for s in syms:
            # Only consider symbols at function-relevant granularity; we still
            # include data symbols because recompiler keys purely on address and
            # a meaningful data name is better than sub_. But to avoid flooding,
            # only take primary symbols.
            if not s.get("primary", True):
                continue
            addr = norm_addr(s.get("address"))
            consider(addr, s.get("name"), s.get("type"),
                     s.get("namespace", ""), s.get("source"))

    # Resolve identifier collisions: same final name on different addrs.
    seen_names = {}
    final = {}
    for addr in sorted(chosen.keys()):
        info = chosen[addr]
        nm = info["name"]
        if nm in RESERVED:
            nm = "%s_%s" % (nm, addr[2:])  # append addr without 0x
        if nm in seen_names and seen_names[nm] != addr:
            nm = "%s_%s" % (nm, addr[2:])
        seen_names[nm] = addr
        final[addr] = nm
        bucket_counts[info["bucket"]] = bucket_counts.get(info["bucket"], 0) + 1

    return final, bucket_counts, chosen


def write_map(final, out_path):
    # Stable, address-sorted output.
    ordered = {a: final[a] for a in sorted(final.keys())}
    with open(out_path, "w") as f:
        json.dump(ordered, f, indent=1)
    return len(ordered)


def apply_to_functions_json(final, functions_json_path):
    if not os.path.exists(functions_json_path):
        print("ERROR: %s not found; cannot --apply" % functions_json_path,
              file=sys.stderr)
        sys.exit(2)
    with open(functions_json_path, "r") as f:
        data = json.load(f)

    bak = functions_json_path + ".bak"
    if not os.path.exists(bak):
        with open(bak, "w") as f:
            json.dump(data, f, indent=2)
        print("Wrote backup: %s" % bak)
    else:
        print("Backup already exists (kept): %s" % bak)

    applied = 0
    for entry in data:
        start = norm_addr(entry.get("start"))
        if start in final:
            newname = final[start]
            if entry.get("name") != newname:
                entry["name"] = newname
                applied += 1
    with open(functions_json_path, "w") as f:
        json.dump(data, f, indent=2)
    print("Applied %d names to %s" % (applied, functions_json_path))
    return applied


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--export-dir", default=DEFAULT_EXPORT_DIR,
                    help="Dir with Ghidra functions.json/symbols.json")
    ap.add_argument("--out", default=DEFAULT_OUT,
                    help="Output ghidra_names.json path")
    ap.add_argument("--functions-json", default=DEFAULT_FUNCTIONS_JSON,
                    help="Recompiler functions.json (for --apply)")
    ap.add_argument("--apply", action="store_true",
                    help="Apply names into functions.json in place (.bak first)")
    ap.add_argument("--names-json",
                    help="Skip the Ghidra export and take names from an existing "
                         "{addr: name} file, e.g. the output of "
                         "tools/symbols/map_names.py. Names are still sanitized "
                         "and de-duplicated the same way.")
    args = ap.parse_args()

    if args.names_json:
        raw = load_json(args.names_json) or {}
        final, buckets = {}, {}
        seen = {}
        for addr, name in raw.items():
            a = norm_addr(addr)
            clean = sanitize(name)
            if not clean or is_placeholder(name):
                continue
            # Same reserved-word rule as build_map. A linker MAP hands back the
            # title's real CRT routine names (strchr, memcpy, sqrt), which
            # collide with the declarations the generated code already gets
            # from <string.h>/<math.h>.
            if clean in RESERVED:
                clean = "%s_%s" % (clean, a[2:])
            # Same de-dup rule as build_map: distinct addresses must not
            # collapse onto one C identifier.
            if clean in seen:
                clean = "%s_%s" % (clean, a[2:])
            seen[clean] = a
            final[a] = clean
            buckets["symbol"] = buckets.get("symbol", 0) + 1
        print("loaded %d names from %s" % (len(final), args.names_json))
    else:
        final, buckets, _chosen = build_map(args.export_dir)
    n = write_map(final, args.out)

    # How many of these addresses actually exist in the recompiler's functions.json?
    matched_in_recomp = None
    if os.path.exists(args.functions_json):
        with open(args.functions_json, "r") as f:
            recomp = json.load(f)
        recomp_starts = set(norm_addr(e.get("start")) for e in recomp)
        matched_in_recomp = sum(1 for a in final if a in recomp_starts)

    print("=" * 60)
    print("ghidra_names.json written: %s" % args.out)
    print("Total meaningful names: %d" % n)
    print("By source:")
    for b in ("fidb/library", "demangled", "rtti", "symbol"):
        print("  %-14s %d" % (b, buckets.get(b, 0)))
    if matched_in_recomp is not None:
        print("Addresses matching recompiler functions.json `start`: %d / %d"
              % (matched_in_recomp, n))
    print("=" * 60)

    if args.apply:
        apply_to_functions_json(final, args.functions_json)
    else:
        print("(--apply NOT set: functions.json left unchanged)")


if __name__ == "__main__":
    main()
