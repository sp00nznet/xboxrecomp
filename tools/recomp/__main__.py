"""
Xbox x86 → C Static Recompiler

Usage:
    py -3 -m tools.recomp <xbe_path> [options]

Options:
    -o, --output-dir DIR    Output directory (default: tools/recomp/output)
    --game-name NAME        Game name stamped into generated-code banners
                            (default: "Xbox Game")
    -f, --function ADDR     Translate a single function (hex address)
    -c, --category CAT      Translate functions of a specific category
    --game-only             Only translate game_engine + game_vtable + unknown functions
    --all                   Translate all functions (including RW, CRT, XDK)
    -n, --max-funcs N       Maximum number of functions to translate
    -v, --verbose           Verbose output
    --list-categories       List available function categories and counts
    --header                Generate C header with forward declarations
"""

import argparse
import json
import os
import sys
import time

from . import config
from .translator import BatchTranslator
from .output import write_summary, print_stats, generate_header


def find_data_files(disasm_dir=None, func_id_dir=None, abi_dir=None, overrides=None):
    """Locate the disasm/func_id output files.

    Defaults to the in-tree tools/*/output directories. A game project living
    outside this repo passes --disasm-dir/--func-id-dir (or the per-file
    overrides) to point at its own pipeline output instead.
    """
    base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    disasm_dir = disasm_dir or os.path.join(base, "disasm", "output")
    func_id_dir = func_id_dir or os.path.join(base, "func_id", "output")
    abi_dir = abi_dir or os.path.join(base, "abi_analysis", "output")

    paths = {
        "functions": os.path.join(disasm_dir, "functions.json"),
        "labels": os.path.join(disasm_dir, "labels.json"),
        "identified": os.path.join(func_id_dir, "identified_functions.json"),
        "abi": os.path.join(abi_dir, "abi_functions.json"),
        "summary": os.path.join(disasm_dir, "summary.json"),
    }
    paths.update({k: v for k, v in (overrides or {}).items() if v})

    for key, path in paths.items():
        if not os.path.exists(path):
            if key != "summary":
                print(f"WARNING: {key} not found at {path}", file=sys.stderr)
            paths[key] = None

    return paths


def _parse_force_returns(items):
    """Parse --force-return ADDR=VALUE pairs into {addr: value}."""
    out = {}
    for item in items or ():
        if "=" not in item:
            raise SystemExit(f"--force-return wants ADDR=VALUE, got {item!r}")
        addr, _, value = item.partition("=")
        out[int(addr, 0)] = int(value, 0)
    return out


def _load_addrs(path):
    """Load a JSON address list (or {addr: name} map) as a set of ints."""
    if not path:
        return set()
    with open(path, "r", encoding="utf-8") as fh:
        entries = json.load(fh)
    if isinstance(entries, dict):
        entries = list(entries.keys())
    out = set()
    for e in entries:
        if isinstance(e, dict):
            e = e.get("start") or e.get("address")
        out.add(int(e, 16) if isinstance(e, str) else int(e))
    return out


def check_data_matches_binary(xbe_path, summary_path):
    """Refuse to lift one binary's code using another binary's disassembly.

    The input paths default to shared in-tree directories, so a stale run from a
    different game silently produces a full set of plausible-looking C that
    belongs to the wrong binary. The disassembler records which file it read;
    compare it and stop if it disagrees.
    """
    if not summary_path:
        return

    try:
        with open(summary_path, "r", encoding="utf-8") as fh:
            recorded = json.load(fh).get("binary")
    except (OSError, ValueError):
        return

    if not recorded:
        return

    if os.path.basename(recorded).lower() != os.path.basename(xbe_path).lower():
        print(
            f"ERROR: disassembly is for '{os.path.basename(recorded)}' but you "
            f"asked to recompile '{os.path.basename(xbe_path)}'.\n"
            f"       Re-run tools.disasm on this binary, or point --disasm-dir "
            f"at the matching output.",
            file=sys.stderr,
        )
        sys.exit(1)


def list_categories(translator):
    """Print category breakdown."""
    cats = {}
    for addr, func_info in sorted(translator.func_db.items()):
        cls = translator.classification_db.get(addr, {})
        cat = cls.get("category", "unknown")
        cats[cat] = cats.get(cat, 0) + 1

    print(f"\nFunction categories ({len(translator.func_db)} total):")
    print(f"{'Category':<30} {'Count':>8} {'Pct':>8}")
    print("-" * 48)
    for cat, count in sorted(cats.items(), key=lambda x: -x[1]):
        pct = count / len(translator.func_db) * 100
        print(f"{cat:<30} {count:>8} {pct:>7.1f}%")


def _report_unimplemented(stats):
    """List the mnemonics that were emitted as TODO comments.

    An unimplemented instruction is not a build error -- it is a comment, and
    the generated function keeps running with whatever the destination already
    held. That is the worst kind of failure: it looks like it worked. Wreckless
    booted for weeks with `bsf eax, ecx` translated to nothing, which made the
    CRT heap's free-list bitmap scan return its own argument and hand out the
    address of an empty list head.

    Sorted by count, with one example address each so the caller can go and
    look at it.
    """
    unimplemented = stats.get("unimplemented") or {}
    if not unimplemented:
        return
    total = sum(len(a) for a in unimplemented.values())
    print(f"{total} instruction(s) unimplemented, emitted as no-op comments "
          f"({len(unimplemented)} distinct mnemonics):", file=sys.stderr)
    ranked = sorted(unimplemented.items(),
                    key=lambda kv: (-len(kv[1]), kv[0]))
    for mnemonic, addrs in ranked[:20]:
        print(f"    {len(addrs):6d}  {mnemonic:<12} e.g. 0x{min(addrs):08X}",
              file=sys.stderr)
    if len(ranked) > 20:
        print(f"    ... and {len(ranked) - 20} more", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(
        description="Xbox x86 -> C Static Recompiler")
    parser.add_argument("xbe_path", help="Path to default.xbe")
    parser.add_argument("--game-name",
                        help="Game name stamped into generated-code banners")
    parser.add_argument("-o", "--output-dir",
                        help="Output directory")
    parser.add_argument("-f", "--function",
                        help="Translate single function (hex address)")
    parser.add_argument("-c", "--category",
                        help="Translate functions of a category")
    parser.add_argument("--game-only", action="store_true",
                        help="Only game functions (game_engine, game_vtable, unknown)")
    parser.add_argument("--all", action="store_true",
                        help="Translate all functions")
    parser.add_argument("-n", "--max-funcs", type=int,
                        help="Max functions to translate")
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument("--list-categories", action="store_true",
                        help="List categories and exit")
    parser.add_argument("--header", action="store_true",
                        help="Generate C header file")
    parser.add_argument("--split", type=int, metavar="N",
                        help="Split output into files of N functions each")
    parser.add_argument("--gen-dir",
                        help="Output dir for split generated files "
                             "(default: src/game/recomp/gen)")
    parser.add_argument("--disasm-dir",
                        help="Directory holding tools.disasm output "
                             "(default: tools/disasm/output)")
    parser.add_argument("--func-id-dir",
                        help="Directory holding tools.func_id output "
                             "(default: tools/func_id/output)")
    parser.add_argument("--abi-dir",
                        help="Directory holding tools.abi_analysis output "
                             "(default: tools/abi_analysis/output)")
    parser.add_argument("--functions",
                        help="Path to functions.json (overrides --disasm-dir)")
    parser.add_argument("--labels",
                        help="Path to labels.json (overrides --disasm-dir)")
    parser.add_argument("--identified",
                        help="Path to identified_functions.json "
                             "(overrides --func-id-dir)")
    parser.add_argument("--abi",
                        help="Path to abi_functions.json (overrides --abi-dir)")
    parser.add_argument("--skip-binary-check", action="store_true",
                        help="Allow disassembly recorded for a different binary")
    parser.add_argument("--manual-functions", metavar="FILE",
                        help="JSON list of addresses the project implements by "
                             "hand. Their bodies are not generated, so the "
                             "hand-written definition links instead")
    parser.add_argument("--exclude-manual", metavar="FILE",
                        nargs="?", const="src/game/recomp/recomp_manual.c",
                        help="Scan a C file (default recomp_manual.c) for the "
                             "functions it defines by hand and skip generating "
                             "their bodies -- the same effect as listing them in "
                             "--manual-functions, but read straight from the "
                             "source of truth so the two cannot drift. Handles "
                             "sub_X_gen wrappers and pins referenced sub_ names. "
                             "Ported from the Burnout 3 fork.")
    parser.add_argument("--trace-functions", metavar="FILE",
                        help="JSON list of addresses to emit an entry trace "
                             "for (RECOMP_TRACE_ENTER). For bring-up: shows "
                             "which call in an init chain is not returning")
    parser.add_argument("--force-return", metavar="ADDR=VALUE",
                        action="append", default=[],
                        help="Make a function hand its callers a constant "
                             "instead of what it computed, e.g. "
                             "0x0015D780=0. Repeatable. A bring-up probe for "
                             "a title waiting on a service the runtime does "
                             "not implement yet: the body still runs, only "
                             "the answer changes, and the emitted code is "
                             "inert unless RECOMP_FORCE_RETURN is set")
    parser.add_argument("--seh-prolog", metavar="ADDR",
                        help="Address of __SEH_prolog (hex). Auto-detected if omitted")
    parser.add_argument("--seh-epilog", metavar="ADDR",
                        help="Address of __SEH_epilog (hex). Auto-detected if omitted")

    args = parser.parse_args()

    if args.game_name:
        config.set_game_name(args.game_name)

    # Find data files
    data_files = find_data_files(
        disasm_dir=args.disasm_dir,
        func_id_dir=args.func_id_dir,
        abi_dir=args.abi_dir,
        overrides={
            "functions": args.functions,
            "labels": args.labels,
            "identified": args.identified,
            "abi": args.abi,
        },
    )
    if not data_files["functions"]:
        print("ERROR: functions.json not found. Run the disassembler first.",
              file=sys.stderr)
        sys.exit(1)

    if not args.skip_binary_check:
        check_data_matches_binary(args.xbe_path, data_files.get("summary"))

    # Derive the memory map from the binary we were handed. Skipping this leaves
    # the fallback layout in place, and every VA outside it lifts as "not code".
    try:
        config.configure_from_xbe(args.xbe_path)
    except Exception as e:
        print(f"ERROR: could not read section layout from {args.xbe_path}: {e}",
              file=sys.stderr)
        sys.exit(1)

    if args.verbose:
        print(f"Section layout from {os.path.basename(args.xbe_path)}: "
              f"{len(config.SECTIONS)} sections, "
              f".text 0x{config.TEXT_VA_START:08X}-0x{config.TEXT_VA_END:08X}",
              file=sys.stderr)

    print(f"Loading data files...", file=sys.stderr)
    t0 = time.time()

    translator = BatchTranslator(
        xbe_path=args.xbe_path,
        func_json_path=data_files["functions"],
        labels_json_path=data_files.get("labels"),
        identified_json_path=data_files.get("identified"),
        abi_json_path=data_files.get("abi"),
        output_dir=args.output_dir,
        trace_functions=_load_addrs(args.trace_functions),
        force_returns=_parse_force_returns(args.force_return),
        seh_prolog=int(args.seh_prolog, 16) if args.seh_prolog else None,
        seh_epilog=int(args.seh_epilog, 16) if args.seh_epilog else None,
    )

    t_load = time.time() - t0
    print(f"Loaded {len(translator.func_db)} functions, "
          f"{len(translator.label_db)} labels, "
          f"{len(translator.classification_db)} classifications, "
          f"{len(translator.abi_db)} ABI entries "
          f"in {t_load:.1f}s", file=sys.stderr)

    # List categories mode
    if args.list_categories:
        list_categories(translator)
        return

    # Single function mode
    if args.function:
        addr = int(args.function, 16)
        code = translator.translate_single(addr)
        if code:
            print(code)
        else:
            print(f"ERROR: Could not translate function at 0x{addr:08X}",
                  file=sys.stderr)
            sys.exit(1)
        return

    # Generate header mode
    if args.header:
        output_dir = args.output_dir or os.path.join(
            os.path.dirname(__file__), "output")
        os.makedirs(output_dir, exist_ok=True)

        if args.game_only:
            funcs = translator.get_functions_by_category(
                categories={"game_engine", "game_vtable", "unknown"})
        elif args.category:
            funcs = translator.get_functions_by_category(
                categories={args.category})
        else:
            funcs = translator.get_functions_by_category()

        header_path = os.path.join(output_dir, "recomp_functions.h")
        generate_header(funcs, header_path, abi_db=translator.abi_db,
                        title=translator.title)
        print(f"Generated header: {header_path} ({len(funcs)} declarations)")
        return

    # Batch translation
    t0 = time.time()

    if args.category:
        categories = {args.category}
        funcs = translator.get_functions_by_category(categories=categories)
    elif args.game_only:
        # Game-specific functions only
        categories = {"game_engine", "game_vtable", "unknown"}
        funcs = translator.get_functions_by_category(categories=categories)
    elif args.all:
        funcs = translator.get_functions_by_category()
    else:
        # Default: game functions only
        categories = {"game_engine", "game_vtable", "unknown"}
        funcs = translator.get_functions_by_category(categories=categories)

    print(f"\nTranslating {len(funcs)} functions...", file=sys.stderr)

    if args.split:
        # Split output mode: multiple .c files + header + dispatch table
        gen_dir = args.gen_dir or os.path.join(
            os.path.dirname(os.path.dirname(os.path.dirname(__file__))),
            "src", "game", "recomp", "gen")

        if args.max_funcs:
            funcs = funcs[:args.max_funcs]

        manual = set()
        if args.manual_functions:
            with open(args.manual_functions, "r", encoding="utf-8") as fh:
                entries = json.load(fh)
            # Accept a bare list of addresses or the {addr: name} shape the
            # naming tools emit, so a project can point this at either.
            #
            # With {addr: name}, the name is binding: the hand-written C
            # defines that symbol, so the generated declaration and every call
            # site must use it regardless of what the function database calls
            # the address. Otherwise re-running the naming tools silently
            # renames the address, the manual definition no longer matches, and
            # the only symptom is a pile of unresolved externals at link time
            # that say nothing about naming.
            pinned = entries if isinstance(entries, dict) else {}
            if isinstance(entries, dict):
                entries = list(entries.keys())
            for e in entries:
                if isinstance(e, dict):
                    e = e.get("start") or e.get("address")
                manual.add(int(e, 16) if isinstance(e, str) else int(e))
            for key, pinned_name in pinned.items():
                if not pinned_name:
                    continue
                addr = int(key, 16) if isinstance(key, str) else int(key)
                entry = translator.func_db.get(addr)
                if entry is None:
                    print(f"  warning: manual function 0x{addr:08X} is not in "
                          f"the function database", file=sys.stderr)
                    continue
                if entry.get("name") != pinned_name:
                    print(f"  pinned 0x{addr:08X}: "
                          f"{entry.get('name')} -> {pinned_name}",
                          file=sys.stderr)
                entry["name"] = pinned_name
            print(f"Hand-written overrides: {len(manual)} functions will not "
                  f"be generated", file=sys.stderr)

        # --exclude-manual: derive the same `manual` set by scanning the C file
        # the project hand-writes, instead of a separate JSON that has to be
        # kept in sync with it. Everything below maps onto mechanisms already
        # used above -- the `manual` set (declare-only) and func_db name pinning
        # -- so the translator needs no changes.
        if args.exclude_manual:
            from .manual_scan import scan as _scan_manual
            skip, wrap, referenced = _scan_manual(args.exclude_manual)
            known = set(translator.func_db)

            # referenced-but-not-wrapped: the hand-written code names these as
            # sub_XXXXXXXX (declares or calls them), so they must keep that
            # name whatever a naming pass wanted. Otherwise the manual reference
            # is left undefined at link.
            pinned = 0
            for addr in (referenced & known) - wrap:
                info = translator.func_db[addr]
                plain = f"sub_{addr:08X}"
                if info.get("name") != plain:
                    info["name"] = plain
                    pinned += 1

            # wrap: recomp_manual.c defines sub_X itself and calls the generated
            # body as sub_X_gen. So do NOT add these to `manual` (the body is
            # still needed) -- just rename them so the emitted body is sub_X_gen.
            for addr in wrap & known:
                translator.func_db[addr]["name"] = f"sub_{addr:08X}_gen"

            # skip - wrap: defined by hand and not wrapped -> declare-only, which
            # is exactly what membership in `manual` produces.
            newly = {a for a in (skip - wrap) if a in known} - manual
            manual |= newly
            print(f"Excluding {len(newly)} functions defined in "
                  f"{args.exclude_manual}"
                  + (f"; {pinned} pinned to sub_ names" if pinned else "")
                  + (f"; {len(wrap & known)} wrapped as sub_X_gen" if wrap else ""),
                  file=sys.stderr)

        stats = translator.translate_batch_split(
            funcs,
            output_dir=gen_dir,
            chunk_size=args.split,
            verbose=args.verbose,
            manual=manual,
        )

        t_translate = time.time() - t0
        print(f"\n=== Split Translation Complete ({t_translate:.1f}s) ===",
              file=sys.stderr)
        print(f"{stats['translated']}/{stats['total']} functions "
              f"({stats['failed']} failed), "
              f"{stats['total_lines']} lines of C, "
              f"{stats['num_chunks']} source files",
              file=sys.stderr)
        if stats.get("unresolved_stubs"):
            print(f"{stats['unresolved_stubs']} unresolved call targets stubbed "
                  f"(addresses called but not detected as functions)",
                  file=sys.stderr)
        _report_unimplemented(stats)
        for f_path in stats.get("files", []):
            print(f"  {f_path}", file=sys.stderr)
    else:
        stats = translator.translate_batch(
            funcs,
            max_funcs=args.max_funcs,
            verbose=args.verbose,
        )

        t_translate = time.time() - t0

        print(f"\n=== Translation Complete ({t_translate:.1f}s) ===",
              file=sys.stderr)
        print_stats(stats)

    # Write summary
    output_dir = args.output_dir or os.path.join(
        os.path.dirname(__file__), "output")
    summary_path = write_summary(stats, output_dir)
    print(f"\nSummary: {summary_path}", file=sys.stderr)


if __name__ == "__main__":
    main()
