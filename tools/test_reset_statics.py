"""Tests for tools/reset_statics.py (game-reset phase 0, task 0.2)."""

import subprocess
import textwrap
from pathlib import Path

from tools import reset_statics as rs

SAMPLE = textwrap.dedent(r"""
    #include <stdint.h>
    #define TABLE(x) static int x##_n;   /* a directive: not scanned */
    static int s_count, s_other = 3;
    static const int s_const = 4;
    static const char *s_name = "x;y";    /* a pointer to const: mutable */
    static const char *const s_names[] = { "a", "b" };
    static struct { int a; int b; } s_anon;
    static struct pair { int a; } s_pairs[4] = { { 1 } };
    static void (*s_cb)(int);
    static int helper(int x);
    static inline int twice(int x) { return 2 * x; }
    static XBOX_THREAD_LOCAL int t_depth;
    static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
    static volatile LONG g_inited;
    int g_run_state;
    extern int g_elsewhere;
    typedef struct { int z; } Thing;
    struct only_a_tag { int q; };
    void set_hook(void (*fn)(void));
    const uint32_t g_table[2] = { 1, 2 };
    __declspec(thread) int t_win;
    #ifdef __cplusplus
    extern "C" {
    #endif
    static int s_in_extern;
    #ifdef __cplusplus
    }
    #endif

    int run(int a)
    {
        static int warned;
        static const char hex[] = "0123456789";
        int local = 0;
        if (a) {
            static uint64_t last_ns;
            last_ns = a;
        }
        return local + warned + hex[0];
    }

    static int helper(int x)
    {
        static XBOX_THREAD_LOCAL char t_buf[64];
        return x + t_buf[0];
    }
""")

OBJC = textwrap.dedent(r"""
    @interface Box : NSObject {
        int ivar;
    }
    @property (nonatomic) int prop;
    @end
    static id<MTLDevice> s_device;
    @implementation Box
    - (void)tick:(int)n {
        static int ticks;
        ticks += n;
    }
    @end
""")


def scan(text, rel="src/x.c"):
    return {(i.scope, i.name): i.kind for i in rs.scan_text(text, rel)}


def test_kinds():
    got = scan(SAMPLE)
    assert got[("-", "s_count")] == "file_static"
    assert got[("-", "s_other")] == "file_static"
    assert got[("-", "s_name")] == "file_static"
    assert got[("-", "s_anon")] == "file_static"
    assert got[("-", "s_pairs")] == "file_static"
    assert got[("-", "s_cb")] == "file_static"
    assert got[("-", "t_depth")] == "tls"
    assert got[("-", "t_win")] == "tls"
    assert got[("-", "s_once")] == "once"
    assert got[("-", "g_inited")] == "once"
    assert got[("-", "g_run_state")] == "global"
    assert got[("-", "s_in_extern")] == "file_static"
    assert got[("run", "warned")] == "local_static"
    assert got[("run", "last_ns")] == "local_static"
    assert got[("helper", "t_buf")] == "tls"


def test_skips_consts_functions_and_types():
    got = scan(SAMPLE)
    names = {n for _, n in got}
    for absent in ("s_const", "s_names", "hex", "g_table", "helper", "twice",
                   "g_elsewhere", "Thing", "z", "q", "fn", "x_n", "local",
                   "set_hook", "only_a_tag"):
        assert absent not in names, absent


def test_objc():
    got = scan(OBJC, "src/x.m")
    assert got[("-", "s_device")] == "file_static"
    assert got[("tick", "ticks")] == "local_static"
    assert ("-", "ivar") not in got and ("-", "prop") not in got


MISSES = textwrap.dedent(r"""
    static uint64_t (*s_clock)(void);
    const char *keys_id(void) { static char id[17]; return id; }
    const char (*stage_names(int s, int *n))[64]
    {
        static int calls;
        return 0;
    }
    int presets(char (*ref)[16], int cap)
    {
        static char stem[8][64];
        return cap;
    }
""")


def test_declarators_the_first_pass_missed():
    """A pointer to function whose return type is a typedef, a local named
    `id` (an Objective-C word, not a C keyword), and the function after one
    that returns a pointer to an array (it used to swallow every header
    after it)."""
    got = scan(MISSES)
    assert got[("-", "s_clock")] == "file_static"
    assert got[("keys_id", "id")] == "local_static"
    assert got[("stage_names", "calls")] == "local_static"
    assert got[("presets", "stem")] == "local_static"


def test_unnamed_body_warns(capsys):
    """A header the parser cannot name is reported, and the next function
    is still found."""
    got = scan("int (*(weird)(int a))(void)\n{\n    static int x;\n}\nint ok(void)\n"
               "{\n    static int y;\n    return y;\n}\n")
    assert got.get(("ok", "y")) == "local_static"
    assert "x.c:2: a function body with no name found" in capsys.readouterr().err


def test_line_numbers():
    items = rs.scan_text(SAMPLE, "src/x.c")
    line = {i.name: i.line for i in items}
    assert line["s_count"] == SAMPLE.splitlines().index(
        "static int s_count, s_other = 3;") + 1


def write_tree(tmp_path: Path, doc_rows: str) -> Path:
    (tmp_path / "src").mkdir()
    (tmp_path / "src" / "a.c").write_text(
        "static int s_a;\nint f(void) { static int calls; return calls++; }\n")
    (tmp_path / "docs").mkdir()
    (tmp_path / "docs" / "reset-statics.md").write_text(textwrap.dedent("""
        # Reset statics

        | file | scope | name | kind | class | hook | why |
        |---|---|---|---|---|---|---|
    """) + doc_rows)
    return tmp_path


def test_check_passes_and_fails(tmp_path, capsys):
    root = write_tree(tmp_path,
                      "| src/a.c | - | s_a | file_static | reset | kernel | run state |\n"
                      "| src/a.c | f | calls | local_static | keep | - | a counter |\n")
    assert rs.main(["check", str(root)]) == 0
    assert "2 objects, 0 unclassified, 1 keep, 1 reset" in capsys.readouterr().out

    root2 = tmp_path / "two"
    root2.mkdir()
    write_tree(root2, "| src/a.c | - | s_a | file_static | reset | kernel | run state |\n")
    assert rs.main(["check", str(root2)]) == 1
    err = capsys.readouterr().err
    assert "src/a.c:2: local_static `calls` (in f)" in err


def test_bad_class_and_stale(tmp_path, capsys):
    root = write_tree(tmp_path,
                      "| src/a.c | - | s_a | file_static | maybe | | |\n"
                      "| src/a.c | f | calls | local_static | keep | | |\n"
                      "| src/a.c | - | s_gone | file_static | keep | | |\n")
    assert rs.main(["check", str(root)]) == 1
    err = capsys.readouterr().err
    assert "class `maybe`" in err
    assert "s_gone (in -) is in reset-statics.md but not in the tree" in err


def test_skeleton(tmp_path, capsys):
    root = write_tree(tmp_path, "")
    assert rs.main(["skeleton", str(root)]) == 0
    out = capsys.readouterr().out
    assert "| src/a.c | - | s_a | file_static | ? | | |" in out
    assert "| src/a.c | f | calls | local_static | ? | | |" in out


def test_diff(tmp_path, capsys):
    root = write_tree(tmp_path, "| src/a.c | - | s_a | file_static | reset | | |\n"
                                "| src/a.c | f | calls | local_static | keep | | |\n")
    run = lambda *a: subprocess.run(["git", "-C", str(root), *a], check=True,
                                    capture_output=True)
    run("init", "-q")
    run("-c", "user.email=t@t", "-c", "user.name=t", "add", ".")
    run("-c", "user.email=t@t", "-c", "user.name=t", "commit", "-qm", "base")
    (root / "src" / "b.c").write_text("static int s_new;\n")
    (root / "src" / "a.c").write_text((root / "src" / "a.c").read_text())
    run("add", "src/b.c")
    assert rs.main(["check", "--diff", "HEAD", str(root)]) == 1
    err = capsys.readouterr().err
    assert "src/b.c:1: file_static `s_new`" in err
    assert "s_a" not in err


def test_toolkit_doc_is_complete():
    """The committed classification covers the whole toolkit tree."""
    root = Path(rs.__file__).resolve().parent.parent
    assert rs.main(["check", str(root)]) == 0
