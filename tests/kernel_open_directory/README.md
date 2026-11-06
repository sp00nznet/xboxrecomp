# Opening an existing directory without FILE_DIRECTORY_FILE

Windows regression using a temporary directory only; no game assets.

```powershell
cmake -S tests/kernel_open_directory -B build/open-dir -A x64
cmake --build build/open-dir --config Release --target kernel_open_directory_test
ctest --test-dir build/open-dir -C Release --output-on-failure
```
