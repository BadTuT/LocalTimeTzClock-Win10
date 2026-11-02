// DIAGNOSTIC build: patch all four time-related IAT slots in Taskbar.View.dll
// and log every distinct call site, so we can see which code actually formats
// the taskbar clock in this build.
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shlobj.h>
#include <strsafe.h>
#include <stdlib.h>
#include <stdarg.h>
#include <intrin.h>

typedef int (WINAPI* PFN_GetTimeFormatEx)(LPCWSTR, DWORD, const SYSTEMTIME*, LPCWSTR, LPWSTR, int);
typedef int (WINAPI* PFN_GetDateFormatEx)(LPCWSTR, DWORD, const SYSTEMTIME*, LPCWSTR, LPWSTR, int, LPCWSTR);
typedef VOID (WINAPI* PFN_GetLocalTime)(LPSYSTEMTIME);
typedef VOID (WINAPI* PFN_GetSystemTime)(LPSYSTEMTIME);

static PFN_GetTimeFormatEx oTime;
static PFN_GetDateFormatEx oDate;
static PFN_GetLocalTime oLocal;
static PFN_GetSystemTime oSystem;
static BYTE* g_base;
static WCHAR g_log[MAX_PATH];
static CRITICAL_SECTION g_cs;

static void LogF(const WCHAR* fmt, ...) {
    if (!g_log[0]) return;
    WCHAR line[2048];
    va_list ap; va_start(ap, fmt);
    StringCchVPrintfW(line, ARRAYSIZE(line), fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_cs);
    HANDLE h = CreateFileW(g_log, FILE_APPEND_DATA, FILE_SHARE_READ|FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w; SetFilePointer(h, 0, nullptr, FILE_END);
        WriteFile(h, line, (DWORD)(wcslen(line)*2), &w, nullptr);
        static const WCHAR nl[] = L"\r\n";
        WriteFile(h, nl, 4, &w, nullptr);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_cs);
}

static int WINAPI HTime(LPCWSTR l, DWORD f, const SYSTEMTIME* t, LPCWSTR fm, LPWSTR b, int c) {
    LogF(L"TIME ra=%08X flags=%X fmt=%s", (DWORD)((BYTE*)_ReturnAddress()-g_base), f, fm?fm:L"(null)");
    return oTime(l,f,t,fm,b,c);
}
static int WINAPI HDate(LPCWSTR l, DWORD f, const SYSTEMTIME* t, LPCWSTR fm, LPWSTR b, int c, LPCWSTR cal) {
    LogF(L"DATE ra=%08X flags=%X fmt=%s", (DWORD)((BYTE*)_ReturnAddress()-g_base), f, fm?fm:L"(null)");
    return oDate(l,f,t,fm,b,c,cal);
}
static VOID WINAPI HLocal(LPSYSTEMTIME s) {
    LogF(L"LOCALTIME ra=%08X", (DWORD)((BYTE*)_ReturnAddress()-g_base));
    oLocal(s);
}
static VOID WINAPI HSystem(LPSYSTEMTIME s) {
    LogF(L"SYSTEMTIME ra=%08X", (DWORD)((BYTE*)_ReturnAddress()-g_base));
    oSystem(s);
}

static void* PatchImport(const char* mod, const char* fn, void* repl) {
    auto dos = (IMAGE_DOS_HEADER*)g_base;
    auto nt = (IMAGE_NT_HEADERS*)(g_base + dos->e_lfanew);
    auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(g_base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; imp && imp->Name; ++imp) {
        const char* dll = (const char*)(g_base + imp->Name);
        if (_stricmp(dll, mod) != 0) continue;
        auto oft = (IMAGE_THUNK_DATA*)(g_base + imp->OriginalFirstThunk);
        auto ft  = (IMAGE_THUNK_DATA*)(g_base + imp->FirstThunk);
        if (!oft) continue;
        for (; oft->u1.AddressOfData; ++oft, ++ft) {
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(g_base + oft->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, fn) != 0) continue;
            DWORD op;
            VirtualProtect(&ft->u1.Function, 8, PAGE_READWRITE, &op);
            void* orig = (void*)ft->u1.Function;
            ft->u1.Function = (ULONG_PTR)repl;
            VirtualProtect(&ft->u1.Function, 8, op, &op);
            return orig;
        }
    }
    return nullptr;
}

extern "C" __declspec(dllexport) DWORD WINAPI DiagStart(LPVOID) {
    InitializeCriticalSection(&g_cs);
    WCHAR dir[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, dir);
    StringCchPrintfW(g_log, MAX_PATH, L"%s\\tzclock-diag.log", dir);
    DeleteFileW(g_log);

    HMODULE m = GetModuleHandleW(L"Taskbar.View.dll");
    if (!m) { LogF(L"no Taskbar.View.dll"); return 1; }
    g_base = (BYTE*)m;

    oTime   = (PFN_GetTimeFormatEx)PatchImport("api-ms-win-core-datetime-l1-1-1.dll","GetTimeFormatEx",(void*)HTime);
    oDate   = (PFN_GetDateFormatEx)PatchImport("api-ms-win-core-datetime-l1-1-1.dll","GetDateFormatEx",(void*)HDate);
    oLocal  = (PFN_GetLocalTime) PatchImport("api-ms-win-core-sysinfo-l1-1-0.dll","GetLocalTime",(void*)HLocal);
    oSystem = (PFN_GetSystemTime)PatchImport("api-ms-win-core-sysinfo-l1-1-0.dll","GetSystemTime",(void*)HSystem);
    LogF(L"patched time=%d date=%d local=%d system=%d", oTime!=nullptr, oDate!=nullptr, oLocal!=nullptr, oSystem!=nullptr);
    // also try the locale variants / other apisets
    LogF(L"extra: try datetime-l1-1-0 Time=%p Date=%p",
        PatchImport("api-ms-win-core-datetime-l1-1-0.dll","GetTimeFormatEx",(void*)HTime),
        PatchImport("api-ms-win-core-datetime-l1-1-0.dll","GetDateFormatEx",(void*)HDate));
    return 0;
}
BOOL WINAPI DllMain(HINSTANCE i, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(i);
        HANDLE h = CreateThread(nullptr,0,[](LPVOID)->DWORD{ return DiagStart(nullptr);},nullptr,0,nullptr);
        if(h) CloseHandle(h);
    }
    return TRUE;
}
