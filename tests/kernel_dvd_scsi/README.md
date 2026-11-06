# DVD SCSI pass-through

Windows regression using synthetic guest memory only; no game assets.
Sends a READ DVD STRUCTURE (0xAD) pass-through to the synthetic DVD handle and
checks the status is GOOD and the DataBuffer is zeroed.

```powershell
cmake -S tests/kernel_dvd_scsi -B build/dvd-scsi -A x64
cmake --build build/dvd-scsi --config Release --target kernel_dvd_scsi_test
ctest --test-dir build/dvd-scsi -C Release --output-on-failure
```
