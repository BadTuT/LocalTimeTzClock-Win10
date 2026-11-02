// Minimal prototype injector: loads a DLL into the taskbar's explorer.exe.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <tlhelp32.h>

static DWORD FindTaskbarPid() {
    DWORD pid = 0;
    HWND h = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (h) GetWindowThreadProcessId(h, &pid);
    return pid;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"usage: %s <path-to-dll>\n", argv[0]);
        return 2;
    }

    WCHAR dll[MAX_PATH];
    if (!GetFullPathNameW(argv[1], MAX_PATH, dll, nullptr)) {
        wprintf(L"bad path\n");
        return 2;
    }
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"dll not found: %s\n", dll);
        return 2;
    }

    DWORD pid = FindTaskbarPid();
    if (!pid) { wprintf(L"taskbar window not found\n"); return 3; }

    HANDLE proc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!proc) { wprintf(L"OpenProcess failed %lu\n", GetLastError()); return 3; }

    size_t bytes = (wcslen(dll) + 1) * sizeof(WCHAR);
    void* remote = VirtualAllocEx(proc, nullptr, bytes,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { wprintf(L"VirtualAllocEx failed\n"); return 3; }
    if (!WriteProcessMemory(proc, remote, dll, bytes, nullptr)) {
        wprintf(L"WriteProcessMemory failed\n"); return 3;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW");
    HANDLE th = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!th) { wprintf(L"CreateRemoteThread failed %lu\n", GetLastError()); return 3; }

    WaitForSingleObject(th, 10000);
    DWORD code = 0;
    GetExitCodeThread(th, &code);
    wprintf(L"pid=%lu LoadLibraryW -> 0x%lX\n", pid, code);
    CloseHandle(th);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return code ? 0 : 4;
}
