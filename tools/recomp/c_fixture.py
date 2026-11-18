"""Compiler dialects for synthetic C fixtures; assertions stay in the caller."""
from pathlib import Path
import shutil


def compiler():
    cc = next((shutil.which(name) for name in ('cl', 'clang', 'gcc', 'cc')
               if shutil.which(name)), None)
    fallback = Path(r'C:\Program Files\LLVM\bin\clang.exe')
    return cc or (str(fallback) if fallback.exists() else None)


def is_msvc(cc):
    return Path(cc).stem.lower() == 'cl'


def _is_link_flag(flag):
    """Flags GNU ld only honours after the objects that need them.

    `-lm` before the source is dropped under --as-needed (Debian/Ubuntu's
    default): the linker has not yet seen a reference to floor, so libm is
    skipped and the later undefined reference fails the link.
    """
    return (flag.startswith('-l') or flag.startswith('-L')
            or flag.startswith('-Wl,'))


def command(cc, sources, output, *, include_dirs=(), defines=(),
            compile_only=False, gnu_flags=()):
    """Build/link or compile only, preserving GNU-specific checks on GNU tools."""
    sources = list(map(str, sources))
    if is_msvc(cc):
        if any(flag.startswith('-fsanitize=') for flag in gnu_flags):
            raise ValueError('MSVC has no signed-integer-overflow sanitizer')
        warnings = ['/W3', '/WX'] if '-Werror' in gnu_flags else ['/W0']
        return ([cc, '/nologo', '/O2', *warnings,
                 *('/I' + str(p) for p in include_dirs),
                 *('/D' + str(d) for d in defines), *sources]
                + (['/c', '/Fo:' + str(output)] if compile_only
                   else ['/Fe:' + str(output)]))
    compile_flags = [f for f in gnu_flags if not _is_link_flag(f)]
    link_flags = ([] if compile_only
                  else [f for f in gnu_flags if _is_link_flag(f)])
    return ([cc, '-w', '-O2', *compile_flags,
             *(arg for p in include_dirs for arg in ('-I', str(p))),
             *('-D' + str(d) for d in defines), *sources,
             *(['-c'] if compile_only else []), '-o', str(output),
             *link_flags])
