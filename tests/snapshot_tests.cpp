#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>

// 固定 UTC 在午夜刚过，原本地时间样本停留在上一分钟。
static VOID WINAPI FixedUtc(LPSYSTEMTIME st) {
    SYSTEMTIME value = {2026, 11, 1, 2, 16, 0, 0, 5};
    *st = value;
}
static VOID WINAPI EarlierLocal(LPSYSTEMTIME st) {
    SYSTEMTIME value = {2026, 11, 1, 2, 23, 59, 59, 990};
    *st = value;
}
#define GetSystemTime FixedUtc
#define main ExistingRegressionMain
#include "regression_tests.cpp"
#undef main
#undef GetSystemTime

int main() {
    PeBuilderConfig config = SystemTrayConfig();
    BYTE* pe = BuildFakePe(config);
    g_modBase = pe;
    if (!ActivateHooks()) return 2;
    wcscpy_s(g_cfg.timeZoneId, L"China Standard Time");
    if (!LoadTargetZone()) return 3;
    g_origLocalTime = EarlierLocal;
    DWORD rva = 0xCA3BD;
    BYTE* entry = pe + rva - 10;
    const BYTE sub[] = {0x48, 0x83, 0xEC, 0x28, 0xFF, 0x15};
    memcpy(entry, sub, sizeof(sub));
    *(INT32*)(entry + 6) = (INT32)(config.localSlotRva - rva);
    const BYTE tail[] = {0x48, 0x83, 0xC4, 0x28, 0xC3};
    memcpy(pe + rva, tail, sizeof(tail));
    FlushInstructionCache(GetCurrentProcess(), entry, 15);
    SYSTEMTIME output = {}, expected = {};
    TargetNow(&expected);
    ((PfnCallStub)entry)(&output);
    printf("actual %u-%u-%u %u:%u:%u.%u; expected %u-%u-%u %u:%u:%u.%u\n",
           output.wYear, output.wMonth, output.wDay, output.wHour, output.wMinute, output.wSecond, output.wMilliseconds,
           expected.wYear, expected.wMonth, expected.wDay, expected.wHour, expected.wMinute, expected.wSecond, expected.wMilliseconds);
    bool match = memcmp(&output, &expected, sizeof(output)) == 0;
    DestroyFakePe(pe);
    return match ? 0 : 1;
}
