# tools.diff

Compares two checkpoint traces (oracle vs recompiled) for one bounded guest
case and reports the first register or memory divergence.

    python -m tools.diff cases/example --oracle oracle.json --recomp recomp.json
    python -m tools.diff.selfcheck

A case (`case.json`) holds the code bytes, entry EIP, initial registers, memory
and a stop condition. The oracle can be a file, a command (`--oracle-command`,
given `XBOXRECOMP_DIFF_CASE` / `XBOXRECOMP_DIFF_OUT`), or the optional Unicorn
runner (`--run-oracle`, needs `pip install unicorn`; it is imported lazily and
never substituted if missing). Use the Python unit tests for the comparator:
`py -3 -m pytest tools/diff`.
