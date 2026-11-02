// LocalTimeTzClock - keeps system timezone, but makes the native taskbar clock
// render the time/date of a different Windows named time zone.
//
// Prototype stage: time zone is hardcoded, logging to a file, no config yet.
//
// Nothing global is hooked. Only the two import table slots of
// Taskbar.View.dll (GetTimeFormatEx / GetDateFormatEx) are redirected, and the
// hooks act only when the return address is one of the three known clock
// formatting call sites. Every other call falls straight through.

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shlobj.h>
#include <strsafe.h>
#include <stdlib.h>
#include <stdarg.h>
#include <intrin.h>

typedef int (WINAPI* PFN_GetTimeFormatEx)(LPCWSTR, DWORD, const SYSTEMTIME*,
                                          LPCWSTR, LPWSTR, int);
typedef int (WINAPI* PFN_GetDateFormatEx)(LPCWSTR, DWORD, const SYSTEMTIME*,
                                          LPCWSTR, LPWSTR, int, LPCWSTR);

static PFN_GetTimeFormatEx g_origTimeFmt = nullptr;
static PFN_GetDateFormatEx g_origDateFmt = nullptr;

static BYTE*  g_modBase = nullptr;
static DWORD  g_callSiteTime = 0;   // return address RVA of the time call
static DWORD  g_callSiteDate[2] = {0, 0};
static int    g_callSiteDateCount = 0;

static WCHAR  g_targetTzId[128] = L"China Standard Time";
static DYNAMIC_TIME_ZONE_INFORMATION g_dtzi;
static bool   g_dtziValid = false;
static volatile LONG g_hits = 0;
static volatile LONG g_misses = 0;
static volatile LONG g_loggedMiss = 0;

// ---------------------------------------------------------------- logging ---
static WCHAR g_logPath[MAX_PATH];
static CRITICAL_SECTION g_logCs;

static void LogF(const WCHAR* fmt, ...) {
    if (!g_logPath[0]) return;
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
        static const WCHAR nl[] = L"\r\n";
        WriteFile(h, nl, 2 * sizeof(WCHAR), &wrote, nullptr);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_logCs);
}

// ------------------------------------------------------------ target zone ---
static bool LoadTargetZone() {
    for (DWORD i = 0;; ++i) {
        DYNAMIC_TIME_ZONE_INFORMATION d;
        if (EnumDynamicTimeZoneInformation(i, &d) != ERROR_SUCCESS) break;
        if (_wcsicmp(d.TimeZoneKeyName, g_targetTzId) == 0) {
            g_dtzi = d;
            g_dtziValid = true;
            return true;
        }
    }
    g_dtziValid = false;
    return false;
}

// Current UTC instant, expressed in the configured time zone using Windows'
// own rules (DST, half/quarter hour offsets, date rollover all handled here).
static bool TargetNow(SYSTEMTIME* out) {
    if (!g_dtziValid) return false;
    SYSTEMTIME utc;
    GetSystemTime(&utc);
    return SystemTimeToTzSpecificLocalTimeEx(&g_dtzi, &utc, out) != FALSE;
}

static bool IsClockCallSite(void* retAddr) {
    DWORD rva = (DWORD)((BYTE*)retAddr - g_modBase);
    if (g_callSiteTime && rva == g_callSiteTime) return true;
    for (int i = 0; i < g_callSiteDateCount; ++i)
        if (rva == g_callSiteDate[i]) return true;
    return false;
}

// ------------------------------------------------------------------ hooks ---
static int WINAPI HookGetTimeFormatEx(LPCWSTR locale, DWORD flags,
                                      const SYSTEMTIME* t, LPCWSTR fmt,
                                      LPWSTR buf, int cch) {
    void* ra = _ReturnAddress();
    if (IsClockCallSite(ra)) {
        SYSTEMTIME target;
        if (TargetNow(&target)) {
            InterlockedIncrement(&g_hits);
            return g_origTimeFmt(locale, flags, &target, fmt, buf, cch);
        }
    } else if (InterlockedIncrement(&g_misses) <= 6) {
        LogF(L"TIME miss ra_rva=%08X", (DWORD)((BYTE*)ra - g_modBase));
    }
    return g_origTimeFmt(locale, flags, t, fmt, buf, cch);
}

static int WINAPI HookGetDateFormatEx(LPCWSTR locale, DWORD flags,
                                      const SYSTEMTIME* t, LPCWSTR fmt,
                                      LPWSTR buf, int cch, LPCWSTR cal) {
    void* ra = _ReturnAddress();
    if (IsClockCallSite(ra)) {
        SYSTEMTIME target;
        if (TargetNow(&target)) {
            InterlockedIncrement(&g_hits);
            return g_origDateFmt(locale, flags, &target, fmt, buf, cch, cal);
        }
    } else if (InterlockedIncrement(&g_misses) <= 12) {
        LogF(L"DATE miss ra_rva=%08X", (DWORD)((BYTE*)ra - g_modBase));
    }
    return g_origDateFmt(locale, flags, t, fmt, buf, cch, cal);
}

// ------------------------------------------------------------ IAT patching --
struct IatEntry { void** slot; void* original; };

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
            if (!VirtualProtect(&ft->u1.Function, sizeof(void*),
                                PAGE_READWRITE, &oldProt))
                return -1;
            found->slot = (void**)&ft->u1.Function;
            found->original = (void*)ft->u1.Function;
            ft->u1.Function = (ULONG_PTR)replacement;
            VirtualProtect(&ft->u1.Function, sizeof(void*), oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), &ft->u1.Function,
                                  sizeof(void*));
            return 1;
        }
    }
    return 0;
}

// Find the call sites that use the two import slots, as a fallback for builds
// whose module version is not the one verified during development.
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

static bool g_patched = false;
static IatEntry g_iatTime{}, g_iatDate{};

static void InitOnce() {
    InitializeCriticalSection(&g_logCs);

    WCHAR dir[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, dir))) {
        StringCchPrintfW(g_logPath, ARRAYSIZE(g_logPath), L"%s\\tzclock.log", dir);
    }
    DeleteFileW(g_logPath);

    HMODULE mod = GetModuleHandleW(L"Taskbar.View.dll");
    if (!mod) {
        LogF(L"FATAL: Taskbar.View.dll not loaded in this process");
        return;
    }
    g_modBase = (BYTE*)mod;

    WCHAR path[MAX_PATH];
    GetModuleFileNameW(mod, path, ARRAYSIZE(path));
    DWORD dummy = 0, verSize = GetFileVersionInfoSizeW(path, &dummy);
    WCHAR ver[64] = L"?";
    if (verSize) {
        void* buf = malloc(verSize);
        if (buf && GetFileVersionInfoW(path, 0, verSize, buf)) {
            VS_FIXEDFILEINFO* fi = nullptr;
            UINT len = 0;
            if (VerQueryValueW(buf, L"\\", (void**)&fi, &len) && fi) {
                StringCchPrintfW(ver, ARRAYSIZE(ver), L"%d.%d.%d.%d",
                                 HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                                 HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
            }
        }
        free(buf);
    }
    LogF(L"Taskbar.View.dll = %s", path);
    LogF(L"Taskbar.View.dll version = %s", ver);

    int a = PatchImport("api-ms-win-core-datetime-l1-1-1.dll", "GetTimeFormatEx",
                        (void*)HookGetTimeFormatEx, &g_iatTime);
    int b = PatchImport("api-ms-win-core-datetime-l1-1-1.dll", "GetDateFormatEx",
                        (void*)HookGetDateFormatEx, &g_iatDate);
    LogF(L"IAT patch: time=%d date=%d", a, b);
    if (a != 1 || b != 1) return;
    g_origTimeFmt = (PFN_GetTimeFormatEx)g_iatTime.original;
    g_origDateFmt = (PFN_GetDateFormatEx)g_iatDate.original;
    if (!g_origTimeFmt || !g_origDateFmt) {
        LogF(L"FATAL: original pointers null");
        *g_iatTime.slot = g_iatTime.original;
        *g_iatDate.slot = g_iatDate.original;
        return;
    }
    g_patched = true;

    DWORD timeSlotRva = (DWORD)((BYTE*)g_iatTime.slot - g_modBase);
    DWORD dateSlotRva = (DWORD)((BYTE*)g_iatDate.slot - g_modBase);
    LogF(L"IAT slot RVA: time=%08X date=%08X", timeSlotRva, dateSlotRva);

    // Verified for 623.27903.70.0 on Windows 11 22631.2861.
    if (wcscmp(ver, L"623.27903.70.0") == 0) {
        g_callSiteTime = 0x359F9 + 6;
        g_callSiteDate[0] = 0x34DF3 + 6;
        g_callSiteDate[1] = 0x34E49 + 6;
        g_callSiteDateCount = 2;
        LogF(L"Using verified call-site table");
    } else {
        DWORD fits[8] = {0}, fds[8] = {0};
        int nt = ScanCallSites(timeSlotRva, fits, 8);
        int nd = ScanCallSites(dateSlotRva, fds, 8);
        LogF(L"Scan found %d time / %d date call sites", nt, nd);
        if (nt != 1 || nd != 2) {
            LogF(L"FATAL: unexpected layout, restoring IAT");
            *g_iatTime.slot = g_iatTime.original;
            *g_iatDate.slot = g_iatDate.original;
            g_patched = false;
            return;
        }
        g_callSiteTime = fits[0];
        memcpy(g_callSiteDate, fds, sizeof(DWORD) * nd);
        g_callSiteDateCount = nd;
        LogF(L"Using scanned call sites: time=%08X date0=%08X date1=%08X",
             g_callSiteTime, g_callSiteDate[0], g_callSiteDate[1]);
    }

    if (LoadTargetZone()) {
        LogF(L"Target zone resolved: %s (%s)", g_targetTzId,
             g_dtzi.TimeZoneKeyName);
    } else {
        LogF(L"FATAL: target time zone '%s' not found", g_targetTzId);
        *g_iatTime.slot = g_iatTime.original;
        *g_iatDate.slot = g_iatDate.original;
        g_patched = false;
        return;
    }

    LogF(L"Ready. System local time vs target:");
    SYSTEMTIME lt, tt;
    GetLocalTime(&lt);
    TargetNow(&tt);
    LogF(L"  local  = %04d-%02d-%02d %02d:%02d:%02d", lt.wYear, lt.wMonth,
         lt.wDay, lt.wHour, lt.wMinute, lt.wSecond);
    LogF(L"  target = %04d-%02d-%02d %02d:%02d:%02d", tt.wYear, tt.wMonth,
         tt.wDay, tt.wHour, tt.wMinute, tt.wSecond);
}

static void Uninit() {
    if (g_patched) {
        DWORD oldProt = 0;
        if (VirtualProtect(g_iatTime.slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
            *g_iatTime.slot = g_iatTime.original;
            VirtualProtect(g_iatTime.slot, sizeof(void*), oldProt, &oldProt);
        }
        if (VirtualProtect(g_iatDate.slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
            *g_iatDate.slot = g_iatDate.original;
            VirtualProtect(g_iatDate.slot, sizeof(void*), oldProt, &oldProt);
        }
        g_patched = false;
        LogF(L"IAT restored, hook hits=%ld", g_hits);
    }
}

// ------------------------------------------------------------------- DllMain
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(inst);
            // Do the work on a private thread: LoadLibrary runs under the
            // loader lock, and we want writable globals.
            {
                HANDLE h = CreateThread(nullptr, 0,
                    [](LPVOID) -> DWORD { InitOnce(); return 0; }, nullptr, 0, nullptr);
                if (h) CloseHandle(h);
            }
            break;
        case DLL_PROCESS_DETACH:
            if (reserved == nullptr) Uninit();
            break;
    }
    return TRUE;
}

// Exported so the injector can confirm the load actually happened.
extern "C" __declspec(dllexport) DWORD WINAPI TzcGetHits() {
    return (DWORD)InterlockedCompareExchange(&g_hits, 0, 0);
}
extern "C" __declspec(dllexport) DWORD WINAPI TzcGetMisses() {
    return (DWORD)InterlockedCompareExchange(&g_misses, 0, 0);
}
