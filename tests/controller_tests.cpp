#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <io.h>
#include <string.h>

// 只替换任务栏定位边界，使控制器连接本测试进程自己的共享内存。
static HWND WINAPI TestFindWindowW(LPCWSTR, LPCWSTR) { return (HWND)1; }
static DWORD WINAPI TestWindowPid(HWND, LPDWORD pid) {
    *pid = GetCurrentProcessId();
    return GetCurrentThreadId();
}
#define FindWindowW TestFindWindowW
#define GetWindowThreadProcessId TestWindowPid
#define wmain ControllerEntryPoint
#include "../src/app/tzclock.cpp"
#undef wmain
#undef FindWindowW
#undef GetWindowThreadProcessId

int main() {
    wchar_t mapName[96];
    CtlName(mapName, 96, MAPPING, GetCurrentProcessId());
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                    0, sizeof(SharedState), mapName);
    if (!map) return 1;
    SharedState* state = (SharedState*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState));
    if (!state) { CloseHandle(map); return 1; }
    state->ready = 1;
    state->dllVersion = TZCLOCK_VERSION;
    state->supported = 0;
    state->lastError = ERROR_PROC_NOT_FOUND;

    FILE* capture = tmpfile();
    if (!capture) { UnmapViewOfFile(state); CloseHandle(map); return 1; }
    int savedOut = _dup(_fileno(stdout)), savedErr = _dup(_fileno(stderr));
    _dup2(_fileno(capture), _fileno(stdout));
    _dup2(_fileno(capture), _fileno(stderr));
    wchar_t exe[] = L"controller_test", command[] = L"on";
    wchar_t* args[] = { exe, command };
    int result = RunCommand(2, args);
    fflush(stdout); fflush(stderr);
    _dup2(savedOut, _fileno(stdout)); _dup2(savedErr, _fileno(stderr));
    _close(savedOut); _close(savedErr);
    rewind(capture);
    char output[4096] = {};
    fread(output, 1, sizeof(output) - 1, capture);
    // 宿主终端可能将宽字符输出设为 UTF-16，断言前统一解码。
    char decoded[4096] = {};
    const char* diagnostic = output;
    if (output[0] && !output[1]) {
        WideCharToMultiByte(CP_UTF8, 0, (WCHAR*)output, -1, decoded, sizeof(decoded), nullptr, nullptr);
        diagnostic = decoded;
    }
    fclose(capture);
    UnmapViewOfFile(state); CloseHandle(map);
    if (result != 3 || !strstr(diagnostic, "supported: NO") ||
        !strstr(diagnostic, "unsupported or unverified taskbar clock module layout")) {
        printf("FAIL: controller returned %d without an actionable failure diagnostic\nCaptured: %s\n", result, diagnostic);
        return 1;
    }
    printf("PASS: unsupported on reports status and reason (no Explorer injection)\n");
    return 0;
}
