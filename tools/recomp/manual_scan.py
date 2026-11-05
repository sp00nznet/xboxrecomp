"""
What recomp_manual.c defines.

Single source of truth, deliberately. Both the recompiler (deciding what NOT to
generate) and gen_dangling_stubs.py (deciding what still needs a stub) have to
agree on what counts as a definition. When they each had their own regex they
disagreed, and every disagreement is a link error: a definition missed by one
becomes "already defined", a definition invented by the other becomes
"unresolved external".
"""

import json
import os
import re
import sys

# A definition is "void sub_XXXXXXXX(void)" followed by a body -- on the same
# line (`void sub_X(void) { esp += 4; return; }`) or the next. A declaration
# ends in ';' and must not match.
DEF_RE = re.compile(r"^void (\w*?(?:sub_)?([0-9A-Fa-f]{8}))\(void\)\s*(?:\{|$)", re.M)
_SUB_DEF_RE = re.compile(r"^void (sub_([0-9A-Fa-f]{8}))\(void\)\s*(?:\{|$)", re.M)
# recomp_manual.c wraps rather than replaces some functions: it defines sub_X
# itself and calls the generated body as sub_X_gen.
_WRAP_RE = re.compile(r"^extern void (sub_([0-9A-Fa-f]{8}))_gen\(void\);", re.M)


def strip_disabled(src):
    """Remove '#if 0 ... #endif' regions.

    recomp_manual.c keeps disabled overrides around as documentation. A
    definition inside one is not a definition; counting it makes the
    recompiler skip a function that then nothing defines.
    """
    out, depth = [], 0
    for line in src.splitlines(keepends=True):
        s = line.strip()
        if re.match(r"^#if\s+0\b", s):
            depth += 1
        elif depth and re.match(r"^#if(def|ndef)?\b", s):
            depth += 1  # nested conditional inside a disabled block
        elif depth and s.startswith("#endif"):
            depth -= 1
        elif depth == 0:
            out.append(line)
    return "".join(out)


# ── Port names (--names) ──────────────────────────────────────────────────
#
# A port may write its hand-written side by name instead of by address, so
# one source builds for every release of its game: XBR_FN(n) for the
# function at n, XBR_GEN(n) for its generated body, XBR_VA(n) for the
# address, and `#if XBR_HAVE(n)` around code a release may lack. The game's
# CLI writes gen/recomp_names.h with the macros for the dump being built and
# hands recomp the same names as --names; the scan resolves the forms to
# the address form first, so every rule below reads what the compiler will.
#
# NAMES is {name: va} once recomp read --names, else None: the forms are then
# left alone, and a manual file that uses them stops (it could not link).
# HAVE is every name the release has, values included (a struct offset has
# no address but its XBR_HAVE is 1 in the header): the XBR_HAVE blocks are
# read against it, so recomp keeps the same side the compiler does.
NAMES = None
HAVE = None

_FORM_RE = re.compile(r"\bXBR_(FN|GEN|VA)\(\s*(\w+)\s*\)")
_ANY_FORM_RE = re.compile(r"\bXBR_(?:FN|GEN|VA|HAVE)\(")
_HAVE_IF_RE = re.compile(r"^#\s*if\s+XBR_HAVE\(\s*(\w+)\s*\)\s*(?:/[*/].*)?$")
_COND_OPEN_RE = re.compile(r"^#\s*if(?:def|ndef)?\b")
_COND_ELSE_RE = re.compile(r"^#\s*(else|elif)\b")
_COND_END_RE = re.compile(r"^#\s*endif\b")


def load_names_have(path):
    """The --names file: ({name: va}, {every name the release has}). Either
    {"addresses": {name: va}, "have": [name, ...]} or, the plain form, a JSON
    object {name: va} whose names are all it has; va an int or a hex string."""
    with open(path, encoding="utf-8") as f:
        raw = json.load(f)
    if not isinstance(raw, dict):
        raise SystemExit(f"ERROR: --names {path}: not a JSON object of name: address")
    have = None
    if "addresses" in raw:
        have = raw.get("have", [])
        raw = raw["addresses"]
        if not isinstance(raw, dict) or not isinstance(have, list) or not all(
                isinstance(n, str) for n in have):
            raise SystemExit(f"ERROR: --names {path}: addresses must be an object "
                             f"and have a list of names")
    out = {}
    for name, va in raw.items():
        try:
            out[name] = int(va, 0) if isinstance(va, str) else int(va)
        except (TypeError, ValueError):
            raise SystemExit(f"ERROR: --names {path}: {name}: {va!r} is not an address")
    have = set(out) if have is None else set(have) | set(out)
    return out, have


def load_names(path):
    """The --names file's {name: va} (load_names_have)."""
    return load_names_have(path)[0]


def strip_missing(src, names):
    """Drop the `#if XBR_HAVE(n)` side a release without n does not build
    (the #if side when n is missing, else its #else side), with the
    directive lines themselves. Other conditionals are kept as they are."""
    out = []
    stack = []  # per open conditional: [keep] for an XBR_HAVE one, else None

    def live():
        return all(f is None or f[0] for f in stack)

    for line in src.splitlines(keepends=True):
        s = line.strip()
        m = _HAVE_IF_RE.match(s)
        if m:
            stack.append([m.group(1) in names])
            continue
        if _COND_OPEN_RE.match(s):
            stack.append(None)
        elif _COND_ELSE_RE.match(s) and stack and stack[-1] is not None:
            if _COND_ELSE_RE.match(s).group(1) == "elif":
                raise SystemExit("ERROR: an #if XBR_HAVE(name) block takes #else, not #elif")
            stack[-1][0] = not stack[-1][0]
            continue
        elif _COND_END_RE.match(s) and stack:
            if stack.pop() is not None:
                continue
        if live():
            out.append(line)
    return "".join(out)


def resolve_names(src, names, path="the manual file"):
    """src with every XBR_FN/XBR_GEN/XBR_VA form in the address form. A form
    in code (not a comment or string) whose name the release lacks stops
    recomp, naming each: the build would otherwise fail at link, or read
    the wrong memory."""
    code = _COMMENT_OR_STRING_RE.sub(" ", src)
    missing = sorted({m.group(2) for m in _FORM_RE.finditer(code)} - set(names))
    if missing:
        raise SystemExit(
            f"ERROR: {path} uses {len(missing)} name{'s' if len(missing) != 1 else ''} "
            f"this release's name map lacks: {', '.join(missing)}. Place "
            f"{'them' if len(missing) != 1 else 'it'} in the release's map, or put "
            f"the code that needs {'them' if len(missing) != 1 else 'it'} under "
            f"#if XBR_HAVE(name).")

    def sub(m):
        # A comment may still name one the release lacks: left as written.
        if m.group(2) not in names:
            return m.group(0)
        va = names[m.group(2)]
        return {"FN": f"sub_{va:08X}", "GEN": f"sub_{va:08X}_gen",
                "VA": f"0x{va:08X}u"}[m.group(1)]

    return _FORM_RE.sub(sub, src)


def _live(path):
    """path as the compiler sees it: #if 0 regions out, then the port names
    resolved (NAMES)."""
    live = strip_disabled(open(path, encoding="utf-8", errors="replace").read())
    if NAMES is not None:
        have = HAVE if HAVE is not None else NAMES
        return resolve_names(strip_missing(live, have), NAMES, path)
    if _ANY_FORM_RE.search(_COMMENT_OR_STRING_RE.sub(" ", live)):
        raise SystemExit(
            f"ERROR: {path} refers to functions by port name (XBR_FN, XBR_GEN, "
            f"XBR_VA, XBR_HAVE), and recomp got no --names map to resolve them")
    return live


def definition_names(path):
    """Names of functions actually defined (compiled) in `path`."""
    if not os.path.exists(path):
        return set()
    live = _live(path)
    return {m.group(1) for m in _SUB_DEF_RE.finditer(live)}


_REF_RE = re.compile(r"\bsub_([0-9A-Fa-f]{8})\b")

# An entry hook: `void sub_XXXXXXXX_enter(void)` defined by hand is called at
# the top of the generated sub_XXXXXXXX, before its first instruction. Unlike
# a _gen wrapper it is reached from direct callers too, and the function's own
# code stays generated: the way to trace, or hold, a function that is called
# directly without writing its body out by hand.
_ENTER_RE = re.compile(r"^void sub_([0-9A-Fa-f]{8})_enter\(void\)\s*(?:\{|$)", re.M)


def entry_hooks(path):
    """Addresses of functions the manual file defines an entry hook for."""
    if not os.path.exists(path):
        return set()
    live = _live(path)
    return {int(m.group(1), 16) for m in _ENTER_RE.finditer(live)}


# Comments and string literals, for code_references: the scan's
# `referenced` counts them on purpose (a name kept in a comment still pins
# sub_), but a reference the compiler sees is only in the code.
_COMMENT_OR_STRING_RE = re.compile(
    r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)


def code_references(path):
    """Addresses of the sub_XXXXXXXX names the manual file's code (not its
    comments or strings, nor #if 0 blocks) mentions."""
    if not os.path.exists(path):
        return set()
    live = _live(path)
    code = _COMMENT_OR_STRING_RE.sub(" ", live)
    return {int(m.group(1), 16) for m in _REF_RE.finditer(code)}


# Words a use may follow (return sub_X(), else sub_X()); any other word
# or a '*' before the name makes it a declarator (void sub_X(void);).
_USE_AFTER_WORDS = {"return", "else", "do", "case", "sizeof"}


# A preprocessor directive with its continuation lines: a macro body is
# no use until something expands it, and code_uses does not expand.
_DIRECTIVE_RE = re.compile(r"^[ \t]*#(?:[^\n]*\\\n)*[^\n]*", re.M)


def code_uses(path):
    """Addresses of the sub_XXXXXXXX names the manual file's function
    bodies use, so that the linker must resolve them: a call (sub_X()) or
    the name as a value (RECOMP_ABI_CALL(va, sub_X), = sub_X). Only inside
    braces: at file scope a name is a declarator (an attribute or a second
    declarator in the list makes it hard to tell from a use, and both
    link). Inside, a name after a type or a '*' is a local declaration.
    Comments, strings, directives (macro bodies) and #if 0 blocks aside.
    A file-scope initializer (= sub_X) is not counted: code_references
    still warns about it."""
    if not os.path.exists(path):
        return set()
    live = _live(path)
    code = _DIRECTIVE_RE.sub(" ", _COMMENT_OR_STRING_RE.sub(" ", live))
    out = set()
    depth, at = 0, 0
    for m in _REF_RE.finditer(code):
        chunk = code[at:m.start()]
        depth = max(0, depth + chunk.count("{") - chunk.count("}"))
        at = m.start()
        if depth == 0:
            continue
        before = code[:m.start()].rstrip()
        word = re.search(r"(\w+)$", before)
        if before.endswith("*") or (word and word.group(1) not in _USE_AFTER_WORDS):
            continue
        out.add(int(m.group(1), 16))
    return out


def scan(path):
    """(skip, wrap, referenced) for functions the manual file handles.

    skip:       defined by hand -- gen must declare but not define them.
    wrap:       manual calls the generated body as sub_X_gen -- gen must still
                emit it, renamed.
    referenced: every sub_XXXXXXXX the hand-written code mentions at all. Those
                must keep the sub_ name in the generated output: recomp_manual.c
                declares and calls sub_00350C10, so if a naming pass renamed it
                to SwapCopy_D3D_..., gen defines that and nothing defines
                sub_00350C10. Correctness beats a prettier name.
    """
    if not os.path.exists(path):
        print(f"WARNING: {path} not found; excluding nothing.", file=sys.stderr)
        return set(), set(), set()
    live = _live(path)
    skip = {int(m.group(2), 16) for m in _SUB_DEF_RE.finditer(live)}
    wrap = {int(m.group(2), 16) for m in _WRAP_RE.finditer(live)}
    referenced = {int(m.group(1), 16) for m in _REF_RE.finditer(live)}
    return skip, wrap, referenced
