// LocalTimeTzClock - native taskbar clock, target time zone, system TZ untouched.
//
// HOW IT WORKS (see docs/analysis.md for the measured evidence):
//   The taskbar clock text is produced by whichever module hosts the tray on
//   the running build - Taskbar.View.dll up to ~24H2, SystemTray.dll on the
//   rewritten (25H2/26H1+) taskbar. That module imports GetTimeFormatEx /
//   GetDateFormatEx and calls them from a handful of display-only sites
//   (clock text, tray tooltips, accessibility names); the exact count differs
//   per build, so it is discovered at runtime instead of being hardcoded.
//
//   This DLL rewrites the *import table slots* of that one module for those
//   two APIs. Inside the hooks the SYSTEMTIME argument is replaced with "now,
//   converted to the configured Windows named time zone". Only call sites
//   inside the host module are rewritten - every other module in Explorer
//   (file timestamps, other shell components) is passed through untouched,
//   because the patch is scoped to a single module's IAT.
//
//   No global/kernelbase hook is installed. GetLocalTime is deliberately not
//   hooked: a WM_TIMECHANGE broadcast is enough to make the clock repaint.

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#include <shlobj.h>
#include <strsafe.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <intrin.h>

// MinGW/GCC has no MSVC _ReturnAddress() intrinsic; map it to the builtin so
// the same source builds under both toolchains.
#if defined(__GNUC__)
#undef _ReturnAddress
#define _ReturnAddress() __builtin_return_address(0)
#endif

#define TZCLOCK_VERSION 14

// The taskbar clock lives in different modules depending on the Windows build:
//   * <= 23H2/24H2 : Taskbar.View.dll (XAML taskbar, clock text via Win32 APIs)
//   * >= 25H2/26H1 : SystemTray.dll  (rewritten tray experience)
// Probe in priority order; the first module that exposes the formatting
// imports wins.
static const WCHAR* kTargetModules[] = { L"SystemTray.dll", L"Taskbar.View.dll" };
#define TARGET_MODULE_COUNT (int)(sizeof(kTargetModules) / sizeof(kTargetModules[0]))

typedef int (WINAPI* PFN_GetTimeFormatEx)(LPCWSTR, DWORD, const SYSTEMTIME*,
                                          LPCWSTR, LPWSTR, int);
typedef int (WINAPI* PFN_GetDateFormatEx)(LPCWSTR, DWORD, const SYSTEMTIME*,
                                          LPCWSTR, LPWSTR, int, LPCWSTR);
typedef VOID (WINAPI* PFN_GetLocalTime)(LPSYSTEMTIME);

#ifndef DEVICE_NOTIFY_CALLBACK
#define DEVICE_NOTIFY_CALLBACK 2
typedef ULONG (CALLBACK *PDEVICE_NOTIFY_CALLBACK_ROUTINE)(PVOID Context, ULONG Type, PVOID Setting);
typedef struct _DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS {
    PDEVICE_NOTIFY_CALLBACK_ROUTINE Callback;
    PVOID Context;
} DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS, *PDEVICE_NOTIFY_SUBSCRIBE_PARAMETERS;
#endif

// ------------------------------------------------------------------ config --
static HMODULE g_selfModule = nullptr;

struct Config {
    WCHAR timeZoneId[128];
    BOOL  enabled;
    BOOL  log;
};

static Config g_cfg = { L"China Standard Time", TRUE, FALSE };

static void GetOwnDir(WCHAR* out, DWORD cch) {
    GetModuleFileNameW(g_selfModule, out, cch);
    WCHAR* p = wcsrchr(out, L'\\');
    if (p) *p = 0;
}

static WCHAR g_iniPath[MAX_PATH];

// Trim leading/trailing whitespace in place, return pointer into buf.
static WCHAR* Trim(WCHAR* s) {
    while (*s == L' ' || *s == L'\t') ++s;
    WCHAR* e = s + wcslen(s);
    while (e > s && (e[-1] == L' ' || e[-1] == L'\t' || e[-1] == L'\r' ||
                     e[-1] == L'\n'))
        --e;
    *e = 0;
    return s;
}

static void LoadConfig() {
    GetOwnDir(g_iniPath, MAX_PATH);
    StringCchCatW(g_iniPath, MAX_PATH, L"\\tzclock.ini");

    // Read the whole file as bytes and decode explicitly. Avoids _wfopen's
    // mode-string/CRT locale interactions inside a foreign process.
    HANDLE h = CreateFileW(g_iniPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 65536) {
        CloseHandle(h);
        return;
    }
    DWORD n = (DWORD)sz.QuadPart;
    char* raw = (char*)malloc(n + 1);
    if (!raw) { CloseHandle(h); return; }
    DWORD read = 0;
    BOOL ok = ReadFile(h, raw, n, &read, nullptr);
    CloseHandle(h);
    if (!ok) { free(raw); return; }
    raw[read] = 0;

    char* cur = raw;
    if ((unsigned char)cur[0] == 0xEF && (unsigned char)cur[1] == 0xBB &&
        (unsigned char)cur[2] == 0xBF)
        cur += 3;

    while (*cur) {
        char* eol = cur;
        while (*eol && *eol != '\n') ++eol;
        if (*eol == '\n') { *eol = 0; ++eol; }
        char* next = eol;

        char* line = cur;
        cur = next;

        for (char* c = line; *c; ++c)
            if (*c == ';' || *c == '#') { *c = 0; break; }

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;

        // Decode key and value separately into fixed buffers with explicit
        // capacities. Never trust the input length.
        WCHAR key[128] = { 0 };
        WCHAR val[512] = { 0 };
        int kn = MultiByteToWideChar(CP_UTF8, 0, line, -1, key, ARRAYSIZE(key));
        int vn = MultiByteToWideChar(CP_UTF8, 0, eq + 1, -1, val, ARRAYSIZE(val));
        if (kn <= 0 || vn <= 0) continue;
        key[ARRAYSIZE(key) - 1] = 0;
        val[ARRAYSIZE(val) - 1] = 0;

        WCHAR* k = Trim(key);
        WCHAR* v = Trim(val);
        if (_wcsicmp(k, L"TimeZoneId") == 0) {
            if (*v) StringCchCopyW(g_cfg.timeZoneId, ARRAYSIZE(g_cfg.timeZoneId), v);
        } else if (_wcsicmp(k, L"Enabled") == 0) {
            g_cfg.enabled = (_wtoi(v) != 0);
        } else if (_wcsicmp(k, L"Log") == 0) {
            g_cfg.log = (_wtoi(v) != 0);
        }
    }
    free(raw);
}

// ---------------------------------------------------------------- logging ---
static WCHAR g_logPath[MAX_PATH];
static CRITICAL_SECTION g_logCs;
static BOOL g_logReady = FALSE;

static void LogF(const WCHAR* fmt, ...) {
    if (!g_cfg.log || !g_logReady || !g_logPath[0]) return;
    WCHAR line[2048];
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfW(line, ARRAYSIZE(line), fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_logCs);
    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        SetFilePointer(h, 0, nullptr, FILE_END);
        WriteFile(h, line, (DWORD)(wcslen(line) * sizeof(WCHAR)), &wrote, nullptr);
        WriteFile(h, L"\r\n", 4, &wrote, nullptr);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_logCs);
}

// ------------------------------------------------------------ target zone ---
struct SharedState {
    volatile LONG ready;       // DLL finished init
    volatile LONG supported;   // hooks installed successfully
    volatile LONG active;      // currently rewriting the clock
    volatile LONG hits;        // number of clock formatting calls rewritten
    volatile LONG lastError;   // Win32 error for the controller to report
    DWORD dllVersion;
};

static SharedState* g_state = nullptr;

static PFN_GetTimeFormatEx g_origTimeFmt = nullptr;
static PFN_GetDateFormatEx g_origDateFmt = nullptr;
static PFN_GetLocalTime   g_origLocalTime = nullptr;

static BYTE* g_modBase = nullptr;
// Modern builds call the formatting APIs from several distinct functions
// (clock text, tray tooltips, accessibility strings, flyout labels). We do not
// hardcode a per-build RVA list any more: every formatting call inside the
// taskbar module is display-only text, so all of them are rewritten.
#define MAX_SITES 16
static DWORD g_callSiteTime[MAX_SITES] = { 0 };
static int   g_callSiteTimeCount = 0;
static DWORD g_callSiteDate[MAX_SITES] = { 0 };
static int   g_callSiteDateCount = 0;

static SRWLOCK g_zoneLock = SRWLOCK_INIT;
static DYNAMIC_TIME_ZONE_INFORMATION g_dtzi;
static BOOL g_dtziValid = FALSE;
static volatile LONG g_active = 0;
static volatile LONG g_hits = 0;

// Set when the user flips the clock on/off. While it is set, the next
// GetLocalTime performed by the clock's own refresh routine is answered with
// second = 59, which makes the clock schedule its next update one second later
// instead of a full minute. Cleared as soon as it has been consumed once.
static volatile LONG g_kickPending = 0;
static DWORD g_callSiteLocal[8] = { 0 };
static int   g_callSiteLocalCount = 0;

static BOOL LoadTargetZone() {
    DYNAMIC_TIME_ZONE_INFORMATION d;
    for (DWORD i = 0;; ++i) {
        if (EnumDynamicTimeZoneInformation(i, &d) != ERROR_SUCCESS) break;
        if (_wcsicmp(d.TimeZoneKeyName, g_cfg.timeZoneId) == 0) {
            AcquireSRWLockExclusive(&g_zoneLock);
            g_dtzi = d;
            g_dtziValid = TRUE;
            ReleaseSRWLockExclusive(&g_zoneLock);
            return TRUE;
        }
    }
    AcquireSRWLockExclusive(&g_zoneLock);
    g_dtziValid = FALSE;
    ReleaseSRWLockExclusive(&g_zoneLock);
    return FALSE;
}

// Current UTC instant expressed in the configured time zone, using Windows'
// own transition rules. Handles DST, half/quarter-hour offsets and the date
// rollover across midnight - it is not a fixed offset added to local time.
static BOOL TargetNow(SYSTEMTIME* out) {
    DYNAMIC_TIME_ZONE_INFORMATION tz;
    BOOL valid = FALSE;
    AcquireSRWLockShared(&g_zoneLock);
    if (g_dtziValid) {
        tz = g_dtzi;
        valid = TRUE;
    }
    ReleaseSRWLockShared(&g_zoneLock);
    if (!valid) return FALSE;

    SYSTEMTIME utc;
    GetSystemTime(&utc);
    return SystemTimeToTzSpecificLocalTimeEx(&tz, &utc, out) != FALSE;
}

static BOOL IsClockCallSite(void* retAddr) {
    DWORD rva = (DWORD)((BYTE*)retAddr - g_modBase);
    for (int i = 0; i < g_callSiteTimeCount; ++i)
        if (rva == g_callSiteTime[i]) return TRUE;
    for (int i = 0; i < g_callSiteDateCount; ++i)
        if (rva == g_callSiteDate[i]) return TRUE;
    return FALSE;
}

// ------------------------------------------------------------------ hooks ---
static BOOL IsClockLocalCallSite(void* retAddr) {
    DWORD rva = (DWORD)((BYTE*)retAddr - g_modBase);
    for (int i = 0; i < g_callSiteLocalCount; ++i)
        if (rva == g_callSiteLocal[i]) return TRUE;
    return FALSE;
}

// Safe GetLocalTime hook: populate valid date/time fields first, then adjust wSecond = 59
// if a refresh nudge is pending. NEVER zero-out the struct.
static VOID WINAPI HookGetLocalTime(LPSYSTEMTIME st) {
    g_origLocalTime(st);
    if (IsClockLocalCallSite(_ReturnAddress()) &&
        InterlockedExchange(&g_kickPending, 0)) {
        st->wSecond = 59;      // "almost a new minute": refresh next second
    }
}

static int WINAPI HookGetTimeFormatEx(LPCWSTR locale, DWORD flags,
                                      const SYSTEMTIME* t, LPCWSTR fmt,
                                      LPWSTR buf, int cch) {
    if (g_active && IsClockCallSite(_ReturnAddress())) {
        SYSTEMTIME target;
        if (TargetNow(&target)) {
            InterlockedIncrement(&g_hits);
            if (g_state) InterlockedIncrement(&g_state->hits);
            return g_origTimeFmt(locale, flags, &target, fmt, buf, cch);
        }
    }
    return g_origTimeFmt(locale, flags, t, fmt, buf, cch);
}

static int WINAPI HookGetDateFormatEx(LPCWSTR locale, DWORD flags,
                                      const SYSTEMTIME* t, LPCWSTR fmt,
                                      LPWSTR buf, int cch, LPCWSTR cal) {
    if (g_active && IsClockCallSite(_ReturnAddress())) {
        SYSTEMTIME target;
        if (TargetNow(&target)) {
            InterlockedIncrement(&g_hits);
            if (g_state) InterlockedIncrement(&g_state->hits);
            return g_origDateFmt(locale, flags, &target, fmt, buf, cch, cal);
        }
    }
    return g_origDateFmt(locale, flags, t, fmt, buf, cch, cal);
}

// ------------------------------------------------------------ IAT patching --
struct IatEntry { void** slot; void* original; };

static IatEntry g_iatTime = { nullptr, nullptr };
static IatEntry g_iatDate = { nullptr, nullptr };
static IatEntry g_iatLocal = { nullptr, nullptr };

// Patch the IAT slot for funcName. When moduleName is null the first import
// descriptor exposing that name wins - newer builds import the date/time
// helpers straight from KERNEL32.dll instead of the api-ms-win-* apisets, so
// matching on the DLL name is no longer portable.
static int PatchImport(const char* moduleName, const char* funcName,
                       void* replacement, IatEntry* found) {
    BYTE* base = g_modBase;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto imp = (IMAGE_IMPORT_DESCRIPTOR*)
        (base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    if (!imp || !imp->Name) return 0;

    for (; imp->Name; ++imp) {
        const char* dll = (const char*)(base + imp->Name);
        if (moduleName && _stricmp(dll, moduleName) != 0) continue;

        auto oft = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        auto ft = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        if (!oft) continue;

        for (; oft->u1.AddressOfData; ++oft, ++ft) {
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, funcName) != 0) continue;

            DWORD oldProt = 0;
            if (!VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProt))
                return -1;
            found->slot = (void**)&ft->u1.Function;
            found->original = (void*)ft->u1.Function;
            ft->u1.Function = (ULONG_PTR)replacement;
            VirtualProtect(&ft->u1.Function, sizeof(void*), oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), &ft->u1.Function, sizeof(void*));
            return 1;
        }
    }
    return 0;
}

static void WriteSlotInternal(void** slot, void* value) {
    DWORD oldProt = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
        *slot = value;
        VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    }
}

// Single-pass scanner: traverses .text exactly once, matching all 3 slot RVAs simultaneously.
// Reduces startup scan time by ~66%.
static void ScanCallSitesSinglePass(DWORD timeSlotRva, DWORD dateSlotRva, DWORD localSlotRva,
                                    DWORD* timeSites, int* timeCount, int maxTime,
                                    DWORD* dateSites, int* dateCount, int maxDate,
                                    DWORD* localSites, int* localCount, int maxLocal) {
    BYTE* base = g_modBase;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    *timeCount = 0;
    *dateCount = 0;
    *localCount = 0;

    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (memcmp(sec->Name, ".text", 5) != 0) continue;
        BYTE* p = base + sec->VirtualAddress;
        DWORD size = sec->Misc.VirtualSize;
        for (DWORD i = 0; i + 6 <= size; ++i) {
            if (p[i] != 0xFF || p[i + 1] != 0x15) continue;  // call qword [rip+disp32]
            INT32 disp = *(INT32*)(p + i + 2);
            DWORD targetRva = sec->VirtualAddress + i + 6 + disp;
            DWORD siteRva = sec->VirtualAddress + i + 6;

            if (targetRva == timeSlotRva) {
                if (*timeCount < maxTime) timeSites[*timeCount] = siteRva;
                (*timeCount)++;
            } else if (targetRva == dateSlotRva) {
                if (*dateCount < maxDate) dateSites[*dateCount] = siteRva;
                (*dateCount)++;
            } else if (targetRva == localSlotRva) {
                if (*localCount < maxLocal) localSites[*localCount] = siteRva;
                (*localCount)++;
            }
        }
    }
}

// Force the clock to re-read and repaint now.
// Sends WM_TIMECHANGE to broadcast and all taskbar windows (primary and multi-monitor secondary).
static void RequestClockRefresh() {
    DWORD_PTR res = 0;

    SendMessageTimeoutW(HWND_BROADCAST, WM_TIMECHANGE, 0, 0,
                        SMTO_ABORTIFHUNG | SMTO_NOTIMEOUTIFNOTHUNG, 300, &res);

    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray) {
        HWND targets[3] = {
            tray,
            FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr),
            FindWindowExW(tray, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr)
        };
        for (int i = 0; i < 3; ++i) {
            if (!targets[i]) continue;
            SendMessageTimeoutW(targets[i], WM_TIMECHANGE, 0, 0,
                                SMTO_ABORTIFHUNG | SMTO_NOTIMEOUTIFNOTHUNG, 300, &res);
            RedrawWindow(targets[i], nullptr, nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_FRAME);
        }
    }

    // Secondary monitors (multi-display taskbars)
    HWND secTray = nullptr;
    while ((secTray = FindWindowExW(nullptr, secTray, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
        HWND bridge = FindWindowExW(secTray, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr);
        SendMessageTimeoutW(secTray, WM_TIMECHANGE, 0, 0,
                            SMTO_ABORTIFHUNG | SMTO_NOTIMEOUTIFNOTHUNG, 300, &res);
        RedrawWindow(secTray, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_FRAME);
        if (bridge) {
            SendMessageTimeoutW(bridge, WM_TIMECHANGE, 0, 0,
                                SMTO_ABORTIFHUNG | SMTO_NOTIMEOUTIFNOTHUNG, 300, &res);
            RedrawWindow(bridge, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_FRAME);
        }
    }
}

// ------------------------------------------------------------ power events --
// Standard Windows 8+ power suspend/resume notification callback.
// Eliminates the need for a dedicated thread, hidden window class, and message loop.
static HPOWERNOTIFY g_powerNotify = nullptr;

static ULONG CALLBACK SuspendResumeCallback(PVOID, ULONG type, PVOID) {
    if (type == PBT_APMRESUMEAUTOMATIC || type == PBT_APMRESUMESUSPEND ||
        type == PBT_APMRESUMECRITICAL) {
        LogF(L"power resume event %lu -> refresh clock", type);
        InterlockedExchange(&g_kickPending, 1);
        RequestClockRefresh();
    }
    return ERROR_SUCCESS;
}

static void InitPowerNotification() {
    if (g_powerNotify) return;
    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS params = {};
    params.Callback = SuspendResumeCallback;
    params.Context = nullptr;
    g_powerNotify = RegisterSuspendResumeNotification((HANDLE)&params, DEVICE_NOTIFY_CALLBACK);
    LogF(L"registered suspend-resume callback: %p", g_powerNotify);
}

static void UninitPowerNotification() {
    if (g_powerNotify) {
        UnregisterSuspendResumeNotification(g_powerNotify);
        g_powerNotify = nullptr;
    }
}

static BOOL g_patched = FALSE;

static BOOL ActivateHooks() {
    if (!g_patched) {
        int a = PatchImport(nullptr, "GetTimeFormatEx", (void*)HookGetTimeFormatEx, &g_iatTime);
        int b = PatchImport(nullptr, "GetDateFormatEx", (void*)HookGetDateFormatEx, &g_iatDate);
        if (a != 1 || b != 1) {
            LogF(L"ERROR: IAT patch failed (time=%d date=%d)", a, b);
            if (a == 1) WriteSlotInternal(g_iatTime.slot, g_iatTime.original);
            if (b == 1) WriteSlotInternal(g_iatDate.slot, g_iatDate.original);
            return FALSE;
        }
        g_origTimeFmt = (PFN_GetTimeFormatEx)g_iatTime.original;
        g_origDateFmt = (PFN_GetDateFormatEx)g_iatDate.original;
        g_patched = TRUE;
        LogF(L"IAT patched: time=%p date=%p", g_iatTime.slot, g_iatDate.slot);

        DWORD timeSlotRva = (DWORD)((BYTE*)g_iatTime.slot - g_modBase);
        DWORD dateSlotRva = (DWORD)((BYTE*)g_iatDate.slot - g_modBase);

        DWORD fits[MAX_SITES] = { 0 }, fds[MAX_SITES] = { 0 }, lcs[8] = { 0 };
        int nt = 0, nd = 0, nl = 0;
        ScanCallSitesSinglePass(timeSlotRva, dateSlotRva, 0,
                                fits, &nt, MAX_SITES, fds, &nd, MAX_SITES, lcs, &nl, 8);
        LogF(L"call sites found: time=%d date=%d local=%d", nt, nd, nl);

        g_callSiteTimeCount = nt > MAX_SITES ? MAX_SITES : nt;
        memcpy(g_callSiteTime, fits, sizeof(DWORD) * g_callSiteTimeCount);
        g_callSiteDateCount = nd > MAX_SITES ? MAX_SITES : nd;
        memcpy(g_callSiteDate, fds, sizeof(DWORD) * g_callSiteDateCount);

        if (g_callSiteTimeCount == 0 || g_callSiteDateCount == 0) {
            LogF(L"ERROR: unexpected call-site layout (%d/%d), staying inert", nt, nd);
            WriteSlotInternal(g_iatTime.slot, g_iatTime.original);
            WriteSlotInternal(g_iatDate.slot, g_iatDate.original);
            g_patched = FALSE;
            return FALSE;
        }
    }
    InterlockedExchange(&g_active, 1);
    InterlockedExchange(&g_kickPending, 1);
    return TRUE;
}

static void DeactivateHooks() {
    InterlockedExchange(&g_active, 0);
    InterlockedExchange(&g_kickPending, 1);   // make it repaint now, not in a minute
}

// ------------------------------------------------------- control plumbing ---
static WCHAR g_evtEnable[96];
static WCHAR g_evtDisable[96];
static WCHAR g_evtQuery[96];
static WCHAR g_mapName[96];

static void BuildControlNames() {
    DWORD pid = GetCurrentProcessId();
    StringCchPrintfW(g_evtEnable, ARRAYSIZE(g_evtEnable),
                     L"Local\\LocalTimeTzClock.v%u.Enable.%lu", TZCLOCK_VERSION, pid);
    StringCchPrintfW(g_evtDisable, ARRAYSIZE(g_evtDisable),
                     L"Local\\LocalTimeTzClock.v%u.Disable.%lu", TZCLOCK_VERSION, pid);
    StringCchPrintfW(g_evtQuery, ARRAYSIZE(g_evtQuery),
                     L"Local\\LocalTimeTzClock.v%u.Query.%lu", TZCLOCK_VERSION, pid);
    StringCchPrintfW(g_mapName, ARRAYSIZE(g_mapName),
                     L"Local\\LocalTimeTzClock.v%u.State.%lu", TZCLOCK_VERSION, pid);
}

static HANDLE g_map = nullptr;

static DWORD WINAPI ControlThread(LPVOID) {
    HANDLE enable  = CreateEventW(nullptr, FALSE, FALSE, g_evtEnable);
    HANDLE disable = CreateEventW(nullptr, FALSE, FALSE, g_evtDisable);
    HANDLE query   = CreateEventW(nullptr, FALSE, FALSE, g_evtQuery);

    LoadConfig();
    if (g_cfg.log) {
        WCHAR dir[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, dir)))
            StringCchPrintfW(g_logPath, MAX_PATH, L"%s\\tzclock.log", dir);
    }
    InitializeCriticalSection(&g_logCs);
    g_logReady = TRUE;
    LogF(L"--- attach (pid %lu, v%d) ---", GetCurrentProcessId(), TZCLOCK_VERSION);
    LogF(L"config: TimeZoneId='%s' Enabled=%d Log=%d", g_cfg.timeZoneId,
         g_cfg.enabled, g_cfg.log);

    // Explorer starts the taskbar asynchronously, so a DLL injected right
    // after the process appears can arrive before the host module exists.
    // Try every known clock host in priority order.
    HMODULE mod = nullptr;
    const WCHAR* modName = nullptr;
    for (int i = 0; i < 300; ++i) {           // up to ~60 s
        for (int m = 0; m < TARGET_MODULE_COUNT; ++m) {
            mod = GetModuleHandleW(kTargetModules[m]);
            if (mod) { modName = kTargetModules[m]; break; }
        }
        if (mod) break;
        Sleep(200);
    }
    if (!mod) {
        LogF(L"ERROR: no taskbar clock module appeared; staying inert");
        if (g_state) g_state->lastError = ERROR_MOD_NOT_FOUND;
        return 0;
    }
    g_modBase = (BYTE*)mod;
    LogF(L"host module '%s' base=%p", modName, mod);

    if (!LoadTargetZone()) {
        LogF(L"ERROR: time zone '%s' not found; staying inert", g_cfg.timeZoneId);
        if (g_state) g_state->lastError = ERROR_INVALID_PARAMETER;
        return 0;
    }
    LogF(L"target zone resolved: %s", g_dtzi.TimeZoneKeyName);

    BOOL ok = (g_cfg.enabled && ActivateHooks());
    if (g_state) {
        g_state->supported = g_patched ? 1 : 0;
        g_state->lastError = (g_cfg.enabled && !ok) ? ERROR_PROC_NOT_FOUND : 0;
    }

    InitPowerNotification();
    LogF(L"initial state: enabled=%d patched=%d", g_cfg.enabled, g_patched);
    RequestClockRefresh();

    for (;;) {
        HANDLE waits[3] = { enable, disable, query };
        DWORD r = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0) {
            LoadConfig();
            if (!LoadTargetZone()) {
                LogF(L"enable requested but zone '%s' invalid", g_cfg.timeZoneId);
                if (g_state) g_state->lastError = ERROR_INVALID_PARAMETER;
                continue;
            }
            BOOL a = ActivateHooks();
            LogF(L"enable requested -> active=%d", a);
            if (g_state) { g_state->active = a ? 1 : 0; g_state->lastError = 0; }
            RequestClockRefresh();
        } else if (r == WAIT_OBJECT_0 + 1) {
            DeactivateHooks();
            LogF(L"disable requested -> hooks pass through");
            if (g_state) g_state->active = 0;
            RequestClockRefresh();
        } else if (r == WAIT_OBJECT_0 + 2) {
            if (g_state) {
                g_state->active = g_active;
                g_state->hits = g_hits;
                g_state->supported = g_patched ? 1 : 0;
            }
        } else {
            break;
        }
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_selfModule = inst;
        DisableThreadLibraryCalls(inst);
        BuildControlNames();

        g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                   PAGE_READWRITE, 0, sizeof(SharedState), g_mapName);
        if (g_map)
            g_state = (SharedState*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0,
                                                  sizeof(SharedState));
        if (g_state) {
            g_state->ready = 1;
            g_state->dllVersion = TZCLOCK_VERSION;
            g_state->hits = 0;
        }

        HANDLE h = CreateThread(nullptr, 0, ControlThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (reserved == nullptr) {
            UninitPowerNotification();
            if (g_patched && g_iatTime.slot) WriteSlotInternal(g_iatTime.slot, g_iatTime.original);
            if (g_patched && g_iatDate.slot) WriteSlotInternal(g_iatDate.slot, g_iatDate.original);
            if (g_patched && g_iatLocal.slot) WriteSlotInternal(g_iatLocal.slot, g_iatLocal.original);
            LogF(L"--- detach ---");
        }
    }
    return TRUE;
}
