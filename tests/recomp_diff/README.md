# Checkpoint ring

Synthetic registers only; no game assets. Checks record fields, the little-endian dump layout read by `tools/diff/checkpoint_decode.py`, and ring wrap.

```powershell
cmake -S tests/recomp_diff -B build/recomp-diff -A x64
cmake --build build/recomp-diff --config Release --target recomp_diff_test
ctest --test-dir build/recomp-diff -C Release --output-on-failure
```
