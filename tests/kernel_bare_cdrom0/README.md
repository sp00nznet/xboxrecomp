# Bare CdRom0 device path

Windows regression using a temporary directory only; no game assets.
Opens the bare `\Device\CdRom0` (no trailing separator) and expects the game directory.

```powershell
cmake -S tests/kernel_bare_cdrom0 -B build/bare-cdrom0 -A x64
cmake --build build/bare-cdrom0 --config Release --target kernel_bare_cdrom0_test
ctest --test-dir build/bare-cdrom0 -C Release --output-on-failure
```
