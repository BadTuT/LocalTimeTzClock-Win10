# Build / dev notes

## Why the DLL file name is versioned

A DLL that is loaded inside `explorer.exe` cannot be overwritten. Since we do
not want to kill Explorer every time the DLL changes, `build.bat` writes
`tzclock_vN.dll` and the controller is compiled with that exact name
(`/DTZCLOCK_DLL_NAME=...`). Bump `DLLNAME` in `build.bat` after changing the
DLL, then it links into a fresh file.

## Debugging the DLL without crashing Explorer

`src/app/host.cpp` loads the DLL into a throwaway process:

```bat
host.exe tzclock_v11.dll
```

If the DLL faults during initialization, this shows it there instead of taking
the user's shell down. Two real bugs were found this way:

1. `g_origTimeFmt` / `g_origDateFmt` were never populated, so the first clock
   refresh called a null pointer (`0xC0000005`, offset 0).
2. The INI parser overflowed a stack buffer and tripped `/GS`
   (`0xC0000409`, offset 0xAF30). Replaced with explicit-capacity reads.

## Static analysis helpers

`tools/pe.py` is a small PE parser (imports, exports, pdata, section map).
Used to prove, on the actual shipped `Taskbar.View.dll`, that only two
functions reference the clock formatting APIs, and to locate the exact call
sites. See `docs/analysis.md`.
