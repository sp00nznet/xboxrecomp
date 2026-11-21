"""tests for the optional templates/new-game game.toml emitter."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest

from tools import new_game

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "templates" / "new-game"


def load_toml(text):
    try:
        import tomllib
    except ImportError:
        return _load_toml_subset(text)
    return tomllib.loads(text)


def _atom(value):
    if value in ("true", "false"):
        return value == "true"
    if value.startswith("[") and value.endswith("]"):
        inner = value[1:-1].strip()
        if not inner:
            return []
        return [_atom(part.strip()) for part in inner.split(",")]
    if value.startswith('"') and value.endswith('"'):
        return value[1:-1]
    return int(value, 0)


def _load_toml_subset(text):
    out = {}
    table = out
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            table = out
            for part in line[1:-1].strip().split("."):
                table = table.setdefault(part, {})
            continue
        key, _, rest = line.partition("=")
        table[key.strip()] = _atom(rest.strip())
    return out


def values_for(dest, **overrides):
    kwargs = dict(
        slug="my-game",
        name="My Game",
        title_id="0",
        cli_tag="v0.22.0",
        cli_commit=new_game.CLI_COMMIT,
        toolkit_tag="v0.18.0",
        toolkit_branch="",
        toolkit_commit="",
    )
    kwargs.update(overrides)
    return new_game.build_values(dest, **kwargs)


def test_template_copy_does_not_include_game_toml():
    """The documented cp -r path must stay CLI-free unless --toml is used."""
    assert not (TEMPLATE / "game.toml").exists()
    assert (TEMPLATE / "CMakeLists.txt").is_file()
    assert (TEMPLATE / "src" / "main.c").is_file()
    assert (TEMPLATE / "src" / "recomp_manual.c").is_file()


def test_emit_writes_schema_required_keys(tmp_path):
    dest = tmp_path / "my-game"
    path = Path(new_game.emit_game_toml(dest, values_for(dest)))
    text = path.read_text(encoding="utf-8")
    doc = load_toml(text)
    assert doc["schema"] == 1
    assert doc["game"]["name"] == "My Game"
    assert doc["game"]["slug"] == "my-game"
    assert doc["xbe"]["path"] == "game_files/default.xbe"
    assert doc["xbe"]["title_id"] == 0
    assert doc["xbe"]["sha256"] == []
    assert doc["cli"]["tag"] == "v0.22.0"
    assert doc["cli"]["commit"] == new_game.CLI_COMMIT
    assert doc["cli"]["url"] == new_game.CLI_URL
    assert doc["toolkit"]["url"] == new_game.TOOLKIT_URL
    assert doc["toolkit"]["tag"] == "v0.18.0"
    assert "branch" not in doc["toolkit"]
    assert "commit" not in doc["toolkit"]
    assert doc["toolchain"]["llvm_mingw"] == new_game.LLVM_MINGW
    assert doc["pipeline"]["game_name"] == "My Game"
    assert doc["pipeline"]["gen"] == "src/recomp/gen"
    assert doc["pipeline"]["out"] == "analysis"
    assert doc["pipeline"]["seeds"] == ["config/seed_functions.json"]
    assert doc["pipeline"]["icall_seeds"] is False
    assert doc["pipeline"]["exclude_manual"] == "src/recomp_manual.c"
    assert doc["pipeline"]["disasm"]["text_only"] is False
    assert doc["build"]["exe"] == "my_game_recomp"
    assert doc["build"]["targets"] == ["windows", "macos"]
    assert doc["build"]["stock_cmake"] == [
        "-DXBOXRECOMP_ENHANCE=ON",
        "-DXBOXRECOMP_APU_DSP=ON",
    ]
    assert doc["data"]["game_files"] == "game_files"
    # Opt-in tables stay comments, as in the CLI starter.
    assert "package" not in doc
    assert "wizard" not in doc
    assert "public" not in doc
    assert "[package]" in text
    assert "[[wizard.option]]" in text


def test_emit_refuses_to_overwrite(tmp_path):
    dest = tmp_path / "my-game"
    new_game.emit_game_toml(dest, values_for(dest))
    with pytest.raises(new_game.Error, match="never overwrites"):
        new_game.emit_game_toml(dest, values_for(dest))


def test_title_id_and_slug_and_pins(tmp_path):
    dest = tmp_path / "ignored"
    values = values_for(
        dest,
        name="BLiNX 2",
        slug="blinx2",
        title_id="0x4D530065",
        toolkit_tag="",
        toolkit_branch="main",
        toolkit_commit="37b61d589168adee3680fabd51805ee81427bf07",
        cli_tag="",
        cli_commit=new_game.CLI_COMMIT,
    )
    doc = load_toml(new_game.render_game_toml(values))
    assert doc["xbe"]["title_id"] == 0x4D530065
    assert doc["game"]["slug"] == "blinx2"
    assert doc["build"]["exe"] == "blinx2_recomp"
    assert doc["toolkit"]["branch"] == "main"
    assert doc["toolkit"]["commit"] == "37b61d589168adee3680fabd51805ee81427bf07"
    assert "tag" not in doc["toolkit"]
    assert doc["cli"]["commit"] == new_game.CLI_COMMIT
    assert "tag" not in doc["cli"]


def test_invalid_slug_and_incomplete_pins():
    assert new_game.slug_from("My Game!") == "my-game"
    with pytest.raises(new_game.Error, match="slug"):
        new_game.slug_from("!!!")
    with pytest.raises(new_game.Error, match="cli"):
        new_game.build_values(
            "dest",
            slug="ok",
            cli_tag="",
            cli_commit="",
            toolkit_tag="v0.18.0",
        )
    with pytest.raises(new_game.Error, match="toolkit"):
        new_game.build_values(
            "dest",
            slug="ok",
            cli_tag="v0.22.0",
            toolkit_tag="",
            toolkit_branch="main",
            toolkit_commit="",
        )
    with pytest.raises(new_game.Error, match="40-character"):
        new_game.build_values(
            "dest",
            slug="ok",
            cli_tag="v0.22.0",
            toolkit_tag="",
            toolkit_branch="main",
            toolkit_commit="abc",
        )


def test_main_copies_template_and_emits(tmp_path):
    dest = tmp_path / "my_xbox_game"
    code = new_game.main(
        [
            str(dest),
            "--toml",
            "--name",
            "My Xbox Game",
            "--slug",
            "my-xbox-game",
            "--toolkit-tag",
            "v0.18.0",
        ]
    )
    assert code == 0
    assert (dest / "CMakeLists.txt").is_file()
    assert (dest / "src" / "main.c").is_file()
    assert (dest / "src" / "recomp_manual.c").is_file()
    assert (dest / ".gitignore").is_file()
    doc = load_toml((dest / "game.toml").read_text(encoding="utf-8"))
    assert doc["game"]["slug"] == "my-xbox-game"
    assert doc["game"]["name"] == "My Xbox Game"
    assert doc["build"]["exe"] == "my_xbox_game_recomp"
    assert doc["toolkit"]["tag"] == "v0.18.0"
    # Defaults lock the schema revision we matched.
    assert doc["cli"]["tag"] == new_game.CLI_TAG
    assert doc["cli"]["commit"] == new_game.CLI_COMMIT


def test_main_emits_into_existing_copy(tmp_path):
    dest = tmp_path / "already"
    new_game.copy_template(dest)
    marker = dest / "CMakeLists.txt"
    before = marker.read_bytes()
    code = new_game.main([str(dest), "--toml", "--slug", "already", "--toolkit-tag", "v0.1.0"])
    assert code == 0
    assert marker.read_bytes() == before
    assert (dest / "game.toml").is_file()
    # A second run must not clobber the project or the manifest.
    assert new_game.main([str(dest), "--toml", "--slug", "already", "--toolkit-tag", "v0.1.0"]) == 1


def test_main_requires_toml_flag(tmp_path):
    dest = tmp_path / "my-game"
    with pytest.raises(SystemExit):
        new_game.main([str(dest)])
    assert not dest.exists()


def test_main_rejects_a_non_template_directory(tmp_path):
    dest = tmp_path / "empty"
    dest.mkdir()
    assert new_game.main([str(dest), "--toml", "--slug", "empty", "--toolkit-tag", "v0.1.0"]) == 1
    assert not (dest / "game.toml").exists()


def test_default_toolkit_pin_exact_tag(tmp_path):
    repo = tmp_path / "tk"
    _init_git(repo)
    sha = _git(repo, "rev-parse", "HEAD")
    tag, commit, branch = new_game.default_toolkit_pin(str(repo))
    assert tag == "v1.2.3"
    assert commit == sha
    assert branch == ""


def test_default_toolkit_pin_reachable_tag_when_ahead(tmp_path):
    repo = tmp_path / "tk"
    _init_git(repo)
    (repo / "next.txt").write_text("ahead\n", encoding="utf-8")
    _git(repo, "add", "next.txt")
    _git(repo, "commit", "-m", "ahead")
    tag, commit, branch = new_game.default_toolkit_pin(str(repo))
    assert tag == "v1.2.3"
    assert commit == ""
    assert branch == ""


def test_module_invocation_from_toolkit_root(tmp_path):
    dest = tmp_path / "invoked"
    result = subprocess.run(
        [
            sys.executable,
            "-m",
            "tools.new_game",
            str(dest),
            "--toml",
            "--slug",
            "invoked",
            "--toolkit-tag",
            "v0.18.0",
        ],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    assert "wrote" in result.stdout
    assert (dest / "game.toml").is_file()
    assert (dest / "CMakeLists.txt").is_file()


def _git(repo, *args):
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=True,
        env={
            **os.environ,
            "GIT_AUTHOR_NAME": "Test",
            "GIT_AUTHOR_EMAIL": "test@example.com",
            "GIT_COMMITTER_NAME": "Test",
            "GIT_COMMITTER_EMAIL": "test@example.com",
        },
    )
    return result.stdout.strip()


def _init_git(repo):
    repo.mkdir()
    (repo / "README").write_text("x\n", encoding="utf-8")
    _git(repo, "init")
    _git(repo, "checkout", "-b", "main")
    _git(repo, "add", "README")
    _git(repo, "commit", "-m", "init")
    _git(repo, "tag", "v1.2.3")
