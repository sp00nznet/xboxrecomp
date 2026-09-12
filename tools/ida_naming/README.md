# IDA Naming Path (Xbox XBE)

The same job as `tools/ghidra_naming/`, through IDA instead: recover real
function and symbol names from an XBE and turn them into the
`{address: name}` map the static recompiler consumes.

Only the *export* differs. The merge, the placeholder filter, the C-identifier
sanitising, the collision handling and `--apply` are
`tools/ghidra_naming/merge_names.py`, which reads the same two JSON files
whichever tool wrote them.

```
tools/ida_naming/
  export_xbe_names.py    # runs inside IDA; writes functions.json + symbols.json
  export/                # generated
```

## Why you might want this rather than Ghidra

IDA's FLIRT signature library and Ghidra's FidDb are the same idea with
different coverage, and neither is a superset. A CRT or SDK version one of them
recognises and the other does not is the normal case, not the exception, so
running both and taking the union names more functions than either alone — the
same result `symbols/map_names` measures for donor MAP files (735 named from
two donors versus 504 from the best single one).

If you only have one of the two installed, use that one. This exists so having
IDA is not a reason to be stuck.

## Loading the XBE

IDA ships no XBE loader either. Reuse the flat image the Ghidra path already
builds — it is a plain binary with nothing Ghidra-specific in it:

```
py -3 tools/ghidra_naming/extract_for_ghidra.py /path/to/default.xbe \
    --out-dir tools/ghidra_naming/work
```

Load `work/xbe_flat.bin` in IDA as **Binary file**, processor `metapc`,
32-bit, **loading offset `0x10000`**. Because the flat image starts at the XBE
base, IDA's addresses equal the recompiler's `functions.json` `start` values
exactly, which is the only property that matters when the names merge back.

The exporter checks this and refuses to write at the wrong base. It has to:
names at the wrong base merge onto nothing, and the failure is silent — a
healthy-looking export count followed by "0 addresses matching".

## Exporting

From the GUI, **File > Script file...** and pick `export_xbe_names.py`, or
headless:

```
ida -A -S"tools/ida_naming/export_xbe_names.py tools/ida_naming/export --exit" \
    xbe_flat.i64
```

Wait for auto-analysis to finish first (the script calls `auto_wait()`, but a
7.6 MB image takes a while either way).

## Merging

```
py -3 tools/ghidra_naming/merge_names.py \
    --export-dir tools/ida_naming/export \
    --out tools/ida_naming/ida_names.json
```

Add `--apply` to write the names into `tools/disasm/output/functions.json` in
place (a `.bak` is written first). Regenerate afterwards and the emitted C uses
them.

To take the union of an IDA run and a Ghidra run, `--apply` both, one after
the other. `--apply` only sets names on entries whose address matches, so the
second pass fills in what the first left as `sub_`; whichever you run last wins
where they disagree. The bucket counts in each report say which tool found what.
