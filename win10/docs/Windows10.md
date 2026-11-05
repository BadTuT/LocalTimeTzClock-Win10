# Windows 10 taskbar clock adaptation

## Target

- Windows 10 Pro 22H2, OS build 19045.2728, x64.
- Taskbar host: `explorer.exe`, file version `10.0.19041.2311`.
- Controller and DLL version: 17.
- Default display time zone: `China Standard Time`.

## Display scope

The main taskbar clock uses Beijing time for its time, date and weekday.
Windows retains its configured system time zone and its actual UTC clock.
Other applications, the native tooltip and additional clocks retain their
original time sources. Clicking the clock continues to open the Windows
calendar panel, which retains its original implementation.

## Implementation

Microsoft's public symbols identify
`ClockButton::UpdateTextStringsIfNecessary(bool*)` at RVA `0x6EA3C`.
Its `GetLocalTime` call returns at RVA `0x6EA6A`. The function uses that
single `SYSTEMTIME` for its time, date and weekday formatting. The existing
`STRATEGY_LOCAL_SOURCE` replaces this sample with the configured target time.
The remaining six `GetLocalTime` call sites retain their original behavior.
The time and date formatting import slots retain their original functions.
Explorer's API-set imports are compared with the KernelBase implementations;
the DLL's Kernel32 entry stubs have separate addresses on this Win10 build.

The module is added to the existing layout selection and import/call-site
matching logic. Win10 clock windows receive the existing `WM_TIMECHANGE`
refresh message directly for immediate enable/disable updates.

## Build and use

Build from an installed Visual Studio x64 C++ toolchain and Windows SDK:

```bat
build.bat dist
```

Keep `tzclock.exe`, `tzclock_v17.dll` and `tzclock.ini` in one folder.

```powershell
.\tzclock.exe on
.\tzclock.exe status
.\tzclock.exe off
```

`on` enables the target time-zone display. `off` returns the display to the
system time zone. The existing `install` command registers login startup;
the existing `uninstall` command removes that startup registration.

This adaptation targets the module version above. Additional Explorer
versions require their own layout entries.
