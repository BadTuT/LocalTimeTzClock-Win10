// Test host: loads the hook DLL into a throwaway process.
// Lets us debug DLL initialization without touching the real Explorer.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int wmain(int argc, wchar_t** argv) {
    const wchar_t* path = (argc > 1) ? argv[1] : L"tzclock_v11.dll";
    HMODULE m = LoadLibraryW(path);
    wprintf(L"LoadLibrary(%ls) -> %p (err %lu)\n", path, m, GetLastError());
    if (!m) return 1;
    Sleep(3000);
    return 0;
}
