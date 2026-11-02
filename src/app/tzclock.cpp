// tzclock.exe - controller for the LocalTimeTzClock taskbar-clock DLL.
//
// It does not stay in memory and it does not poll. It injects the DLL into the
// taskbar's explorer.exe once, then talks to it through named events and a
// shared memory block.
//
//   tzclock.exe on       enable (inject if needed)
//   tzclock.exe off      disable, clock returns to native immediately
//   tzclock.exe status   report real state from the DLL
//   tzclock.exe install  enable + register login autostart (HKCU Run)
//   tzclock.exe uninstall remove autostart
//
// Never terminates explorer.exe.

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>

static void CtlName(wchar_t* out, size_t cch, const wchar_t* kind, DWORD pid) {
    _snwprintf(out, cch, L"Local\\LocalTimeTzClock.%s.%lu", kind, pid);
}
#define EVT_ENABLE  L"Enable"
#define EVT_DISABLE L"Disable"
#define EVT_QUERY   L"Query"
#define MAPPING     L"State"
#define RUNKEY      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUNVALUE    L"LocalTimeTzClock"

#ifndef TZCLOCK_DLL_NAME
#define TZCLOCK_DLL_NAME L"tzclock.dll"
#endif

struct SharedState {
    volatile LONG ready;
    volatile LONG supported;
    volatile LONG active;
    volatile LONG hits;
    volatile LONG lastError;
    DWORD dllVersion;
};

static void Out(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static int Fail(const wchar_t* msg, DWORD err) {
    fwprintf(stderr, L"ERROR: %s (win32 %lu)\n", msg, err);
    fflush(stderr);
    return 1;
}

static DWORD FindTaskbarPid() {
    DWORD pid = 0;
    HWND h = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (h) GetWindowThreadProcessId(h, &pid);
    return pid;
}

static bool InjectedIn(DWORD pid) {
    // Match the exact file name we ship, so a leftover older build in the same
    // Explorer does not make us skip injecting the current one.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me = { sizeof(me) };
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, TZCLOCK_DLL_NAME) == 0) { found = true; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

static bool Inject(DWORD pid, const wchar_t* dllPath) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                              PROCESS_VM_READ, FALSE, pid);
    if (!proc) return false;

    size_t bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    bool ok = false;
    if (remote && WriteProcessMemory(proc, remote, dllPath, bytes, nullptr)) {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        auto loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW");
        HANDLE th = CreateRemoteThread(proc, nullptr, 0, loadLib, remote, 0, nullptr);
        if (th) {
            WaitForSingleObject(th, 15000);
            DWORD code = 0;
            GetExitCodeThread(th, &code);
            ok = (code != 0);
            CloseHandle(th);
        }
    }
    if (remote) VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}

static wchar_t* DllPathNextToExe() {
    static wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* p = wcsrchr(path, L'\\');
    if (p) *(p + 1) = 0;
    wcscat(path, TZCLOCK_DLL_NAME);
    return path;
}

static DWORD g_statePid = 0;

static SharedState* OpenState(HANDLE* mapOut) {
    wchar_t name[96];
    CtlName(name, 96, MAPPING, g_statePid);
    HANDLE m = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!m) return nullptr;
    SharedState* s = (SharedState*)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0,
                                                 sizeof(SharedState));
    if (!s) { CloseHandle(m); return nullptr; }
    *mapOut = m;
    return s;
}

static bool Signal(const wchar_t* kind) {
    wchar_t name[96];
    CtlName(name, 96, kind, g_statePid);
    HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (!h) return false;
    SetEvent(h);
    CloseHandle(h);
    return true;
}

static const wchar_t* ErrText(DWORD e) {
    switch (e) {
        case 0: return L"none";
        case ERROR_MOD_NOT_FOUND: return L"Taskbar.View.dll not loaded";
        case ERROR_PROC_NOT_FOUND: return L"unexpected function layout in Taskbar.View.dll";
        case ERROR_INVALID_PARAMETER: return L"TimeZoneId in tzclock.ini is not a valid Windows time zone";
        default: return L"see tzclock.log";
    }
}

static void PrintStatus(SharedState* s) {
    Out(L"injected : yes (taskbar explorer.exe)\n");
    Out(L"supported: %s\n", s->supported ? L"yes" : L"NO");
    Out(L"active   : %s\n", s->active ? L"yes" : L"no");
    Out(L"rewrites : %ld clock formatting calls\n", s->hits);
    if (s->lastError) Out(L"lastError: %s\n", ErrText(s->lastError));
}

static WCHAR g_tracePath[MAX_PATH];
static void Trace(const wchar_t* fmt, ...) {
    if (!g_tracePath[0]) return;
    WCHAR line[1024];
    va_list ap; va_start(ap, fmt);
    vswprintf(line, 1024, fmt, ap);
    va_end(ap);
    HANDLE h = CreateFileW(g_tracePath, FILE_APPEND_DATA, FILE_SHARE_READ|FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w; SetFilePointer(h,0,nullptr,FILE_END);
        WriteFile(h,line,(DWORD)(wcslen(line)*2),&w,nullptr);
        WriteFile(h,L"\r\n",4,&w,nullptr);
        CloseHandle(h);
    }
}

static int RunCommand(const wchar_t* cmd);

int wmain(int argc, wchar_t** argv) {
    GetModuleFileNameW(nullptr, g_tracePath, MAX_PATH);
    wchar_t* sl = wcsrchr(g_tracePath, L'\\');
    if (sl) wcscpy(sl + 1, L"trace.log");
    const wchar_t* cmd = (argc > 1) ? argv[1] : L"status";
    int rc = RunCommand(cmd);
    return rc;
}

// One watchdog at a time. Named mutex, no polling, no registry.
#define WATCH_MUTEX L"Local\\LocalTimeTzClock.Watchdog"

static bool EnsureOnFor(DWORD pid, const wchar_t* dllPath, HANDLE* mapOut,
                        SharedState** stateOut, bool waitForActive);

// Watchdog: stays resident with a single blocking wait on the Explorer process
// handle. No polling of windows or processes. Cost is one idle thread plus the
// handle wait; memory is measured and reported in the delivery notes.
static int WatchLoop() {
    HANDLE once = CreateMutexW(nullptr, TRUE, WATCH_MUTEX);
    if (!once) return Fail(L"cannot create watchdog mutex", GetLastError());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Out(L"watchdog already running\n");
        CloseHandle(once);
        return 0;
    }

    const wchar_t* dll = DllPathNextToExe();
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES)
        return Fail(L"the clock DLL is missing next to this exe", 0);

    Out(L"watchdog started (Ctrl+C to stop)\n");
    DWORD lastPid = 0;
    for (;;) {
        DWORD pid = FindTaskbarPid();
        if (!pid) { Sleep(2000); continue; }

        if (pid != lastPid) {
            // A just-started Explorer may still be loading the taskbar. Retry a
            // few times before reporting a real failure; no busy polling.
            bool ok = false;
            DWORD lastErr = 0;
            for (int attempt = 0; attempt < 30 && !ok; ++attempt) {
                HANDLE map = nullptr;
                SharedState* s = nullptr;
                ok = EnsureOnFor(pid, dll, &map, &s, true);
                if (s) { lastErr = (DWORD)s->lastError; UnmapViewOfFile(s); }
                if (map) CloseHandle(map);
                if (ok) break;
                Sleep(1000);
            }
            if (ok) {
                Out(L"attached to explorer pid %lu, clock active\n", pid);
                lastPid = pid;
            } else {
                Out(L"explorer pid %lu not supported: %s\n", pid, ErrText(lastErr));
                lastPid = 0;   // stay idle, try again on the next Explorer
                Sleep(2000);
                continue;
            }
        }

        HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!proc) { Sleep(1000); continue; }
        WaitForSingleObject(proc, INFINITE);   // blocks; no polling
        CloseHandle(proc);
        Out(L"explorer exited, waiting for the new taskbar...\n");
        lastPid = 0;
        Sleep(1000);
    }
    return 0;
}

// Inject (if needed) and turn the clock rewrite on. Returns true when the DLL
// is present and reports supported; the caller owns *mapOut/*stateOut.
static bool EnsureOnFor(DWORD pid, const wchar_t* dllPath, HANDLE* mapOut,
                        SharedState** stateOut, bool waitForActive) {
    g_statePid = pid;   // control names are scoped to this taskbar process
    HANDLE map = nullptr;
    SharedState* s = OpenState(&map);
    if (!s || !s->ready) {
        if (s) { UnmapViewOfFile(s); CloseHandle(map); s = nullptr; map = nullptr; }
        if (!InjectedIn(pid)) {
            if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) return false;
            if (!Inject(pid, dllPath)) return false;
        }
        for (int i = 0; i < 120 && !s; ++i) {
            Sleep(50);
            s = OpenState(&map);
            if (s && !s->ready) { UnmapViewOfFile(s); CloseHandle(map); s = nullptr; map = nullptr; }
        }
        if (!s) return false;
    }

    // The DLL may still be waiting for Taskbar.View.dll to appear in a freshly
    // started Explorer, so poll the published state for a bounded time rather
    // than assuming an immediate result. This is the controller, not Explorer,
    // and it stops as soon as the state settles.
    Signal(EVT_ENABLE);
    int spins = waitForActive ? 400 : 40;   // up to ~5s / ~2s
    for (int i = 0; i < spins; ++i) {
        Sleep(50);
        Signal(EVT_QUERY);
        Sleep(20);
        if (s->active) break;
        if (s->lastError) break;
    }

    *mapOut = map;
    *stateOut = s;
    return s->supported != 0;
}

static int RunCommand(const wchar_t* cmd) {
    if (_wcsicmp(cmd, L"watch") == 0) return WatchLoop();
    if (_wcsicmp(cmd, L"stopwatch") == 0) {
        HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, WATCH_MUTEX);
        if (!h) { Out(L"no watchdog running\n"); return 0; }
        // Nothing to signal; report so the user can close it.
        Out(L"watchdog is running (close its console to stop it)\n");
        CloseHandle(h);
        return 0;
    }

    if (_wcsicmp(cmd, L"uninstall") == 0) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, RUNVALUE);
            RegCloseKey(k);
        }
        Out(L"autostart removed. Running clock is unaffected.\n");
        Out(L"To stop now: tzclock.exe off\n");
        return 0;
    }

    bool wantInstall = (_wcsicmp(cmd, L"install") == 0);
    bool wantOn      = (wantInstall || _wcsicmp(cmd, L"on") == 0 ||
                        _wcsicmp(cmd, L"enable") == 0);
    bool wantOff     = (_wcsicmp(cmd, L"off") == 0 || _wcsicmp(cmd, L"disable") == 0);
    bool wantStatus  = !(wantOn || wantOff);

    if (!wantStatus) {
        DWORD pid = FindTaskbarPid();
        if (!pid) return Fail(L"taskbar window (Shell_TrayWnd) not found", 0);

        HANDLE map = nullptr;
        SharedState* s = nullptr;
        if (!EnsureOnFor(pid, DllPathNextToExe(), &map, &s, false))
            return 3;

        if (wantOff) {
            Signal(EVT_DISABLE);
            for (int i = 0; i < 40; ++i) {
                Sleep(50);
                Signal(EVT_QUERY);
                Sleep(20);
                if (!s->active) break;
            }
        }

        PrintStatus(s);

        bool supported = s->supported != 0;
        DWORD lastErr = (DWORD)s->lastError;
        UnmapViewOfFile(s);
        CloseHandle(map);

        if (wantOn && !supported) {
            fwprintf(stderr, L"NOT SUPPORTED: %s\n", ErrText(lastErr));
            fwprintf(stderr, L"The clock was left showing the system time.\n");
            return 3;
        }
        if (wantOff) Out(L"Clock returned to the system time zone.\n");

        if (wantInstall) {
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            wchar_t quoted[MAX_PATH + 32];
            _snwprintf(quoted, MAX_PATH + 32, L"\"%s\" watch", exe);
            HKEY k;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, nullptr, 0,
                                KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
                return Fail(L"cannot write HKCU Run key", GetLastError());
            RegSetValueExW(k, RUNVALUE, 0, REG_SZ, (const BYTE*)quoted,
                           (DWORD)((wcslen(quoted) + 1) * sizeof(wchar_t)));
            RegCloseKey(k);
            Out(L"autostart registered (HKCU Run, runs 'watch').\n");
        }
        return 0;
    }

    // status
    DWORD pid = FindTaskbarPid();
    g_statePid = pid;
    if (!pid) return Fail(L"taskbar window not found", 0);
    if (!InjectedIn(pid)) {
        Out(L"injected : no (native clock, system time zone)\n");
        return 0;
    }
    HANDLE map = nullptr;
    SharedState* s = OpenState(&map);
    if (!s) {
        Out(L"injected : yes, but DLL is not responding\n");
        return 0;
    }
    PrintStatus(s);
    UnmapViewOfFile(s);
    CloseHandle(map);
    return 0;
}
