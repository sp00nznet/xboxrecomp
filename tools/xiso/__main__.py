"""
List or extract files from an Xbox disc image.

    py -3 -m tools.xiso ls      game.iso
    py -3 -m tools.xiso get     game.iso default.xbe -o game/
    py -3 -m tools.xiso unpack  game.iso -o extracted/
"""

import argparse
import os
import sys

from .xdvdfs import Xiso, XisoError


def cmd_ls(args):
    with Xiso(args.iso) as iso:
        print("partition base : 0x%08X" % iso.base)
        total = 0
        for d, e in sorted(iso.walk(), key=lambda x: (x[0], x[1].name)):
            print("  %10d  %s" % (e.size, (d + "/" + e.name).lstrip("/")))
            total += e.size
        print("  %10d  total" % total)
    return 0


def cmd_get(args):
    with Xiso(args.iso) as iso:
        # Allow a path so companion executables in subdirectories work too.
        path, _, name = args.name.replace("\\", "/").rpartition("/")
        e = iso.find(name, path)
        if e is None:
            print("not found in image: %s" % args.name, file=sys.stderr)
            return 1
        dest = os.path.join(args.output, e.name) if os.path.isdir(args.output) \
            or args.output.endswith(("/", "\\")) else args.output
        iso.extract(e, dest)
        print("%s -> %s (%d bytes)" % (e.name, dest, e.size))
    return 0


def _safe_component(name):
    """A disc-supplied name that can be a single path component under the output.

    The names come from the image, not from the user, so a crafted entry such as
    '../x' or 'a\\b' would otherwise escape the output directory on extraction.
    """
    return name not in ("", ".", "..") and not any(c in name for c in "/\\:")


def cmd_unpack(args):
    with Xiso(args.iso) as iso:
        n = 0
        for d, e in iso.walk():
            parts = (d.split("/") if d else []) + [e.name]
            if not all(_safe_component(p) for p in parts):
                print("skipping unsafe disc path: %r" % "/".join(parts), file=sys.stderr)
                continue
            dest = os.path.join(args.output, *parts)
            iso.extract(e, dest)
            n += 1
        print("extracted %d files to %s" % (n, args.output))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools.xiso", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("ls", help="list every file in the image")
    p.add_argument("iso")
    p.set_defaults(func=cmd_ls)

    p = sub.add_parser("get", help="extract one file")
    p.add_argument("iso")
    p.add_argument("name", help="e.g. default.xbe")
    p.add_argument("-o", "--output", default=".", help="file or directory")
    p.set_defaults(func=cmd_get)

    p = sub.add_parser("unpack", help="extract everything")
    p.add_argument("iso")
    p.add_argument("-o", "--output", required=True)
    p.set_defaults(func=cmd_unpack)

    args = ap.parse_args(argv)
    try:
        return args.func(args)
    except XisoError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
