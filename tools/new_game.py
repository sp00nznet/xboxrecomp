"""Optional game.toml emitter for a project started from templates/new-game.

The documented default is still to copy the template by hand:

    cp -r templates/new-game ../mygame

This module is the opt-in path invited in issue #223: emit a game.toml whose
shape matches xboxrecomp-cli's `xbr new` starter, so a game repo can use that
external front end without running `xbr new`. It never overwrites a file.

    py -3 -m tools.new_game ../mygame --toml

Run from the toolkit root. If DIR is absent, the template is copied first.
If DIR already exists (the usual `cp -r` result), only game.toml is added.

The TOML itself belongs to xboxrecomp-cli (apexJCL). GAME_TOML below follows
src/xboxrecomp_cli/scaffold.py at v0.22.0
(a994f6aec0972346f65063bd2586398aad1e4b21). Required keys and pin rules are
those in src/xboxrecomp_cli/manifest.py and docs/manifest.md at the same
commit. regen.sh and the README steps stay the documented route.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEMPLATE_DIR = os.path.join(REPO_ROOT, "templates", "new-game")

# Official upstream, not a personal fork: this is the repo the template lives in.
TOOLKIT_URL = "https://github.com/sp00nznet/xboxrecomp.git"
CLI_URL = "https://github.com/apexJCL/xboxrecomp-cli.git"
# The xboxrecomp-cli release whose GAME_TOML / manifest schema we matched.
CLI_TAG = "v0.22.0"
CLI_COMMIT = "a994f6aec0972346f65063bd2586398aad1e4b21"
# Same default the CLI's starter writes (scaffold.py LLVM_MINGW at that commit).
LLVM_MINGW = "20260922"

SLUG_RE = re.compile(r"^[a-z0-9][a-z0-9-]*$")
PLACEHOLDER_RE = re.compile(r"@(\w+)@")

# Starter written by `xbr new`. Comments and keys are kept in the same order
# so a later CLI release can be diffed against this string. @CLI_PIN@ expands
# to the tag/commit assignment lines, including their trailing newlines, and
# sits immediately before `url`.
GAME_TOML = """\
# game.toml: what xboxrecomp-cli needs to know about @NAME@. The CLI reads
# it on every command; every key and its default is in the CLI's
# docs/manifest.md. Written by `py -3 -m tools.new_game DIR --toml`; the
# keys match xboxrecomp-cli's `xbr new` starter (scaffold.py GAME_TOML at
# v0.22.0, a994f6aec097). The CLI never writes this file again.
schema = 1

[game]
name = "@NAME@"
slug = "@SLUG@"

[xbe]
path = "game_files/default.xbe"
# The certificate's title ID: `package` refuses a dump with another one.
title_id = @TITLE_ID@
# Known dumps (sha256 of default.xbe); empty accepts any dump.
sha256 = []

[cli]
# The xboxrecomp-cli ./@SLUG@ runs: the bootstrap clones it at this release
# tag (or commit) when it finds none; with both, the commit locks the tag.
# `@SLUG@ pins refresh` prints the newest release.
@CLI_PIN@url = "@CLI_URL@"

[toolkit]
# `@SLUG@ setup` clones it into external/xboxrecomp at this commit. A
# release tag pins it too: tag = "..." (branch is then optional).
url = "@TOOLKIT_URL@"
@TOOLKIT_PIN@
[toolchain]
# The llvm-mingw release the windows target cross-compiles with, on every host.
llvm_mingw = "@LLVM_MINGW@"

[pipeline]
# Stamped into every generated file: fixed for the life of the game.
game_name = "@NAME@"
gen = "src/recomp/gen"
out = "analysis"
# Function starts the disassembler missed (ICALL failures at runtime).
seeds = ["config/seed_functions.json"]
# The toolkit's icall_feedback database belongs to another game: off.
icall_seeds = false
exclude_manual = "src/recomp_manual.c"

[pipeline.disasm]
# Every code section: without the XDK sections (D3D, DSOUND, XGRPH, ...)
# there are no function boundaries there, and their internal calls become
# stubs. A title whose XBE flags data sections executable sets
# text_only = true and names its code sections in extra_sections.
text_only = false

[build]
# The CMake target and file name: build-win/@EXE@.exe
exe = "@EXE@"
# windows builds on every host (llvm-mingw); macos builds on a Mac (Apple
# clang). Drop one to stop `build` offering it.
targets = ["windows", "macos"]
# The enhancements layer (the menu, render scale, present shaders; stock
# until a player changes them) and the APU's DSP JIT (the toolkit's default
# where it has an archive for the target).
stock_cmake = ["-DXBOXRECOMP_ENHANCE=ON", "-DXBOXRECOMP_APU_DSP=ON"]

# How much memory one generated-code compile job takes sets how many run
# at once. Unset, it is estimated (140 MiB per MB of the largest gen/
# file); after a build, `@SLUG@ doctor` shows the measured peak on its
# `build jobs` line, and that figure goes here.
# [build.resources]
# gen_job_mib = 1200
# tool_mib = 1700        # the largest single pipeline tool (disasm)
# tool_step = "disasm"

[data]
game_files = "game_files"

# Packaging (a private bundle with your own dump inside) is opt-in:
# [package]
# app = "@APP@"
# targets = ["windows", "steamos"]
# with packaging/launch.env.default.windows and packaging/windows/README.part.
# See docs/manifest.md, [package].

# The public repo (opt-in): `@SLUG@ public init`, then each round
# `@SLUG@ public export` and the push it prints; the private notes, the
# patterns and the rewrite rules stay out of it. See the CLI's
# docs/layout.md.
# [public]
# url = "git@github.com:OWNER/@SLUG@-recomp.git"
# author = "Your Name <you@users.noreply.github.com>"

# The player's build wizard (needs [package]): `@SLUG@ wizard build` makes
# its Windows, macOS and Linux downloads for a release tag, and
# .github/workflows/release-wizard.yml attaches them to the release. See
# the CLI's docs/wizard.md.
# [wizard]
# repo = "https://github.com/OWNER/@SLUG@-recomp.git"   # [public] url, https form
# icon = "packaging/wizard-icon.png"
# [[wizard.option]]
# key = "scale"
# label = "Render scale"
# kind = "range"
# min = 1
# max = 4
# default = 1
# config = "render.scale"
# [[wizard.option]]
# key = "fullscreen"
# label = "Fullscreen"
# kind = "bool"
# default = false
# config = "present.fullscreen"
"""


class Error(Exception):
    pass


def fill(text, values):
    for key, value in values.items():
        text = text.replace("@%s@" % key, str(value))
    leftover = PLACEHOLDER_RE.findall(text)
    if leftover:
        raise Error("unfilled placeholders: %s" % ", ".join(sorted(set(leftover))))
    return text


def pin_lines(tag, commit, branch=""):
    """Assignment lines for a [cli] or [toolkit] pin, each with a newline."""
    rows = (("tag", tag), ("commit", commit), ("branch", branch))
    return "".join('%s = "%s"\n' % (key, value) for key, value in rows if value)


def slug_from(name):
    text = re.sub(r"[^a-z0-9]+", "-", name.strip().lower()).strip("-")
    if not SLUG_RE.match(text):
        raise Error("slug %r: lower-case letters, digits and '-' only (--slug)" % (name,))
    return text


def clean_name(name, slug):
    text = " ".join(name.replace("_", " ").replace("-", " ").split())
    return text or slug


def parse_title_id(value):
    try:
        title_id = int(value, 0)
    except (TypeError, ValueError):
        raise Error("title-id %r: an integer, such as 0x4D530065" % (value,))
    if not 0 <= title_id <= 0xFFFFFFFF:
        raise Error("title-id %r: a 32-bit certificate field" % (value,))
    return "0x%08X" % title_id


def git_out(repo, *args):
    result = subprocess.run(
        ["git", "-C", repo, *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    if result.returncode != 0:
        return ""
    return result.stdout.strip()


def default_toolkit_pin(repo=REPO_ROOT):
    """Pin a release tag when this checkout has one, else branch+commit.

    An exact tag on HEAD is preferred (and locked with its commit). Otherwise
    the newest reachable tag is a stable starter; a checkout with no tags
    falls back to the current branch and commit, which only clones if that
    commit is on the remote.
    """
    commit = git_out(repo, "rev-parse", "HEAD")
    exact = git_out(repo, "describe", "--tags", "--exact-match", "HEAD")
    if exact and commit:
        return exact, commit, ""
    tag = git_out(repo, "describe", "--tags", "--abbrev=0")
    if tag:
        return tag, "", ""
    branch = git_out(repo, "rev-parse", "--abbrev-ref", "HEAD")
    if branch == "HEAD":
        branch = ""
    if commit and branch:
        return "", commit, branch
    raise Error(
        "could not detect a toolkit pin from %s; pass --toolkit-tag or "
        "--toolkit-branch and --toolkit-commit" % repo
    )


def render_game_toml(values):
    text = fill(GAME_TOML, values)
    if not text.endswith("\n"):
        text += "\n"
    return text


def looks_like_template(dest):
    return os.path.isfile(os.path.join(dest, "CMakeLists.txt")) and os.path.isfile(
        os.path.join(dest, "src", "main.c")
    )


def copy_template(dest, template_dir=TEMPLATE_DIR):
    if not os.path.isdir(template_dir):
        raise Error("templates/new-game is missing at %s" % template_dir)
    os.makedirs(os.path.dirname(os.path.abspath(dest)) or ".", exist_ok=True)
    shutil.copytree(template_dir, dest)
    return dest


def emit_game_toml(dest, values):
    path = os.path.join(dest, "game.toml")
    if os.path.exists(path):
        raise Error("%s exists; the --toml option never overwrites a file" % path)
    os.makedirs(dest, exist_ok=True)
    text = render_game_toml(values)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    return path


def build_values(
    dest,
    name="",
    slug="",
    title_id="0",
    cli_tag=None,
    cli_commit=None,
    cli_url=CLI_URL,
    toolkit_url=TOOLKIT_URL,
    toolkit_tag=None,
    toolkit_branch=None,
    toolkit_commit=None,
    llvm_mingw=LLVM_MINGW,
    toolkit_repo=REPO_ROOT,
):
    slug = slug_from(slug or os.path.basename(os.path.abspath(dest)))
    name = clean_name(name or os.path.basename(os.path.abspath(dest)), slug)
    if cli_tag is None and cli_commit is None:
        cli_tag, cli_commit = CLI_TAG, CLI_COMMIT
    cli_tag = cli_tag or ""
    cli_commit = cli_commit or ""
    if not cli_tag and not cli_commit:
        raise Error("cli: tag or commit required (--cli-tag or --cli-commit)")
    if toolkit_tag is None and toolkit_branch is None and toolkit_commit is None:
        toolkit_tag, toolkit_commit, toolkit_branch = default_toolkit_pin(toolkit_repo)
    toolkit_tag = toolkit_tag or ""
    toolkit_branch = toolkit_branch or ""
    toolkit_commit = toolkit_commit or ""
    if not toolkit_tag and not (toolkit_branch and toolkit_commit):
        raise Error("toolkit: a tag, or both a branch and a commit, is required")
    if toolkit_commit and not re.match(r"^[0-9a-f]{40}$", toolkit_commit):
        raise Error("toolkit.commit: a full 40-character commit sha")
    if cli_commit and not re.match(r"^[0-9a-f]{40}$", cli_commit):
        raise Error("cli.commit: a full 40-character commit sha")
    exe = slug.replace("-", "_") + "_recomp"
    app = re.sub(r"[^A-Za-z0-9]", "", name.title()) or slug
    return {
        "NAME": name,
        "SLUG": slug,
        "EXE": exe,
        "APP": app,
        "TITLE_ID": parse_title_id(title_id),
        "CLI_PIN": pin_lines(cli_tag, cli_commit),
        "CLI_URL": cli_url,
        "TOOLKIT_URL": toolkit_url,
        "TOOLKIT_PIN": pin_lines(toolkit_tag, toolkit_commit, toolkit_branch),
        "LLVM_MINGW": llvm_mingw,
    }


def prepare_dest(dest, template_dir=TEMPLATE_DIR):
    """Copy templates/new-game when DIR is new; otherwise leave it as-is."""
    if os.path.exists(dest):
        if not os.path.isdir(dest):
            raise Error("%s exists and is not a directory" % dest)
        if not looks_like_template(dest):
            raise Error(
                "%s exists but is not a templates/new-game project "
                "(needs CMakeLists.txt and src/main.c)" % dest
            )
        return False
    copy_template(dest, template_dir=template_dir)
    return True


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        prog="py -3 -m tools.new_game",
        description=(
            "optional game.toml for a templates/new-game project, matching "
            "the xboxrecomp-cli starter schema. The default copy path is unchanged."
        ),
    )
    parser.add_argument("dir", help="the game project directory (created if absent)")
    parser.add_argument(
        "--toml",
        action="store_true",
        help="emit game.toml for xboxrecomp-cli (this is the option)",
    )
    parser.add_argument("--name", default="", help="the game's name (default: DIR's name)")
    parser.add_argument("--slug", default="", help="[a-z0-9-] (default: DIR's name)")
    parser.add_argument(
        "--title-id",
        default="0",
        dest="title_id",
        help="XBE certificate title ID (default: 0, fill in later)",
    )
    parser.add_argument("--cli-tag", default=None, help="xboxrecomp-cli release tag")
    parser.add_argument("--cli-commit", default=None, help="full 40-character sha")
    parser.add_argument("--cli-url", default=CLI_URL)
    parser.add_argument("--toolkit-url", default=TOOLKIT_URL)
    parser.add_argument("--toolkit-tag", default=None)
    parser.add_argument("--toolkit-branch", default=None)
    parser.add_argument("--toolkit-commit", default=None)
    parser.add_argument("--llvm-mingw", default=LLVM_MINGW)
    args = parser.parse_args(argv)
    if not args.toml:
        parser.error("pass --toml to emit game.toml (the templates/new-game option)")
    return args


def main(argv=None):
    args = parse_args(argv)
    dest = os.path.abspath(args.dir)
    try:
        copied = prepare_dest(dest)
        values = build_values(
            dest,
            name=args.name,
            slug=args.slug,
            title_id=args.title_id,
            cli_tag=args.cli_tag,
            cli_commit=args.cli_commit,
            cli_url=args.cli_url,
            toolkit_url=args.toolkit_url,
            toolkit_tag=args.toolkit_tag,
            toolkit_branch=args.toolkit_branch,
            toolkit_commit=args.toolkit_commit,
            llvm_mingw=args.llvm_mingw,
        )
        path = emit_game_toml(dest, values)
    except Error as exc:
        print(exc, file=sys.stderr)
        return 1
    if copied:
        print("copied templates/new-game to %s" % dest)
    print("wrote %s" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
