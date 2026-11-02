// Test host: loads tzclock.dll into a throwaway process so we can debug the
// DLL without crashing the real Explorer.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
int wmain(int argc, wchar_t** argv) {
    const wchar_t* p = (argc > 1) ? argv[1] : L"tzclock.dll";
    HMODULE m = LoadLibraryW(p);
    wprintf(L"LoadLibrary -> %p (err %lu)\n", m, GetLastError());
    if (!m) return 1;
    Sleep(3000);
    return 0;
}
