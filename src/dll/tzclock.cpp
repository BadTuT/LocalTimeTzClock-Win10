// LocalTimeTzClock - native taskbar clock, target time zone, system TZ untouched.
//
// HOW IT WORKS (see docs/analysis.md for the measured evidence):
//   The native taskbar clock text is produced inside Taskbar.View.dll by
//   exactly one formatting function (RVA 0x34BA4 on 623.27903.70.0) plus one
//   time-formatting helper it calls (RVA 0x35960). Those two functions are the
//   only code in the whole module that reference GetTimeFormatEx /
//   GetDateFormatEx. This DLL rewrites the *import table slots* of
//   Taskbar.View.dll for those two APIs and, inside the hooks, replaces the
//   SYSTEMTIME argument with "now, converted to the configured Windows named
//   time zone" - but only when the return address is one of the three known
//   clock call sites. Every other caller of those APIs (and every other module
//   in Explorer, including file timestamps) is passed through untouched.
//
//   No global/kernelbase hook is installed. Nothing outside the taskbar clock
//   formatting path is affected.

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shlobj.h>
#include <strsafe.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <intrin.h>

typedef int (WINAPI* PFN_GetTimeFormatEx)(LPCWSTR, DWORD, const SYSTEMTIME*,
                                          LPCWSTR, LPWSTR, int);
typedef int (WINAPI* PFN_GetDateFormatEx)(LPCWSTR, DWORD, const SYSTEMTIME*,
                                          LPCWSTR, LPWSTR, int, LPCWSTR);

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
static PFN_GetTimeFormatEx g_origTimeFmt = nullptr;
static PFN_GetDateFormatEx g_origDateFmt = nullptr;

static BYTE* g_modBase = nullptr;
static DWORD g_callSiteTime = 0;
static DWORD g_callSiteDate[2] = { 0, 0 };
static int   g_callSiteDateCount = 0;

static DYNAMIC_TIME_ZONE_INFORMATION g_dtzi;
static BOOL g_dtziValid = FALSE;
static volatile LONG g_active = 0;
static volatile LONG g_hits = 0;

static BOOL LoadTargetZone() {
    for (DWORD i = 0;; ++i) {
        DYNAMIC_TIME_ZONE_INFORMATION d;
        if (EnumDynamicTimeZoneInformation(i, &d) != ERROR_SUCCESS) break;
        if (_wcsicmp(d.TimeZoneKeyName, g_cfg.timeZoneId) == 0) {
            g_dtzi = d;
            g_dtziValid = TRUE;
            return TRUE;
        }
    }
    g_dtziValid = FALSE;
    return FALSE;
}

// Current UTC instant expressed in the configured time zone, using Windows'
// own transition rules. Handles DST, half/quarter-hour offsets and the date
// rollover across midnight - it is not a fixed offset added to local time.
static BOOL TargetNow(SYSTEMTIME* out) {
    if (!g_dtziValid) return FALSE;
    SYSTEMTIME utc;
    GetSystemTime(&utc);
    return SystemTimeToTzSpecificLocalTimeEx(&g_dtzi, &utc, out) != FALSE;
}

static BOOL IsClockCallSite(void* retAddr) {
    DWORD rva = (DWORD)((BYTE*)retAddr - g_modBase);
    if (g_callSiteTime && rva == g_callSiteTime) return TRUE;
    for (int i = 0; i < g_callSiteDateCount; ++i)
        if (rva == g_callSiteDate[i]) return TRUE;
    return FALSE;
}

// ------------------------------------------------------------------ hooks ---
static int WINAPI HookGetTimeFormatEx(LPCWSTR locale, DWORD flags,
                                      const SYSTEMTIME* t, LPCWSTR fmt,
                                      LPWSTR buf, int cch) {
    if (InterlockedCompareExchange(&g_active, 0, 0) && IsClockCallSite(_ReturnAddress())) {
        SYSTEMTIME target;
        if (TargetNow(&target)) {
            InterlockedIncrement(&g_hits);
            return g_origTimeFmt(locale, flags, &target, fmt, buf, cch);
        }
    }
    return g_origTimeFmt(locale, flags, t, fmt, buf, cch);
}

static int WINAPI HookGetDateFormatEx(LPCWSTR locale, DWORD flags,
                                      const SYSTEMTIME* t, LPCWSTR fmt,
                                      LPWSTR buf, int cch, LPCWSTR cal) {
    if (InterlockedCompareExchange(&g_active, 0, 0) && IsClockCallSite(_ReturnAddress())) {
        SYSTEMTIME target;
        if (TargetNow(&target)) {
            InterlockedIncrement(&g_hits);
            return g_origDateFmt(locale, flags, &target, fmt, buf, cch, cal);
        }
    }
    return g_origDateFmt(locale, flags, t, fmt, buf, cch, cal);
}

// ------------------------------------------------------------ IAT patching --
struct IatEntry { void** slot; void* original; };

static IatEntry g_iatTime = { nullptr, nullptr };
static IatEntry g_iatDate = { nullptr, nullptr };

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
        if (_stricmp(dll, moduleName) != 0) continue;

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

// Fallback for a Taskbar.View.dll version whose layout was not verified: find
// the call instructions that go through the patched IAT slots.
static int ScanCallSites(DWORD iatSlotRva, DWORD* out, int maxOut) {
    BYTE* base = g_modBase;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    int n = 0;
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (memcmp(sec->Name, ".text", 5) != 0) continue;
        BYTE* p = base + sec->VirtualAddress;
        DWORD size = sec->Misc.VirtualSize;
        for (DWORD i = 0; i + 6 <= size; ++i) {
            if (p[i] != 0xFF || p[i + 1] != 0x15) continue;  // call qword [rip+d]
            INT32 disp = *(INT32*)(p + i + 2);
            DWORD targetRva = sec->VirtualAddress + i + 6 + disp;
            if (targetRva != iatSlotRva) continue;
            if (n < maxOut) out[n] = sec->VirtualAddress + i + 6;
            ++n;
        }
    }
    return n;
}

// Ask the shell to repaint the clock right away instead of waiting for the
// next natural minute tick. WM_TIMECHANGE changes nothing; it is the
// documented notification apps use to refresh displayed time.
static void RequestClockRefresh() {
    DWORD_PTR res = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_TIMECHANGE, 0, 0,
                        SMTO_ABORTIFHUNG | SMTO_NOTIMEOUTIFNOTHUNG, 200, &res);
}

static BOOL g_patched = FALSE;

static BOOL ActivateHooks() {
    if (!g_patched) {
        int a = PatchImport("api-ms-win-core-datetime-l1-1-1.dll",
                            "GetTimeFormatEx", (void*)HookGetTimeFormatEx, &g_iatTime);
        int b = PatchImport("api-ms-win-core-datetime-l1-1-1.dll",
                            "GetDateFormatEx", (void*)HookGetDateFormatEx, &g_iatDate);
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
        DWORD fits[8] = { 0 }, fds[8] = { 0 };
        int nt = ScanCallSites(timeSlotRva, fits, 8);
        int nd = ScanCallSites(dateSlotRva, fds, 8);
        LogF(L"call sites found: time=%d date=%d", nt, nd);
        if (nt == 1 && nd >= 1) {
            g_callSiteTime = fits[0];
            memcpy(g_callSiteDate, fds, sizeof(DWORD) * (nd > 2 ? 2 : nd));
            g_callSiteDateCount = (nd > 2 ? 2 : nd);
            LogF(L"call sites: time=%08X date0=%08X date1=%08X", g_callSiteTime,
                 g_callSiteDate[0], g_callSiteDateCount > 1 ? g_callSiteDate[1] : 0);
        } else {
            LogF(L"ERROR: unexpected call-site layout (%d/%d), staying inert", nt, nd);
            WriteSlotInternal(g_iatTime.slot, g_iatTime.original);
            WriteSlotInternal(g_iatDate.slot, g_iatDate.original);
            g_patched = FALSE;
            return FALSE;
        }
    }
    InterlockedExchange(&g_active, 1);
    return TRUE;
}

static void DeactivateHooks() {
    InterlockedExchange(&g_active, 0);
}

// ------------------------------------------------------- control plumbing ---
#define EVT_ENABLE  L"Local\\LocalTimeTzClock.Enable"
#define EVT_DISABLE L"Local\\LocalTimeTzClock.Disable"
#define EVT_QUERY   L"Local\\LocalTimeTzClock.Query"
#define MAPPING     L"Local\\LocalTimeTzClock.State"

struct SharedState {
    volatile LONG ready;       // DLL finished init
    volatile LONG supported;   // hooks installed successfully
    volatile LONG active;      // currently rewriting the clock
    volatile LONG hits;        // number of clock formatting calls rewritten
    volatile LONG lastError;   // Win32 error for the controller to report
    DWORD dllVersion;
};

static HANDLE g_map = nullptr;
static SharedState* g_state = nullptr;

static DWORD WINAPI ControlThread(LPVOID) {
    HANDLE enable  = CreateEventW(nullptr, FALSE, FALSE, EVT_ENABLE);
    HANDLE disable = CreateEventW(nullptr, FALSE, FALSE, EVT_DISABLE);
    HANDLE query   = CreateEventW(nullptr, FALSE, FALSE, EVT_QUERY);

    LoadConfig();
    if (g_cfg.log) {
        WCHAR dir[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, dir)))
            StringCchPrintfW(g_logPath, MAX_PATH, L"%s\\tzclock.log", dir);
    }
    InitializeCriticalSection(&g_logCs);
    g_logReady = TRUE;
    LogF(L"--- attach (pid %lu) ---", GetCurrentProcessId());
    LogF(L"config: TimeZoneId='%s' Enabled=%d Log=%d", g_cfg.timeZoneId,
         g_cfg.enabled, g_cfg.log);

    HMODULE mod = GetModuleHandleW(L"Taskbar.View.dll");
    if (!mod) {
        LogF(L"ERROR: Taskbar.View.dll not loaded in this process; staying inert");
        if (g_state) g_state->lastError = ERROR_MOD_NOT_FOUND;
        return 0;
    }
    g_modBase = (BYTE*)mod;

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
    LogF(L"initial state: enabled=%d patched=%d", g_cfg.enabled, g_patched);
    RequestClockRefresh();

    for (;;) {
        HANDLE waits[3] = { enable, disable, query };
        DWORD r = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0) {
            LoadConfig();
            g_dtziValid = FALSE;
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
                g_state->active = (DWORD)InterlockedCompareExchange(&g_active, 0, 0);
                g_state->hits = (DWORD)InterlockedCompareExchange(&g_hits, 0, 0);
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

        // Published state so the controller can report honestly instead of
        // guessing that the injection worked.
        g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                   PAGE_READWRITE, 0, sizeof(SharedState), MAPPING);
        if (g_map)
            g_state = (SharedState*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0,
                                                  sizeof(SharedState));
        if (g_state) { g_state->ready = 1; g_state->dllVersion = 1; }

        HANDLE h = CreateThread(nullptr, 0, ControlThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (reserved == nullptr) {
            if (g_patched && g_iatTime.slot) WriteSlotInternal(g_iatTime.slot, g_iatTime.original);
            if (g_patched && g_iatDate.slot) WriteSlotInternal(g_iatDate.slot, g_iatDate.original);
            LogF(L"--- detach ---");
        }
    }
    return TRUE;
}
