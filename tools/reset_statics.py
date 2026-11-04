#!/usr/bin/env python3
"""reset_statics.py - every piece of C state a game reset has to know about.

The in-window reset (the game-reset design, D5) puts every module
back to its start state. Its audit, docs/reset-statics.md, classifies each
piece of mutable state in the tree as keep, reset or constant. This script
finds that state by reading the sources, so the list cannot fall behind:

  file_static   a `static` object at file scope
  local_static  a `static` object inside a function
  tls           a thread-local object (XBOX_THREAD_LOCAL, RECOMP_TLS,
                __thread, _Thread_local, thread_local, __declspec(thread))
  once          an init-once guard (INIT_ONCE, pthread_once_t,
                dispatch_once_t, or an int-like flag named *_once, *init,
                *inited, *initialized, *init_done, *ready, *started,
                *registered or *installed)
  global        a non-static object defined at file scope

An object that is const itself (`static const int`, `char *const p`) cannot
hold run state and is skipped; a pointer to const (`const char *p`) is not.

    reset_statics.py check [--doc DOC] [--diff REV] [ROOT [PATH...]]
        Fail (exit 1) on every object missing from the doc. With --diff,
        only the files `git diff --name-only REV` names under ROOT.
    reset_statics.py list [ROOT [PATH...]]
        Print every object found, one tab-separated line each.
    reset_statics.py skeleton [--doc DOC] [ROOT [PATH...]]
        Print doc table rows (class `?`) for the objects the doc lacks.

ROOT defaults to the toolkit (this file's parent's parent) and PATH to its
`src`. A game passes its own root, its paths and `--doc`. The doc's rows
are Markdown table rows whose first five cells are
`file | scope | name | kind | class`: file relative to ROOT, scope the
enclosing function (`-` at file scope), class one of keep, reset or
constant. Line numbers are not part of the key, so a move is not a change.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

C_SUFFIXES = {".c", ".h", ".m", ".mm", ".cpp", ".cc"}
CLASSES = {"keep", "reset", "constant"}
KINDS = {"file_static", "local_static", "tls", "once", "global"}

TLS_WORDS = {"XBOX_THREAD_LOCAL", "RECOMP_TLS", "__thread", "_Thread_local",
             "thread_local"}
ONCE_TYPES = {"INIT_ONCE", "pthread_once_t", "dispatch_once_t", "RTL_RUN_ONCE"}
ONCE_NAME = re.compile(r"(?:^|_)(?:once|init|inited|initiali[sz]ed|init_done|did_init"
                       r"|ready|started|registered|installed)$",
                       re.IGNORECASE)
IDENT = re.compile(r"[A-Za-z_]\w*")
# Words that are never a declared name.
KEYWORDS = {
    "static", "extern", "const", "volatile", "register", "inline", "__inline",
    "__inline__", "__forceinline", "unsigned", "signed", "int", "char",
    "short", "long", "float", "double", "void", "struct", "union", "enum",
    "typedef", "restrict", "__restrict", "_Atomic", "_Alignas", "alignas",
    "__declspec", "__attribute__", "auto", "bool", "_Bool", "sizeof",
    "return", "if", "else", "for", "while", "do", "switch", "case", "goto",
    "break", "continue", "default", "__unused", "__strong", "__weak",
    "__block", "NSString",
} | TLS_WORDS
# Statement heads at file scope that are not object definitions.
NOT_OBJECT_HEADS = ("typedef", "extern", "return", "using", "namespace",
                    "template", "@", "static_assert", "_Static_assert")


@dataclass(frozen=True)
class Item:
    file: str
    scope: str
    name: str
    kind: str
    line: int

    @property
    def key(self) -> tuple[str, str, str]:
        return (self.file, self.scope, self.name)


def strip_source(text: str) -> str:
    """Comments, string and char literals and preprocessor lines blanked,
    newlines kept so offsets still map to lines."""
    out = []
    i, n = 0, len(text)
    at_line_start = True
    while i < n:
        c = text[i]
        if at_line_start:
            j = i
            while j < n and text[j] in " \t":
                j += 1
            if j < n and text[j] == "#":
                # A directive, with its continuations.
                while j < n:
                    if text[j] == "\n":
                        if text[j - 1] == "\\":
                            out.append("\n")
                            j += 1
                            continue
                        break
                    if text[j] == "/" and text[j + 1:j + 2] == "*":
                        end = text.find("*/", j + 2)
                        end = n if end < 0 else end + 2
                        out.append("\n" * text.count("\n", j, end))
                        j = end
                        continue
                    j += 1
                i = j
                at_line_start = False
                continue
        if c == "/" and text[i + 1:i + 2] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "/" and text[i + 1:i + 2] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(" " + "\n" * text.count("\n", i, j))
            i = j
            continue
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                if text[j] == "\\":
                    j += 1
                elif text[j] == "\n":
                    break
                j += 1
            out.append(c + c)
            i = j + 1
            continue
        out.append(c)
        at_line_start = c == "\n"
        i += 1
    return "".join(out)


def split_top(s: str, sep: str) -> list[str]:
    """s split at sep outside (), [] and {}."""
    parts, depth, cur = [], 0, []
    for ch in s:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == sep and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    return parts


def drop_groups(s: str, open_ch: str, close_ch: str) -> str:
    out, depth = [], 0
    for ch in s:
        if ch == open_ch:
            depth += 1
            continue
        if ch == close_ch:
            depth = max(0, depth - 1)
            continue
        if depth == 0:
            out.append(ch)
    return "".join(out)


def strip_attrs(s: str) -> str:
    """__attribute__((...)), __declspec(...) (thread kept as a word),
    alignas(...) and macro-like calls in upper case, dropped."""
    s = re.sub(r"__declspec\s*\(\s*thread\s*\)", " __thread ", s)
    for word in ("__attribute__", "__declspec", "_Alignas", "alignas",
                 "__asm__", "__asm", "asm"):
        while True:
            m = re.search(r"\b" + word + r"\s*\(", s)
            if not m:
                break
            depth, j = 0, m.end() - 1
            while j < len(s):
                if s[j] == "(":
                    depth += 1
                elif s[j] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            s = s[:m.start()] + " " + s[j + 1:]
    return s


def declarator_name(decl: str) -> str | None:
    """The declared name of one declarator (`*const s_tab[4]`,
    `(*s_fn)(int)`), or None for a function declarator."""
    decl = drop_groups(decl, "[", "]")
    m = re.match(r"[\s*&]*\(\s*[*^&]\s*(?:const\s+|volatile\s+)*([A-Za-z_]\w*)\s*\)", decl)
    if m:
        return m.group(1)                       # a pointer to function
    if "(" in decl:
        return None                             # a function (or a macro call)
    names = [w for w in IDENT.findall(decl) if w not in KEYWORDS]
    return names[-1] if names else None


def object_is_const(head: str, decl: str) -> bool:
    """The object itself is const: `const` after the last `*`, or in the
    specifiers with no `*`. A pointer to const is not."""
    full = head + " " + drop_groups(decl, "[", "]")
    if "*" in full:
        return bool(re.search(r"\*[^*]*\bconst\b[^*]*$", full.split("=")[0]))
    return bool(re.search(r"\bconst\b|\bconstexpr\b", head))


def classify_statement(stmt: str, at_file_scope: bool) -> list[tuple[str, str]]:
    """(name, kind) for each object stmt defines; [] when it is not one."""
    s = strip_attrs(" ".join(stmt.split()))
    if not s:
        return []
    words = IDENT.findall(s)
    if not words:
        return []
    if s.startswith(NOT_OBJECT_HEADS) or words[0] in ("typedef", "extern"):
        return []
    is_static = "static" in words[:6]
    is_tls = any(w in TLS_WORDS for w in words[:8])
    if not at_file_scope and not is_static and not is_tls:
        return []
    if at_file_scope and not is_static and not is_tls:
        # A non-static definition: a type and at least one declarator.
        if words[0] in ("struct", "union", "enum") and "=" not in s \
                and len(split_top(s, ",")) == 1 and len(words) <= 2:
            return []
    # Separate the specifiers from the declarators: the first declarator
    # starts at the first name that is followed by [ = , ; ( or the end.
    body = split_top(s, "=")[0] if "=" in s else s
    pieces = split_top(s, ",")
    first = pieces[0]
    first_lhs = split_top(first, "=")[0]
    # Struct or enum bodies in the specifiers: the declarators follow }.
    if "{" in first_lhs:
        tail = first_lhs[first_lhs.rfind("}") + 1:]
        head = first_lhs[:first_lhs.find("{")]
        decls = [tail] + [split_top(p, "=")[0] for p in pieces[1:]]
    else:
        tokens = list(re.finditer(r"[A-Za-z_]\w*|\S", first_lhs))
        # The head is every token up to the last identifier that is not a
        # keyword and is followed by a declarator character or the end.
        head_end = 0
        for idx, t in enumerate(tokens):
            if t.group() not in KEYWORDS and IDENT.fullmatch(t.group()):
                nxt = tokens[idx + 1].group() if idx + 1 < len(tokens) else ""
                after = tokens[idx + 2].group() if idx + 2 < len(tokens) else ""
                # `uint64_t (*s_fn)(void)`: a type before a pointer
                # declarator, not a name.
                if nxt == "(" and after in ("*", "^"):
                    head_end = t.end()
                    continue
                if nxt in ("", "[", "=", ",", ";", ")", ":", "("):
                    break
            head_end = t.end()
        # Move leading '*' / '(' of the declarator out of the head.
        head = first_lhs[:head_end]
        hm = re.match(r"^(.*?)([\s*&^(]*(?:const\s*|volatile\s*)*[\s*&^(]*)$", head, re.S)
        head, star = (hm.group(1), hm.group(2)) if hm else (head, "")
        decls = [star + first_lhs[head_end:]] + [split_top(p, "=")[0] for p in pieces[1:]]
    del body
    head_words = set(IDENT.findall(head))
    if not head_words - {"static", "inline", "const", "volatile"} - TLS_WORDS \
            and not is_tls:
        return []                               # `static x;` is not C here
    out = []
    for d in decls:
        name = declarator_name(d)
        if not name or name in KEYWORDS:
            continue
        if object_is_const(head, d):
            continue
        if is_tls:
            kind = "tls"
        elif head_words & ONCE_TYPES or (ONCE_NAME.search(name)
                                         and re.search(r"\b(int|LONG|bool|_Bool|BOOL|char|uint\d+_t|int\d+_t)\b", head)):
            kind = "once"
        elif not is_static:
            kind = "global"
        elif at_file_scope:
            kind = "file_static"
        else:
            kind = "local_static"
        out.append((name, kind))
    return out


FUNC_NAME = re.compile(r"([A-Za-z_]\w*)\s*\([^()]*(?:\([^()]*\)[^()]*)*\)\s*"
                       r"(?:const\s*)?(?:__attribute__\s*\(\(.*?\)\)\s*)*$", re.S)


def function_name(header: str) -> str | None:
    """The function a `{` at file scope opens, from the text before it."""
    h = strip_attrs(" ".join(header.split()))
    if "=" in split_top(h, "=")[0] and "(" not in h:
        return None
    # A function returning a pointer to an array or a function,
    # `char (*f(int s))[64]`: the name sits inside the parentheses.
    pm = re.search(r"\(\s*\*\s*([A-Za-z_]\w*)\s*\([^()]*(?:\([^()]*\)[^()]*)*\)\s*\)"
                   r"\s*(?:\[[^\]]*\]\s*)+$", h)
    if pm and "=" not in h[:pm.start()]:
        return pm.group(1)
    m = FUNC_NAME.search(h)
    if not m:
        # Objective-C methods: `- (void)foo:(int)x {` and `@implementation`.
        om = re.match(r"^[-+]\s*\([^)]*\)\s*([A-Za-z_]\w*)", h)
        return om.group(1) if om else None
    name = m.group(1)
    if name in ("if", "while", "for", "switch", "sizeof", "__attribute__"):
        return None
    if "=" in h[:m.start()]:
        return None                             # an initializer with a call
    return name


def scan_text(text: str, rel: str) -> list[Item]:
    src = strip_source(text)
    items: list[Item] = []
    seen: set[tuple[str, str, str]] = set()

    def add(scope: str, name: str, kind: str, off: int) -> None:
        key = (rel, scope, name)
        if key in seen:
            return
        seen.add(key)
        items.append(Item(rel, scope, name, kind, src.count("\n", 0, off) + 1))

    i, n = 0, len(src)
    stmt_start = 0
    while i < n:
        c = src[i]
        if c == ";":
            stmt = src[stmt_start:i]
            for name, kind in classify_statement(stmt, True):
                add("-", name, kind, stmt_start + len(stmt) - len(stmt.lstrip()))
            i += 1
            stmt_start = i
            continue
        if c == "}":
            # A stray close (an extern "C" block's end).
            i += 1
            stmt_start = i
            continue
        if c == "{":
            header = src[stmt_start:i]
            hs = " ".join(header.split())
            if re.fullmatch(r'extern\s*""', hs) or hs.startswith("extern \"\"") \
                    or hs in ("@implementation", "") or hs.startswith(("@implementation", "@interface", "@protocol")):
                if hs.startswith(("@interface", "@protocol")):
                    i = skip_braces(src, i)
                    stmt_start = i
                    continue
                i += 1
                stmt_start = i
                continue
            fname = function_name(header)
            if fname and not re.search(r"\b(struct|union|enum)\b\s*\w*\s*$", hs) \
                    and not hs.rstrip().endswith("="):
                end = skip_braces(src, i)
                scan_body(src, i + 1, end - 1, fname, add)
                i = end
                stmt_start = i
                continue
            if re.search(r"\)\s*(?:\[[^\]]*\]\s*)*$", hs) and "=" not in hs:
                # A body after a header this parser cannot name: say so,
                # since its statics would be missed, and start the next
                # statement after it rather than folding it into the
                # next header (which would lose every function after it).
                print(f"reset_statics: {rel}:{src.count(chr(10), 0, i) + 1}: "
                      "a function body with no name found; its statics are not "
                      "scanned", file=sys.stderr)
                i = skip_braces(src, i)
                stmt_start = i
                continue
            # A struct, union or enum body, or an initializer: part of the
            # statement.
            i = skip_braces(src, i)
            continue
        if c == "@" and (src.startswith("@end", i) or src.startswith("@implementation", i)):
            # Objective-C: neither ends in ; or opens a brace.
            j = src.find("\n", i)
            i = n if j < 0 else j
            stmt_start = i
            continue
        i += 1
    return items


def skip_braces(src: str, i: int) -> int:
    """Index just past the } that closes the { at i."""
    depth = 0
    n = len(src)
    while i < n:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def scan_body(src: str, lo: int, hi: int, fname: str, add) -> None:
    for m in re.finditer(r"\b(static|XBOX_THREAD_LOCAL|RECOMP_TLS|__thread|_Thread_local)\b",
                         src[lo:hi]):
        start = lo + m.start()
        # The statement start: the keyword must begin one.
        k = start - 1
        while k >= lo and src[k] in " \t\n":
            k -= 1
        if k >= lo and src[k] not in ";{}:)":
            continue
        if k >= lo and src[k] == ")":
            continue                            # a cast or `if (...) static`?
        # The statement runs to ; at this depth, past any initializer braces.
        j, depth = start, 0
        while j < hi:
            ch = src[j]
            if ch in "({[":
                depth += 1
            elif ch in ")}]":
                depth -= 1
            elif ch == ";" and depth <= 0:
                break
            j += 1
        for name, kind in classify_statement(src[start:j], False):
            add(fname, name, kind, start)


def iter_sources(root: Path, paths: list[str]) -> list[Path]:
    files: list[Path] = []
    for p in paths:
        base = root / p
        if base.is_file():
            files.append(base)
            continue
        for f in sorted(base.rglob("*")):
            if f.suffix in C_SUFFIXES and f.is_file():
                files.append(f)
    return files


def scan_tree(root: Path, paths: list[str], only: set[str] | None = None,
              exclude: tuple[str, ...] = ()) -> list[Item]:
    items: list[Item] = []
    for f in iter_sources(root, paths):
        rel = f.relative_to(root).as_posix()
        if only is not None and rel not in only:
            continue
        if any(rel.startswith(e) for e in exclude):
            continue
        items.extend(scan_text(f.read_text(errors="replace"), rel))
    return items


ROW = re.compile(r"^\|\s*`?([^|`]+?)`?\s*\|\s*`?([^|`]+?)`?\s*\|\s*`?([^|`]+?)`?\s*"
                 r"\|\s*`?([^|`]+?)`?\s*\|\s*`?([^|`]+?)`?\s*\|")


def read_doc(doc: Path) -> tuple[dict[tuple[str, str, str], str], list[str]]:
    """{(file, scope, name): class} from the doc's table rows, and errors."""
    rows: dict[tuple[str, str, str], str] = {}
    errors: list[str] = []
    for ln, line in enumerate(doc.read_text().splitlines(), 1):
        m = ROW.match(line.strip())
        if not m:
            continue
        file, scope, name, kind, cls = (g.strip() for g in m.groups())
        if file in ("file", "---") or set(file) <= set("-: "):
            continue
        if kind not in KINDS:
            continue                            # another table in the doc
        if cls not in CLASSES:
            errors.append(f"{doc.name}:{ln}: {file} {name}: class `{cls}` is not keep, reset or constant")
        rows[(file, scope, name)] = cls
    return rows, errors


def changed_files(root: Path, rev: str) -> set[str]:
    out = subprocess.run(["git", "-C", str(root), "diff", "--name-only", rev, "--", "."],
                         check=True, capture_output=True, text=True).stdout
    top = subprocess.run(["git", "-C", str(root), "rev-parse", "--show-prefix"],
                         check=True, capture_output=True, text=True).stdout.strip()
    files = set()
    for line in out.splitlines():
        rel = line[len(top):] if top and line.startswith(top) else line
        files.add(rel)
    return files


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("cmd", choices=("check", "list", "skeleton"))
    ap.add_argument("root", nargs="?", default=str(Path(__file__).resolve().parent.parent))
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--doc", help="the classification (default ROOT/docs/reset-statics.md)")
    ap.add_argument("--diff", metavar="REV", help="only the files changed since REV")
    ap.add_argument("--exclude", action="append", default=[],
                    help="a path prefix under ROOT to leave out (repeatable)")
    args = ap.parse_args(argv)

    root = Path(args.root).resolve()
    paths = args.paths or ["src"]
    only = changed_files(root, args.diff) if args.diff else None
    items = scan_tree(root, paths, only, tuple(args.exclude))

    if args.cmd == "list":
        for it in items:
            print(f"{it.file}:{it.line}\t{it.scope}\t{it.name}\t{it.kind}")
        return 0

    doc = Path(args.doc) if args.doc else root / "docs" / "reset-statics.md"
    rows, errors = read_doc(doc) if doc.exists() else ({}, [f"{doc}: missing"])
    missing = [it for it in items if it.key not in rows]

    if args.cmd == "skeleton":
        for it in missing:
            print(f"| {it.file} | {it.scope} | {it.name} | {it.kind} | ? | | |")
        return 0

    for e in errors:
        print(e, file=sys.stderr)
    for it in missing:
        print(f"{it.file}:{it.line}: {it.kind} `{it.name}` (in {it.scope}) "
              f"is not in {doc.name}", file=sys.stderr)
    if only is None:
        found = {it.key for it in items}
        scanned = {p.rstrip("/") for p in paths}
        stale = [k for k in rows if k not in found
                 and any(k[0] == s or k[0].startswith(s + "/") for s in scanned)
                 and not any(k[0].startswith(e) for e in args.exclude)]
        for k in stale:
            print(f"note: {k[0]} {k[2]} (in {k[1]}) is in {doc.name} but not in the tree",
                  file=sys.stderr)
    by_class: dict[str, int] = {}
    for it in items:
        cls = rows.get(it.key)
        if cls:
            by_class[cls] = by_class.get(cls, 0) + 1
    print(f"reset_statics: {len(items)} objects, {len(missing)} unclassified"
          + "".join(f", {by_class.get(c, 0)} {c}" for c in ("keep", "reset", "constant")))
    return 1 if missing or errors else 0


if __name__ == "__main__":
    sys.exit(main())
