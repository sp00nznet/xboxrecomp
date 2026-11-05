"""
What recomp_manual.c defines.

Single source of truth, deliberately. Both the recompiler (deciding what NOT to
generate) and gen_dangling_stubs.py (deciding what still needs a stub) have to
agree on what counts as a definition. When they each had their own regex they
disagreed, and every disagreement is a link error: a definition missed by one
becomes "already defined", a definition invented by the other becomes
"unresolved external".
"""

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


def definition_names(path):
    """Names of functions actually defined (compiled) in `path`."""
    if not os.path.exists(path):
        return set()
    live = strip_disabled(open(path, encoding="utf-8", errors="replace").read())
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
    live = strip_disabled(open(path, encoding="utf-8", errors="replace").read())
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
    live = strip_disabled(open(path, encoding="utf-8", errors="replace").read())
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
    live = strip_disabled(open(path, encoding="utf-8", errors="replace").read())
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
    live = strip_disabled(open(path, encoding="utf-8", errors="replace").read())
    skip = {int(m.group(2), 16) for m in _SUB_DEF_RE.finditer(live)}
    wrap = {int(m.group(2), 16) for m in _WRAP_RE.finditer(live)}
    referenced = {int(m.group(1), 16) for m in _REF_RE.finditer(live)}
    return skip, wrap, referenced
