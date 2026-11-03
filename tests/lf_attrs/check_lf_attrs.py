"""Fail when a tracked file that must stay LF has no eol=lf attribute.

The patterns follow xboxrecomp-cli's package/lib.py LF_ONLY (the files a
bundle carries to Linux and macOS). .slang and .slangp are added beyond
LF_ONLY on purpose: every bundle ships the shader presets, and keeping them
LF keeps a Windows-built bundle's copies the same bytes as a Mac-built one."""

import re
import subprocess
import sys

LF_ONLY = re.compile(r"\.(sh|py|default|in|toml|env|slang|slangp)$")


def main(root):
    try:
        out = subprocess.run(
            ["git", "-C", root, "ls-files", "-z"], capture_output=True, check=True
        ).stdout
    except (OSError, subprocess.CalledProcessError):
        print("lf_attrs: %s is not a git checkout" % root)
        return 77
    names = [n for n in out.decode("utf-8").split("\0") if n and LF_ONLY.search(n.split("/")[-1])]
    if not names:
        print("lf_attrs: no files to check")
        return 1
    attrs = subprocess.run(
        ["git", "-C", root, "check-attr", "-z", "--stdin", "eol"],
        input="\0".join(names).encode("utf-8") + b"\0",
        capture_output=True,
        check=True,
    ).stdout.decode("utf-8")
    f = attrs.split("\0")
    bad = [f[i] for i in range(0, len(f) - 2, 3) if f[i + 2] != "lf"]
    for n in bad:
        print("lf_attrs: %s has no eol=lf in .gitattributes" % n)
    print("lf_attrs: %d files, %d without eol=lf" % (len(names), len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
