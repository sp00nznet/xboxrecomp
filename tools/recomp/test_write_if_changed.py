"""write_if_changed must not touch mtime when the bytes are the same.

That is the whole point of it: a regen rewrites all 54 chunks of generated C,
and an mtime bump on an identical chunk costs a full /O2 rebuild of it.
"""
import os
import tempfile

from .translator import write_if_changed


def test_write_if_changed():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "chunk.c")

        assert write_if_changed(p, "a") is True, "must create a missing file"
        assert open(p, encoding="utf-8").read() == "a"

        os.utime(p, (0, 0))
        assert write_if_changed(p, "a") is False, "identical must not write"
        assert os.stat(p).st_mtime == 0, "identical must not bump mtime"

        assert write_if_changed(p, "b") is True, "changed must write"
        assert open(p, encoding="utf-8").read() == "b"
        assert os.stat(p).st_mtime != 0


if __name__ == "__main__":
    test_write_if_changed()
    print("ok")
