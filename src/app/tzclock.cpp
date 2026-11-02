// tzclock.exe - controller for the LocalTimeTzClock taskbar-clock DLL.
//
// It does not stay in memory and it does not poll. It injects the DLL into the
// taskbar's explorer.exe once, then talks to it through named events and a
// shared memory block.
//
//   tzclock.exe on          enable (inject if needed)
//   tzclock.exe off         disable, clock returns to native immediately
//   tzclock.exe status      report real state from the DLL
//   tzclock.exe watch       watchdog mode (auto-reinject if Explorer restarts)
//   tzclock.exe stopwatch   stop the running watchdog instance
//   tzclock.exe install     enable + register login autostart (HKCU Run)
//   tzclock.exe uninstall   remove autostart & stop watchdog
//
// Never terminates explorer.exe.

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>

#define TZCLOCK_VERSION 14

static void CtlName(wchar_t* out, size_t cch, const wchar_t* kind, DWORD pid) {
    _snwprintf(out, cch, L"Local\\LocalTimeTzClock.v%u.%s.%lu", TZCLOCK_VERSION, kind, pid);
}
#define EVT_ENABLE     L"Enable"
#define EVT_DISABLE    L"Disable"
#define EVT_QUERY      L"Query"
#define MAPPING        L"State"
#define RUNKEY         L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUNVALUE       L"LocalTimeTzClock"
#define WATCH_STOP_EVT L"Local\\LocalTimeTzClock.WatchdogStop"
#define WATCH_MUTEX    L"Local\\LocalTimeTzClock.Watchdog"

#ifndef TZCLOCK_DLL_NAME
#define TZCLOCK_DLL_NAME L"tzclock_v14.dll"
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
    if (h) {
        GetWindowThreadProcessId(h, &pid);
        if (pid) return pid;
    }

    // If caller is in another virtual desktop or background session, check Default desktop
    HDESK hDef = OpenDesktopW(L"Default", 0, FALSE, DESKTOP_READOBJECTS | DESKTOP_ENUMERATE);
    if (hDef) {
        HDESK hOld = GetThreadDesktop(GetCurrentThreadId());
        if (SetThreadDesktop(hDef)) {
            HWND h2 = FindWindowW(L"Shell_TrayWnd", nullptr);
            if (h2) GetWindowThreadProcessId(h2, &pid);
            SetThreadDesktop(hOld);
        }
        CloseDesktop(hDef);
        if (pid) return pid;
    }

    // Fallback: search for explorer.exe running in the current user session
    DWORD curSessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &curSessionId);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
                    DWORD sId = 0;
                    if (ProcessIdToSessionId(pe.th32ProcessID, &sId) && sId == curSessionId) {
                        pid = pe.th32ProcessID;
                        break;
                    }
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    return pid;
}

static bool InjectedIn(DWORD pid) {
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
    wcsncat(path, TZCLOCK_DLL_NAME, MAX_PATH - wcslen(path) - 1);
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
        case ERROR_MOD_NOT_FOUND: return L"taskbar clock module not loaded";
        case ERROR_PROC_NOT_FOUND: return L"unexpected import layout in the taskbar clock module";
        case ERROR_INVALID_PARAMETER: return L"TimeZoneId in tzclock.ini is not a valid Windows time zone";
        default: return L"see tzclock.log";
    }
}

static void PrintStatus(SharedState* s) {
    Out(L"injected : yes (taskbar explorer.exe)\n");
    Out(L"version  : v%lu\n", s->dllVersion);
    Out(L"supported: %s\n", s->supported ? L"yes" : L"NO");
    Out(L"active   : %s\n", s->active ? L"yes" : L"no");
    Out(L"rewrites : %ld clock formatting calls\n", s->hits);
    if (s->lastError) Out(L"lastError: %s\n", ErrText(s->lastError));
}

static bool EnsureOnFor(DWORD pid, const wchar_t* dllPath, HANDLE* mapOut,
                        SharedState** stateOut, bool waitForActive) {
    g_statePid = pid;
    HANDLE map = nullptr;
    SharedState* s = OpenState(&map);
    if (!s || !s->ready || s->dllVersion != TZCLOCK_VERSION) {
        if (s) { UnmapViewOfFile(s); CloseHandle(map); s = nullptr; map = nullptr; }
        if (!InjectedIn(pid)) {
            if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) return false;
            if (!Inject(pid, dllPath)) return false;
        }
        for (int i = 0; i < 120 && !s; ++i) {
            Sleep(50);
            s = OpenState(&map);
            if (s && (!s->ready || s->dllVersion != TZCLOCK_VERSION)) {
                UnmapViewOfFile(s); CloseHandle(map); s = nullptr; map = nullptr;
            }
        }
        if (!s) return false;
    }

    Signal(EVT_ENABLE);
    int spins = waitForActive ? 400 : 60;   // up to ~5s / ~3s
    for (int i = 0; i < spins; ++i) {
        Sleep(40);
        Signal(EVT_QUERY);
        Sleep(20);
        if (s->active) break;
        if (s->lastError) break;
    }

    *mapOut = map;
    *stateOut = s;
    return s->supported != 0;
}

static int WatchLoop(bool hideWindow) {
    if (hideWindow) {
        HWND con = GetConsoleWindow();
        if (con) ShowWindow(con, SW_HIDE);
    }

    HANDLE once = CreateMutexW(nullptr, TRUE, WATCH_MUTEX);
    if (!once) return Fail(L"cannot create watchdog mutex", GetLastError());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Out(L"watchdog already running\n");
        CloseHandle(once);
        return 0;
    }

    HANDLE stopEvt = CreateEventW(nullptr, TRUE, FALSE, WATCH_STOP_EVT);
    ResetEvent(stopEvt);

    const wchar_t* dll = DllPathNextToExe();
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
        CloseHandle(once);
        if (stopEvt) CloseHandle(stopEvt);
        return Fail(L"the clock DLL is missing next to this exe", 0);
    }

    Out(L"watchdog started (run 'tzclock.exe stopwatch' or Ctrl+C to stop)\n");
    DWORD lastPid = 0;
    for (;;) {
        // Check if stop requested while idle
        if (WaitForSingleObject(stopEvt, 0) == WAIT_OBJECT_0) {
            Out(L"watchdog received stop signal, exiting...\n");
            break;
        }

        DWORD pid = FindTaskbarPid();
        if (!pid) { Sleep(2000); continue; }

        if (pid != lastPid) {
            bool ok = false;
            DWORD lastErr = 0;
            for (int attempt = 0; attempt < 30 && !ok; ++attempt) {
                if (WaitForSingleObject(stopEvt, 0) == WAIT_OBJECT_0) break;
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
                lastPid = 0;
                Sleep(2000);
                continue;
            }
        }

        HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!proc) { Sleep(1000); continue; }

        HANDLE waits[2] = { proc, stopEvt };
        DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        CloseHandle(proc);

        if (wr == WAIT_OBJECT_0 + 1) {
            Out(L"watchdog received stop signal, exiting...\n");
            break;
        }

        Out(L"explorer exited, waiting for the new taskbar...\n");
        lastPid = 0;
        Sleep(1000);
    }

    CloseHandle(once);
    if (stopEvt) CloseHandle(stopEvt);
    return 0;
}

static int RunCommand(int argc, wchar_t** argv) {
    const wchar_t* cmd = (argc > 1) ? argv[1] : L"status";
    bool hide = false;
    for (int i = 2; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--hide") == 0 || _wcsicmp(argv[i], L"-hide") == 0) hide = true;
    }

    if (_wcsicmp(cmd, L"watch") == 0) return WatchLoop(hide);

    if (_wcsicmp(cmd, L"stopwatch") == 0) {
        HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, WATCH_STOP_EVT);
        if (h) {
            SetEvent(h);
            CloseHandle(h);
            Out(L"watchdog stop signal sent successfully.\n");
        } else {
            Out(L"no active watchdog found.\n");
        }
        return 0;
    }

    if (_wcsicmp(cmd, L"uninstall") == 0) {
        // Also signal running watchdog to stop
        HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, WATCH_STOP_EVT);
        if (h) { SetEvent(h); CloseHandle(h); }

        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, RUNVALUE);
            RegCloseKey(k);
        }
        Out(L"autostart removed and watchdog stopped. Running clock is unaffected.\n");
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
        if (!pid) return Fail(L"taskbar explorer process not found", 0);

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
            _snwprintf(quoted, MAX_PATH + 32, L"\"%s\" watch --hide", exe);
            HKEY k;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, nullptr, 0,
                                KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
                return Fail(L"cannot write HKCU Run key", GetLastError());
            RegSetValueExW(k, RUNVALUE, 0, REG_SZ, (const BYTE*)quoted,
                           (DWORD)((wcslen(quoted) + 1) * sizeof(wchar_t)));
            RegCloseKey(k);
            Out(L"autostart registered (HKCU Run, runs 'watch --hide').\n");
        }
        return 0;
    }

    // status: query live values
    DWORD pid = FindTaskbarPid();
    g_statePid = pid;
    if (!pid) return Fail(L"taskbar explorer process not found", 0);
    if (!InjectedIn(pid)) {
        Out(L"injected : no (native clock, system time zone)\n");
        return 0;
    }

    Signal(EVT_QUERY);
    Sleep(30);

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

int wmain(int argc, wchar_t** argv) {
    return RunCommand(argc, argv);
}
