// Measure how many pages of a specific module are actually resident in a
// process. Used to report the real in-Explorer footprint of tzclock.dll.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct _PSAPI_WS_EX_BLOCK {
    ULONG_PTR Flags;
    ULONG_PTR Data;
} WS_EX_BLOCK;

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        wprintf(L"usage: %s <pid> <module-substring>\n", argv[0]);
        return 2;
    }
    DWORD pid = (DWORD)_wtoi(argv[1]);
    const wchar_t* want = argv[2];

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) { wprintf(L"snapshot failed %lu\n", GetLastError()); return 1; }
    MODULEENTRY32W me = { sizeof(me) };
    MODULEENTRY32W found = {};
    BOOL have = FALSE;
    if (Module32FirstW(snap, &me)) {
        do {
            if (wcsstr(me.szModule, want)) { found = me; have = TRUE; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    if (!have) { wprintf(L"module not found in pid %lu\n", pid); return 1; }

    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) { wprintf(L"OpenProcess failed %lu\n", GetLastError()); return 1; }

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const size_t page = si.dwPageSize;
    BYTE* base = found.modBaseAddr;
    size_t size = found.modBaseSize;
    size_t pages = size / page;

    WS_EX_BLOCK* info = (WS_EX_BLOCK*)malloc(pages * sizeof(WS_EX_BLOCK));
    if (!info) return 1;
    for (size_t i = 0; i < pages; ++i)
        info[i].Flags = (ULONG_PTR)(base + i * page);

    size_t resident = 0;
    if (QueryWorkingSetEx(proc, info, (DWORD)(pages * sizeof(WS_EX_BLOCK)))) {
        for (size_t i = 0; i < pages; ++i)
            if (info[i].Data & 1) ++resident;   // Valid bit set => page resident
    } else {
        wprintf(L"QueryWorkingSetEx failed %lu\n", GetLastError());
        free(info); CloseHandle(proc); return 1;
    }

    wprintf(L"module        : %ls\n", found.szModule);
    wprintf(L"image size    : %.1f KB (%zu pages)\n", size / 1024.0, pages);
    wprintf(L"resident      : %.1f KB (%zu pages)\n", resident * page / 1024.0, resident);
    wprintf(L"not resident  : %.1f KB (%zu pages)\n",
            (pages - resident) * page / 1024.0, pages - resident);
    free(info);
    CloseHandle(proc);
    return 0;
}
