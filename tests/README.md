# Tests

- `ctest --test-dir build -C Release --output-on-failure` runs the fast self-test.
- `tests\run_sample.cmd` runs a full sample conversion + K4A playback/hash verification.
  It uses `powershell.exe -ExecutionPolicy Bypass` internally, so it works even when
  the machine's execution policy blocks unsigned `.ps1` files.
  By default it writes **BGRA 1080p + ORIGINAL_COLOR_MJPG** (~12.5 GB) and verifies
  decoded pixels + source JPEG hashes. Use `-Color mjpg` for the smaller (~1.2 GB)
  MJPEG-only output, or `-Keep` to keep the output file.

If you prefer PowerShell directly:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\run_sample.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\run_sample.ps1 -Color mjpg
# or, for the current process only:
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\tests\run_sample.ps1
```
