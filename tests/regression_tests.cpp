#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 仅在测试进程中注入 VirtualProtect 故障，真实写入和回滚仍执行生产函数。
static unsigned g_protectCalls = 0;
static unsigned long long g_failProtectMask = 0;
static BOOL WINAPI TestVirtualProtect(LPVOID p, SIZE_T n, DWORD prot, PDWORD old) {
    ++g_protectCalls;
    if (g_protectCalls <= 64 && (g_failProtectMask & (1ULL << (g_protectCalls - 1)))) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return VirtualProtect(p, n, prot, old);
}
#define VirtualProtect TestVirtualProtect
#define TZCLOCK_TEST_RUNNER 1
#include "../src/dll/tzclock.cpp"
#undef VirtualProtect

struct PeBuilderConfig {
    DWORD timeDateStamp;
    DWORD sizeOfImage;
    bool emptyImports;
    bool hasOriginalFirstThunk;
    bool includeTime;
    bool includeDate;
    bool includeLocal;
    bool duplicateTime;
    const char* timeModuleName;
    const char* dateModuleName;
    const char* localModuleName;
    DWORD timeSlotRva;
    DWORD dateSlotRva;
    DWORD localSlotRva;
    int numTimeCalls;
    DWORD timeReturnRvas[32];
    int numDateCalls;
    DWORD dateReturnRvas[32];
    int numLocalCalls;
    DWORD localReturnRvas[32];
};

static void* g_dummyTimePtr = (void*)GetTimeFormatEx;
static void* g_dummyDatePtr = (void*)GetDateFormatEx;
static void* g_dummyLocalPtr = (void*)GetLocalTime;

static BYTE* BuildFakePe(const PeBuilderConfig& cfg) {
    DWORD peSize = cfg.sizeOfImage ? cfg.sizeOfImage : 0x760000;
    DWORD allocSize = peSize + 0x10000;
    if (allocSize < 0x760000) allocSize = 0x760000;
    BYTE* base = (BYTE*)VirtualAlloc(nullptr, allocSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!base) return nullptr;
    memset(base, 0, allocSize);

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;

    PIMAGE_NT_HEADERS64 nt = (PIMAGE_NT_HEADERS64)(base + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = 2;
    nt->FileHeader.TimeDateStamp = cfg.timeDateStamp;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.ImageBase = (ULONG_PTR)base;
    nt->OptionalHeader.SectionAlignment = 0x1000;
    nt->OptionalHeader.FileAlignment = 0x200;
    nt->OptionalHeader.SizeOfImage = peSize;
    nt->OptionalHeader.SizeOfHeaders = 0x1000;
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;

    PIMAGE_SECTION_HEADER secText = IMAGE_FIRST_SECTION(nt);
    memcpy(secText->Name, ".text", 5);
    secText->VirtualAddress = 0x1000;
    secText->Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;

    PIMAGE_SECTION_HEADER secRdata = secText + 1;
    memcpy(secRdata->Name, ".rdata", 6);
    secRdata->Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;

    DWORD impRva = 0;
    DWORD strPoolRva = 0;
    DWORD thunkPoolRva = 0;

    if (cfg.timeDateStamp == 0x6AA9E607) {
        // SystemTray.dll 2609.11000.100.0: .text 覆盖全部调用点（最高 0x12CCEF）
        secText->Misc.VirtualSize = 0x18A000;
        secRdata->VirtualAddress = 0x18B000;
        secRdata->Misc.VirtualSize = peSize - 0x18B000;
        impRva = 0x1DE000;
        strPoolRva = 0x1DF000;
        thunkPoolRva = 0x1E0000;
    } else {
        // Taskbar.View.dll 623.27903.70.0
        secText->Misc.VirtualSize = 0x50000;
        secRdata->VirtualAddress = 0x60000;
        secRdata->Misc.VirtualSize = peSize - 0x60000;
        impRva = 0x100000;
        strPoolRva = 0x101000;
        thunkPoolRva = 0x102000;
    }

    DWORD textLimit = secText->VirtualAddress + secText->Misc.VirtualSize;

    for (int i = 0; i < cfg.numTimeCalls; ++i) {
        DWORD retRva = cfg.timeReturnRvas[i];
        if (retRva >= 6 && retRva < textLimit) {
            BYTE* callSite = base + retRva - 6;
            callSite[0] = 0xFF;
            callSite[1] = 0x15;
            *(INT32*)(callSite + 2) = (INT32)(cfg.timeSlotRva - retRva);
        }
    }
    for (int i = 0; i < cfg.numDateCalls; ++i) {
        DWORD retRva = cfg.dateReturnRvas[i];
        if (retRva >= 6 && retRva < textLimit) {
            BYTE* callSite = base + retRva - 6;
            callSite[0] = 0xFF;
            callSite[1] = 0x15;
            *(INT32*)(callSite + 2) = (INT32)(cfg.dateSlotRva - retRva);
        }
    }
    for (int i = 0; i < cfg.numLocalCalls; ++i) {
        DWORD retRva = cfg.localReturnRvas[i];
        if (retRva >= 6 && retRva < textLimit) {
            BYTE* callSite = base + retRva - 6;
            callSite[0] = 0xFF;
            callSite[1] = 0x15;
            *(INT32*)(callSite + 2) = (INT32)(cfg.localSlotRva - retRva);
        }
    }

    if (!cfg.emptyImports) {
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = impRva;
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = 0x1000;

        IMAGE_IMPORT_DESCRIPTOR* desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + impRva);
        int descIdx = 0;

        auto addImport = [&](const char* modName, const char* fnName, DWORD slotRva, void* dummyPtr) {
            DWORD modNameRva = strPoolRva;
            strcpy((char*)(base + modNameRva), modName);
            strPoolRva += (DWORD)(strlen(modName) + 1);

            DWORD fnNameRva = strPoolRva;
            *(WORD*)(base + fnNameRva) = 0;
            strcpy((char*)(base + fnNameRva + 2), fnName);
            strPoolRva += (DWORD)(strlen(fnName) + 3);

            DWORD oftRva = thunkPoolRva;
            thunkPoolRva += sizeof(IMAGE_THUNK_DATA64) * 2;
            if (cfg.hasOriginalFirstThunk) {
                IMAGE_THUNK_DATA64* oft = (IMAGE_THUNK_DATA64*)(base + oftRva);
                oft[0].u1.AddressOfData = fnNameRva;
                oft[1].u1.AddressOfData = 0;
            }

            IMAGE_THUNK_DATA64* ft = (IMAGE_THUNK_DATA64*)(base + slotRva);
            ft[0].u1.Function = (ULONG_PTR)dummyPtr;
            // 不对 ft[1] 清零，避免覆盖相邻紧凑排列的其它 IAT 槽

            desc[descIdx].Name = modNameRva;
            desc[descIdx].OriginalFirstThunk = cfg.hasOriginalFirstThunk ? oftRva : 0;
            desc[descIdx].FirstThunk = slotRva;
            descIdx++;
        };

        if (cfg.includeTime) {
            addImport(cfg.timeModuleName ? cfg.timeModuleName : "KERNEL32.dll",
                      "GetTimeFormatEx", cfg.timeSlotRva, g_dummyTimePtr);
        }
        if (cfg.duplicateTime) {
            addImport(cfg.timeModuleName ? cfg.timeModuleName : "KERNEL32.dll",
                      "GetTimeFormatEx", cfg.timeSlotRva + 0x10, g_dummyTimePtr);
        }
        if (cfg.includeDate) {
            addImport(cfg.dateModuleName ? cfg.dateModuleName : "KERNEL32.dll",
                      "GetDateFormatEx", cfg.dateSlotRva, g_dummyDatePtr);
        }
        if (cfg.includeLocal) {
            addImport(cfg.localModuleName ? cfg.localModuleName : "KERNEL32.dll",
                      "GetLocalTime", cfg.localSlotRva, g_dummyLocalPtr);
        }
        memset(&desc[descIdx], 0, sizeof(IMAGE_IMPORT_DESCRIPTOR));
    }

    return base;
}

static void DestroyFakePe(BYTE* base) {
    if (base) VirtualFree(base, 0, MEM_RELEASE);
}

// ---------------------------------------------------------------------------
// 辅助函数：构造 Taskbar.View.dll 布局
// ---------------------------------------------------------------------------
static PeBuilderConfig KnownConfig() {
    PeBuilderConfig c = {};
    c.timeDateStamp = 0x654E6445;
    c.sizeOfImage = 0x758000;
    c.hasOriginalFirstThunk = true;
    c.includeTime = c.includeDate = c.includeLocal = true;
    c.timeSlotRva = 0x617578;
    c.dateSlotRva = 0x617580;
    c.localSlotRva = 0x617880;
    c.numTimeCalls = 1;
    c.timeReturnRvas[0] = 0x359FF;
    c.numDateCalls = 2;
    c.dateReturnRvas[0] = 0x34DF9;
    c.dateReturnRvas[1] = 0x34E4F;
    c.numLocalCalls = 3;
    c.localReturnRvas[0] = 0x34C0C;
    c.localReturnRvas[1] = 0x34CB7;
    c.localReturnRvas[2] = 0x34D94;
    return c;
}

// ---------------------------------------------------------------------------
// 辅助函数：构造 SystemTray.dll 2609.11000.100.0 布局
// ---------------------------------------------------------------------------
static PeBuilderConfig SystemTrayConfig() {
    PeBuilderConfig c = {};
    c.timeDateStamp = 0x6AA9E607;
    c.sizeOfImage = 0x202000;
    c.hasOriginalFirstThunk = true;
    c.includeTime = c.includeDate = c.includeLocal = true;
    c.localSlotRva = 0x18B270;
    c.dateSlotRva  = 0x18B278;
    c.timeSlotRva  = 0x18B288;
    c.numTimeCalls = 7;
    DWORD timeSites[7] = { 0xC8150, 0xC85F3, 0xC8A7B, 0xC8A90, 0x129C9B, 0x129F4F, 0x129F64 };
    for (int i = 0; i < 7; ++i) c.timeReturnRvas[i] = timeSites[i];
    c.numDateCalls = 7;
    DWORD dateSites[7] = { 0xCC3F9, 0xCC5BE, 0xCC5F7, 0xCC9C6, 0x12CAFE, 0x12CCB9, 0x12CCEF };
    for (int i = 0; i < 7; ++i) c.dateReturnRvas[i] = dateSites[i];
    c.numLocalCalls = 4;
    DWORD localSites[4] = { 0xCA3BD, 0xCAA7D, 0x12B73D, 0x12BCED };
    for (int i = 0; i < 4; ++i) c.localReturnRvas[i] = localSites[i];
    return c;
}

// ---------------------------------------------------------------------------
// 测试用例 1: 缺少 OriginalFirstThunk / 空导入表 / 缺失导入防护
// ---------------------------------------------------------------------------
static int Test_ImportProbing_MissingOFT_And_EmptyImports() {
    printf("[TEST 1] Missing OFT and Empty Imports Protection...\n");
    PeBuilderConfig cfg = {};
    cfg.timeDateStamp = 0x654E6445;
    cfg.sizeOfImage = 0x758000;
    cfg.timeSlotRva = 0x617578;
    cfg.dateSlotRva = 0x617580;
    cfg.localSlotRva = 0x617880;

    cfg.emptyImports = true;
    BYTE* peEmpty = BuildFakePe(cfg);
    g_modBase = peEmpty;
    IatEntry entry = {};
    int r1 = 0;
    __try {
        r1 = FindImportSlot(g_modBase, nullptr, "GetTimeFormatEx", &entry.slot, &entry.original);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        printf("  FAIL: PatchImport crashed on empty import directory (0x%08X)!\n", GetExceptionCode());
        DestroyFakePe(peEmpty);
        return 1;
    }
    DestroyFakePe(peEmpty);
    if (r1 != 0) {
        printf("  FAIL: empty import descriptor should return 0 (got %d)\n", r1);
        return 1;
    }

    cfg.emptyImports = false;
    cfg.hasOriginalFirstThunk = false;
    cfg.includeTime = true;
    BYTE* peNoOft = BuildFakePe(cfg);
    g_modBase = peNoOft;
    memset(&entry, 0, sizeof(entry));
    int r2 = 0;
    __try {
        r2 = FindImportSlot(g_modBase, nullptr, "GetTimeFormatEx", &entry.slot, &entry.original);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        printf("  FAIL: PatchImport crashed on missing OFT (0x%08X)!\n", GetExceptionCode());
        DestroyFakePe(peNoOft);
        return 1;
    }
    DestroyFakePe(peNoOft);
    if (r2 != 0) {
        printf("  FAIL: missing OriginalFirstThunk must be rejected (got %d)\n", r2);
        return 1;
    }

    printf("  PASS: Test 1 ok\n");
    return 0;
}

// ---------------------------------------------------------------------------
// 测试用例 2: 17 个调用点超限拒绝（不改写 IAT，不截断启用）
// ---------------------------------------------------------------------------
static int Test_CallSites_ExceedLimit_Rejected() {
    printf("[TEST 2] 17 Call Sites Exceed Limit (Must Reject, Not Truncate)...\n");
    PeBuilderConfig cfg = {};
    cfg.timeDateStamp = 0x654E6445;
    cfg.sizeOfImage = 0x758000;
    cfg.hasOriginalFirstThunk = true;
    cfg.includeTime = true;
    cfg.includeDate = true;
    cfg.timeSlotRva = 0x617578;
    cfg.dateSlotRva = 0x617580;

    cfg.numTimeCalls = 17;
    for (int i = 0; i < 17; ++i) {
        cfg.timeReturnRvas[i] = 0x1000 + (i + 1) * 0x20;
    }
    cfg.numDateCalls = 2;
    cfg.dateReturnRvas[0] = 0x34DF9;
    cfg.dateReturnRvas[1] = 0x34E4F;

    BYTE* pe = BuildFakePe(cfg);
    g_modBase = pe;
    g_patched = FALSE;
    InterlockedExchange(&g_active, 0);

    BOOL activated = FALSE;
    __try {
        activated = ActivateHooks();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        printf("  FAIL: ActivateHooks crashed (0x%08X)!\n", GetExceptionCode());
        DestroyFakePe(pe);
        return 1;
    }

    void** timeSlot = (void**)(pe + cfg.timeSlotRva);
    void* slotValAfter = *timeSlot;

    DestroyFakePe(pe);

    if (activated) {
        printf("  FAIL: 17 call sites were accepted via truncation! Expected rejection.\n");
        return 1;
    }
    if (slotValAfter != g_dummyTimePtr) {
        printf("  FAIL: IAT slot was left modified after rejection! (val=%p, expected=%p)\n",
               slotValAfter, g_dummyTimePtr);
        return 1;
    }

    printf("  PASS: Test 2 ok\n");
    return 0;
}

// ---------------------------------------------------------------------------
// 测试用例 3: 已验证配置 vs 未知配置
// ---------------------------------------------------------------------------
static int Test_VerifiedLayout_Vs_UnknownLayout() {
    printf("[TEST 3] Verified Layout vs Unknown Layout...\n");

    PeBuilderConfig cfgUnknown = {};
    cfgUnknown.timeDateStamp = 0x12345678;
    cfgUnknown.sizeOfImage = 0x758000;
    cfgUnknown.hasOriginalFirstThunk = true;
    cfgUnknown.includeTime = true;
    cfgUnknown.includeDate = true;
    cfgUnknown.timeSlotRva = 0x617578;
    cfgUnknown.dateSlotRva = 0x617580;
    cfgUnknown.numTimeCalls = 7;
    for (int i = 0; i < 7; ++i) cfgUnknown.timeReturnRvas[i] = 0x2000 + i * 0x20;
    cfgUnknown.numDateCalls = 7;
    for (int i = 0; i < 7; ++i) cfgUnknown.dateReturnRvas[i] = 0x3000 + i * 0x20;

    BYTE* peUnknown = BuildFakePe(cfgUnknown);
    g_modBase = peUnknown;
    g_patched = FALSE;
    InterlockedExchange(&g_active, 0);

    BOOL rUnknown = FALSE;
    __try {
        rUnknown = ActivateHooks();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        printf("  FAIL: ActivateHooks crashed on unknown PE (0x%08X)!\n", GetExceptionCode());
        DestroyFakePe(peUnknown);
        return 1;
    }
    DestroyFakePe(peUnknown);

    if (rUnknown) {
        printf("  FAIL: Unknown layout was accepted! Design requires strict verification.\n");
        return 1;
    }

    PeBuilderConfig cfgKnown = KnownConfig();
    BYTE* peKnown = BuildFakePe(cfgKnown);
    g_modBase = peKnown;
    g_patched = FALSE;
    InterlockedExchange(&g_active, 0);

    BOOL rKnown = FALSE;
    __try {
        rKnown = ActivateHooks();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        printf("  FAIL: ActivateHooks crashed on known PE (0x%08X)!\n", GetExceptionCode());
        DestroyFakePe(peKnown);
        return 1;
    }
    DestroyFakePe(peKnown);

    if (!rKnown) {
        printf("  FAIL: Verified layout (Taskbar.View.dll 623.27903) was rejected!\n");
        return 1;
    }

    printf("  PASS: Test 3 ok\n");
    return 0;
}

// ---------------------------------------------------------------------------
// 测试用例 4: 第一候选失败，回退第二候选成功
// ---------------------------------------------------------------------------
static BYTE* g_testFakeMod1 = nullptr;
static BYTE* g_testFakeMod2 = nullptr;

static HMODULE WINAPI TestResolver(LPCWSTR modName) {
    if (_wcsicmp(modName, L"SystemTray.dll") == 0) return (HMODULE)g_testFakeMod1;
    if (_wcsicmp(modName, L"Taskbar.View.dll") == 0) return (HMODULE)g_testFakeMod2;
    return nullptr;
}

static int Test_CandidateFallback_FirstFails_SecondSucceeds() {
    printf("[TEST 4] Candidate Fallback (SystemTray fails -> Taskbar.View succeeds)...\n");

    PeBuilderConfig cfg1 = {};
    cfg1.timeDateStamp = 0x99999999;
    cfg1.sizeOfImage = 0x758000;
    cfg1.emptyImports = true;
    g_testFakeMod1 = BuildFakePe(cfg1);

    PeBuilderConfig cfg2 = KnownConfig();
    g_testFakeMod2 = BuildFakePe(cfg2);

    g_testModuleResolver = TestResolver;

    HMODULE chosenMod = nullptr;
    const WCHAR* chosenName = nullptr;
    ProbedModuleInfo selected;
    bool fallbackSuccess = FindSupportedCandidate(&chosenMod, &chosenName, &selected) != FALSE;
    if (fallbackSuccess) {
        g_modBase = (BYTE*)chosenMod; g_patched = FALSE; g_active = 0;
        fallbackSuccess = ActivateHooks() != FALSE;
    }
    BYTE* expectedMod = g_testFakeMod2;
    DestroyFakePe(g_testFakeMod1);
    DestroyFakePe(g_testFakeMod2);
    g_testFakeMod1 = nullptr;
    g_testFakeMod2 = nullptr;
    g_testModuleResolver = nullptr;

    if (!fallbackSuccess || chosenMod != (HMODULE)expectedMod) {
        printf("  FAIL: Fallback to second candidate failed!\n");
        return 1;
    }

    printf("  PASS: Test 4 ok\n");
    return 0;
}

// ---------------------------------------------------------------------------
// 测试用例 5: 安装失败回滚与发布顺序
// ---------------------------------------------------------------------------
static int Test_InstallFailure_Rollback_And_PublishOrder() {
    printf("[TEST 5] Install Failure Rollback & Publish Order...\n");
    PeBuilderConfig cfg = KnownConfig();
    BYTE* pe = BuildFakePe(cfg);
    g_modBase = pe;
    g_patched = FALSE;
    InterlockedExchange(&g_active, 0);

    BYTE* dateSite = pe + 0x34DF9 - 6;
    dateSite[0] = 0x90; dateSite[1] = 0x90;

    BOOL r = FALSE;
    __try {
        r = ActivateHooks();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        r = FALSE;
    }

    void** timeSlot = (void**)(pe + cfg.timeSlotRva);
    void* slotVal = *timeSlot;

    DestroyFakePe(pe);

    if (r) {
        printf("  FAIL: Hook activation succeeded when date call sites were missing!\n");
        return 1;
    }
    if (slotVal != g_dummyTimePtr) {
        printf("  FAIL: Rollback failed, timeSlot left as %p\n", slotVal);
        return 1;
    }
    if (g_patched) {
        printf("  FAIL: g_patched is TRUE after failed installation!\n");
        return 1;
    }

    printf("  PASS: Test 5 ok\n");
    return 0;
}

// ---------------------------------------------------------------------------
// 测试用例 6: 未授权调用与 GetLocalTime kick / SYSTEMTIME 透传 (Taskbar.View)
// ---------------------------------------------------------------------------
static int Test_UnauthorizedCallSites_And_SystemTimePassthrough() {
    printf("[TEST 6] Unauthorized Call Sites Passthrough & GetLocalTime Kick...\n");

    PeBuilderConfig cfg = KnownConfig();
    BYTE* pe = BuildFakePe(cfg);
    g_modBase = pe;
    g_patched = FALSE;
    InterlockedExchange(&g_active, 1);
    g_currentLayout = &kVerifiedLayouts[1]; // Taskbar.View layout

    auto fakeOrigLocal = [](LPSYSTEMTIME st) {
        st->wYear = 2026; st->wMonth = 11; st->wDay = 2;
        st->wHour = 10; st->wMinute = 30; st->wSecond = 15;
    };
    g_origLocalTime = fakeOrigLocal;

    InterlockedExchange(&g_kickPending, 1);
    SYSTEMTIME st1 = {};
    HookGetLocalTime(&st1);
    if (st1.wSecond != 15) {
        printf("  FAIL: Unauthorized GetLocalTime call site altered wSecond to %d!\n", st1.wSecond);
        DestroyFakePe(pe);
        return 1;
    }
    if (g_kickPending != 1) {
        printf("  FAIL: Unauthorized GetLocalTime consumed g_kickPending!\n");
        DestroyFakePe(pe);
        return 1;
    }

    if (!IsClockLocalCallSite(pe + 0x34C0C)) {
        printf("  FAIL: 0x34C0C is not recognized as a valid clock kick site!\n");
        DestroyFakePe(pe);
        return 1;
    }
    if (IsClockLocalCallSite(pe + 0x34CB7)) {
        printf("  FAIL: 0x34CB7 should NOT be permitted to consume kick (must passthrough)!\n");
        DestroyFakePe(pe);
        return 1;
    }
    if (IsClockLocalCallSite(pe + 0x34D94)) {
        printf("  FAIL: 0x34D94 should NOT be permitted to consume kick (must passthrough)!\n");
        DestroyFakePe(pe);
        return 1;
    }

    DestroyFakePe(pe);
    printf("  PASS: Test 6 ok\n");
    return 0;
}

// ---------------------------------------------------------------------------
// 测试用例 8: 外来 Hook、重复导入与边界越界
// ---------------------------------------------------------------------------
static int Test_RejectForeignHooks_And_BoundedNames() {
    printf("[TEST 8] Foreign hook, duplicate import and out-of-image name...\n");
    int failed = 0;
    PeBuilderConfig c = KnownConfig();
    BYTE* pe = BuildFakePe(c);
    *(void**)(pe + c.timeSlotRva) = (void*)HookGetTimeFormatEx;
    ProbedModuleInfo info;
    if (ProbeCandidateModule(pe, L"Taskbar.View.dll", &info)) {
        printf("  FAIL: pre-existing hook must not be chained\n"); ++failed;
    }
    DestroyFakePe(pe);

    c.duplicateTime = true;
    pe = BuildFakePe(c);
    void** slot = nullptr; void* orig = nullptr;
    if (FindImportSlot(pe, nullptr, "GetTimeFormatEx", &slot, &orig) != -2) {
        printf("  FAIL: duplicate API import was accepted\n"); ++failed;
    }
    DestroyFakePe(pe);

    c.duplicateTime = false;
    pe = BuildFakePe(c);
    auto nt = (IMAGE_NT_HEADERS64*)(pe + 0x80);
    auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(pe + nt->OptionalHeader.DataDirectory[1].VirtualAddress);
    auto oft = (IMAGE_THUNK_DATA64*)(pe + imp->OriginalFirstThunk);
    oft[0].u1.AddressOfData = c.sizeOfImage - 8;
    strcpy((char*)(pe + c.sizeOfImage - 6), "GetTimeFormatEx");
    if (FindImportSlot(pe, nullptr, "GetTimeFormatEx", &slot, &orig) == 1) {
        printf("  FAIL: name read beyond SizeOfImage\n"); ++failed;
    }
    DestroyFakePe(pe);
    return failed ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 测试用例 9: 格式化透传与地址范围判定
// ---------------------------------------------------------------------------
static int Test_FormattingPassthrough_And_AddressRange() {
    printf("[TEST 9] Formatting preserves the caller's original date and time...\n");
    PeBuilderConfig c = KnownConfig();
    BYTE* pe = BuildFakePe(c);
    g_modBase = pe; g_patched = FALSE; g_active = 0;
    if (!ActivateHooks()) { DestroyFakePe(pe); return 1; }
    SYSTEMTIME input = { 2001, 2, 0, 3, 4, 5, 6, 0 };
    WCHAR actual[64] = {}, expected[64] = {};
    int failed = 0;
    GetTimeFormatEx(LOCALE_NAME_INVARIANT, 0, &input, L"HH':'mm':'ss", expected, 64);
    HookGetTimeFormatEx(LOCALE_NAME_INVARIANT, 0, &input, L"HH':'mm':'ss", actual, 64);
    if (wcscmp(actual, expected)) { printf("  FAIL: original time changed\n"); ++failed; }
    GetDateFormatEx(LOCALE_NAME_INVARIANT, 0, &input, L"yyyy'-'MM'-'dd", expected, 64, nullptr);
    HookGetDateFormatEx(LOCALE_NAME_INVARIANT, 0, &input, L"yyyy'-'MM'-'dd", actual, 64, nullptr);
    if (wcscmp(actual, expected)) { printf("  FAIL: original date changed\n"); ++failed; }
    void* aliased = (void*)((ULONG_PTR)pe + 0x100000000ULL + 0x359FF);
    if (IsClockCallSite(aliased)) { printf("  FAIL: return address outside host was accepted\n"); ++failed; }
    DestroyFakePe(pe);
    return failed ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 测试用例 10: 本机 Taskbar.View.dll 只读探测 (若存在)
// ---------------------------------------------------------------------------
static int Test_LocalSystemImage() {
    printf("[TEST 10] Probe the real local Taskbar.View.dll without running its code...\n");
    WCHAR path[MAX_PATH];
    GetWindowsDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\SystemApps\\MicrosoftWindows.Client.Core_cw5n1h2txyewy\\Taskbar.View.dll");
    HMODULE module = LoadLibraryExW(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!module) { printf("  SKIP: local image unavailable\n"); return 0; }
    BYTE* base = (BYTE*)module;
    auto nt = ImageHeaders(base);
    if (!nt || nt->FileHeader.TimeDateStamp != 0x654E6445 || nt->OptionalHeader.SizeOfImage != 0x758000) {
        printf("  SKIP: local image has no verified fixture\n"); FreeLibrary(module); return 0;
    }
    const char* names[] = { "GetTimeFormatEx", "GetDateFormatEx", "GetLocalTime" };
    void* targets[] = { (void*)GetTimeFormatEx, (void*)GetDateFormatEx, (void*)GetLocalTime };
    void** slots[3] = {}; void* saved[3] = {};
    int initialized = 0, failed = 0;
    for (int i = 0; i < 3; ++i) {
        if (FindImportSlot(base, nullptr, names[i], &slots[i], &saved[i]) != 1) { failed = 1; break; }
        DWORD old;
        if (!VirtualProtect(slots[i], sizeof(void*), PAGE_READWRITE, &old)) { failed = 1; break; }
        *slots[i] = targets[i];
        DWORD ignored; VirtualProtect(slots[i], sizeof(void*), old, &ignored);
        ++initialized;
    }
    ProbedModuleInfo info;
    if (!failed && !ProbeCandidateModule(base, L"Taskbar.View.dll", &info)) failed = 1;
    for (int i = 0; i < initialized; ++i) {
        DWORD old;
        if (VirtualProtect(slots[i], sizeof(void*), PAGE_READWRITE, &old)) {
            *slots[i] = saved[i]; DWORD ignored; VirtualProtect(slots[i], sizeof(void*), old, &ignored);
        }
    }
    FreeLibrary(module);
    printf(failed ? "  FAIL: real image probe failed\n" : "  PASS: real image identity and all call sites verified\n");
    return failed;
}

// ---------------------------------------------------------------------------
// 测试用例 11: 真实下载的 SystemTray.dll 2609.11000.100.0 只读核验与策略验证
// ---------------------------------------------------------------------------
static int Test_SystemTrayRealImage_ReadOnlyProbe() {
    printf("[TEST 11] Probe real downloaded SystemTray-2609.11000.100.0.dll without executing code...\n");
    const WCHAR* path = L"artifacts\\build26300\\SystemTray-2609.11000.100.0.dll";
    WCHAR fullPath[MAX_PATH] = {};
    GetFullPathNameW(path, MAX_PATH, fullPath, nullptr);
    HMODULE module = LoadLibraryExW(fullPath, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!module) {
        printf("  FAIL: could not load real downloaded SystemTray.dll at %ls (err=%lu)\n", fullPath, GetLastError());
        return 1;
    }
    BYTE* base = (BYTE*)module;
    auto nt = ImageHeaders(base);
    if (!nt) {
        printf("  FAIL: invalid PE headers on real SystemTray.dll\n");
        FreeLibrary(module);
        return 1;
    }

    if (nt->FileHeader.TimeDateStamp != 0x6AA9E607 ||
        nt->OptionalHeader.SizeOfImage != 0x202000 ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        printf("  FAIL: PE header mismatch on real SystemTray.dll (ts=0x%08X, sz=0x%08X, mach=0x%04X)\n",
               nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage, nt->FileHeader.Machine);
        FreeLibrary(module);
        return 1;
    }

    // 填入 3 个系统 IAT 临时槽用于验证
    const char* names[] = { "GetTimeFormatEx", "GetDateFormatEx", "GetLocalTime" };
    void* targets[] = { (void*)GetTimeFormatEx, (void*)GetDateFormatEx, (void*)GetLocalTime };
    void** slots[3] = {}; void* saved[3] = {};
    int initialized = 0, failed = 0;
    for (int i = 0; i < 3; ++i) {
        if (FindImportSlot(base, nullptr, names[i], &slots[i], &saved[i]) != 1) {
            printf("  FAIL: FindImportSlot failed for %s\n", names[i]);
            failed = 1; break;
        }
        DWORD old;
        if (!VirtualProtect(slots[i], sizeof(void*), PAGE_READWRITE, &old)) {
            printf("  FAIL: VirtualProtect failed for %s\n", names[i]);
            failed = 1; break;
        }
        *slots[i] = targets[i];
        DWORD ignored; VirtualProtect(slots[i], sizeof(void*), old, &ignored);
        ++initialized;
    }

    ProbedModuleInfo info;
    if (!failed) {
        if (!ProbeCandidateModule(base, L"SystemTray.dll", &info)) {
            printf("  FAIL: ProbeCandidateModule rejected real SystemTray.dll\n");
            failed = 1;
        } else {
            if (info.layout->strategy != STRATEGY_LOCAL_SOURCE) {
                printf("  FAIL: expected STRATEGY_LOCAL_SOURCE, got %d\n", (int)info.layout->strategy);
                failed = 1;
            }
            if (info.layout->timeCount != 7 || info.layout->dateCount != 7 ||
                info.layout->localSourceCount != 2 || info.layout->localScheduleCount != 2) {
                printf("  FAIL: callsite counts mismatch (time=%d, date=%d, src=%d, sch=%d)\n",
                       info.layout->timeCount, info.layout->dateCount,
                       info.layout->localSourceCount, info.layout->localScheduleCount);
                failed = 1;
            }
        }
    }

    // 还原临时填入的 IAT 槽
    for (int i = 0; i < initialized; ++i) {
        DWORD old;
        if (VirtualProtect(slots[i], sizeof(void*), PAGE_READWRITE, &old)) {
            *slots[i] = saved[i];
            DWORD ignored; VirtualProtect(slots[i], sizeof(void*), old, &ignored);
        }
    }
    FreeLibrary(module);

    if (!failed) {
        printf("  PASS: real SystemTray.dll verified (18 call sites and identity match)\n");
    }
    return failed;
}

// ---------------------------------------------------------------------------
// 测试用例 12: SystemTray 布局合法接纳与异常拒绝
// ---------------------------------------------------------------------------
static int Test_SystemTray_Acceptance_And_Rejection() {
    printf("[TEST 12] SystemTray layout acceptance and rejection checks...\n");
    int failed = 0;

    // 12a: 合法配置接受
    PeBuilderConfig c = SystemTrayConfig();
    BYTE* pe = BuildFakePe(c);
    ProbedModuleInfo info;
    if (!ProbeCandidateModule(pe, L"SystemTray.dll", &info)) {
        printf("  FAIL: valid SystemTray layout was rejected\n"); ++failed;
    } else {
        if (info.layout->strategy != STRATEGY_LOCAL_SOURCE) {
            printf("  FAIL: wrong strategy for SystemTray\n"); ++failed;
        }
    }
    DestroyFakePe(pe);

    // 12b: 模块名错误被拒
    pe = BuildFakePe(c);
    if (ProbeCandidateModule(pe, L"Taskbar.View.dll", &info)) {
        printf("  FAIL: SystemTray PE accepted under Taskbar.View name\n"); ++failed;
    }
    DestroyFakePe(pe);

    // 12c: 时间戳/身份错误被拒
    c.timeDateStamp = 0x11223344;
    pe = BuildFakePe(c);
    if (ProbeCandidateModule(pe, L"SystemTray.dll", &info)) {
        printf("  FAIL: wrong TimeDateStamp was accepted\n"); ++failed;
    }
    DestroyFakePe(pe);

    // 12d: 映像大小错误被拒
    c = SystemTrayConfig();
    c.sizeOfImage = 0x300000;
    pe = BuildFakePe(c);
    if (ProbeCandidateModule(pe, L"SystemTray.dll", &info)) {
        printf("  FAIL: wrong SizeOfImage was accepted\n"); ++failed;
    }
    DestroyFakePe(pe);

    // 12e: 漏 local 调用点被拒 (3 local sites instead of 4)
    c = SystemTrayConfig();
    c.numLocalCalls = 3;
    pe = BuildFakePe(c);
    if (ProbeCandidateModule(pe, L"SystemTray.dll", &info)) {
        printf("  FAIL: missing local call site was accepted\n"); ++failed;
    }
    DestroyFakePe(pe);

    // 12f: 多 local 调用点被拒 (5 local sites instead of 4)
    c = SystemTrayConfig();
    c.numLocalCalls = 5;
    c.localReturnRvas[4] = 0x130000;
    pe = BuildFakePe(c);
    if (ProbeCandidateModule(pe, L"SystemTray.dll", &info)) {
        printf("  FAIL: extra local call site was accepted\n"); ++failed;
    }
    DestroyFakePe(pe);

    // 12g: 漏 date 调用点被拒 (6 date sites instead of 7)
    c = SystemTrayConfig();
    c.numDateCalls = 6;
    pe = BuildFakePe(c);
    if (ProbeCandidateModule(pe, L"SystemTray.dll", &info)) {
        printf("  FAIL: missing date call site was accepted\n"); ++failed;
    }
    DestroyFakePe(pe);

    // 12h: 多 date 调用点被拒 (8 date sites instead of 7)
    c = SystemTrayConfig();
    c.numDateCalls = 8;
    c.dateReturnRvas[7] = 0x131000;
    pe = BuildFakePe(c);
    if (ProbeCandidateModule(pe, L"SystemTray.dll", &info)) {
        printf("  FAIL: extra date call site was accepted\n"); ++failed;
    }
    DestroyFakePe(pe);

    // 12i: 漏 time 调用点被拒 (6 time sites instead of 7)
    c = SystemTrayConfig();
    c.numTimeCalls = 6;
    pe = BuildFakePe(c);
    if (ProbeCandidateModule(pe, L"SystemTray.dll", &info)) {
        printf("  FAIL: missing time call site was accepted\n"); ++failed;
    }
    DestroyFakePe(pe);

    if (!failed) printf("  PASS: Test 12 ok\n");
    return failed ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 测试用例 13: 新策略仅挂钩 local 槽，time/date 绝对不写，失败回滚安全
// ---------------------------------------------------------------------------
static int Test_SystemTray_Activation_And_SlotStrategy() {
    printf("[TEST 13] SystemTray slot strategy & rollback safety...\n");
    int failed = 0;

    g_installPoisoned = FALSE;
    g_installError = 0;

    PeBuilderConfig c = SystemTrayConfig();
    BYTE* pe = BuildFakePe(c);
    g_modBase = pe; g_patched = FALSE; g_active = 0;

    if (!ActivateHooks()) {
        printf("  FAIL: ActivateHooks failed on SystemTray\n"); ++failed;
    } else {
        // 关键安全断言：只挂钩 local 槽，time 和 date 槽绝不修改
        void* timeSlotVal = *(void**)(pe + c.timeSlotRva);
        void* dateSlotVal = *(void**)(pe + c.dateSlotRva);
        void* localSlotVal = *(void**)(pe + c.localSlotRva);

        if (timeSlotVal != g_dummyTimePtr) {
            printf("  FAIL: timeSlot was modified under STRATEGY_LOCAL_SOURCE! (%p != %p)\n",
                   timeSlotVal, g_dummyTimePtr);
            ++failed;
        }
        if (dateSlotVal != g_dummyDatePtr) {
            printf("  FAIL: dateSlot was modified under STRATEGY_LOCAL_SOURCE! (%p != %p)\n",
                   dateSlotVal, g_dummyDatePtr);
            ++failed;
        }
        if (localSlotVal != (void*)HookGetLocalTime) {
            printf("  FAIL: localSlot was not patched to HookGetLocalTime! (%p)\n", localSlotVal);
            ++failed;
        }

        if (g_iatTime.slot != nullptr || g_iatDate.slot != nullptr ||
            g_iatLocal.slot != (void**)(pe + c.localSlotRva)) {
            printf("  FAIL: g_iat recorded wrong slots\n");
            ++failed;
        }
    }
    DestroyFakePe(pe);

    // 回滚测试：在 local 槽 VirtualProtect 失败时回滚
    pe = BuildFakePe(c);
    g_modBase = pe; g_patched = FALSE; g_active = 0;
    g_protectCalls = 0;
    g_failProtectMask = (1ULL << 0); // 第一次 VirtualProtect (保护 localSlot) 失败
    BOOL ok = ActivateHooks();
    g_failProtectMask = 0;
    if (ok || *(void**)(pe + c.localSlotRva) != g_dummyLocalPtr || g_active) {
        printf("  FAIL: local slot rollback did not restore slot safely\n");
        ++failed;
    }
    DestroyFakePe(pe);

    if (!failed) printf("  PASS: Test 13 ok\n");
    return failed ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 测试用例 14: SystemTray HookGetLocalTime 行为与真实 x64 stub 返回调用
// ---------------------------------------------------------------------------
typedef void (*PfnCallStub)(SYSTEMTIME* st);

// 完整时间必须处于两次目标时区取样之间，不得跨取样拼接字段。
static bool WithinTargetWindow(const SYSTEMTIME& actual, const SYSTEMTIME& before, const SYSTEMTIME& after) {
    FILETIME a = {}, b = {}, c = {};
    if (!SystemTimeToFileTime(&actual, &a) || !SystemTimeToFileTime(&before, &b) ||
        !SystemTimeToFileTime(&after, &c)) return false;
    ULONGLONG value = ((ULONGLONG)a.dwHighDateTime << 32) | a.dwLowDateTime;
    ULONGLONG lower = ((ULONGLONG)b.dwHighDateTime << 32) | b.dwLowDateTime;
    ULONGLONG upper = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
    return lower <= value && value <= upper;
}

static void WINAPI FakeSourceLocalTime(LPSYSTEMTIME st) {
    if (st) {
        st->wYear = 2026;
        st->wMonth = 11;
        st->wDay = 2;
        st->wHour = 10;
        st->wMinute = 30;
        st->wSecond = 15;
        st->wMilliseconds = 250;
    }
}

static int Test_SystemTray_HookGetLocalTime_Behavior_And_Stub() {
    printf("[TEST 14] SystemTray HookGetLocalTime behavior & executable call stubs...\n");
    int failed = 0;

    g_installPoisoned = FALSE;
    g_installError = 0;

    PeBuilderConfig c = SystemTrayConfig();
    BYTE* pe = BuildFakePe(c);
    g_modBase = pe; g_patched = FALSE; g_active = 0;
    if (!ActivateHooks()) {
        printf("  FAIL: ActivateHooks failed\n");
        DestroyFakePe(pe);
        return 1;
    }

    // 设置目标时区为 China Standard Time (Windows 标配命名时区)
    wcscpy_s(g_cfg.timeZoneId, L"China Standard Time");
    if (!LoadTargetZone()) {
        printf("  FAIL: LoadTargetZone failed for China Standard Time\n");
        DestroyFakePe(pe);
        return 1;
    }

    g_origLocalTime = FakeSourceLocalTime;

    // Helper 分类测试
    if (ClassifyReturnAddress(pe + 0xCA3BD) != SITE_LOCAL_SOURCE ||
        ClassifyReturnAddress(pe + 0x12B73D) != SITE_LOCAL_SOURCE) {
        printf("  FAIL: RefreshIcon source sites not classified as SITE_LOCAL_SOURCE\n");
        ++failed;
    }
    if (ClassifyReturnAddress(pe + 0xCAA7D) != SITE_LOCAL_SCHEDULE ||
        ClassifyReturnAddress(pe + 0x12BCED) != SITE_LOCAL_SCHEDULE) {
        printf("  FAIL: Schedule sites not classified as SITE_LOCAL_SCHEDULE\n");
        ++failed;
    }
    if (ClassifyReturnAddress(pe + 0x1000) != SITE_UNKNOWN) {
        printf("  FAIL: unknown site not classified as SITE_UNKNOWN\n");
        ++failed;
    }

    // 14a: 真实小 stub 从数据源点 0xCA3BD 返回调用 HookGetLocalTime
    auto buildStub = [&](DWORD retRva) -> PfnCallStub {
        BYTE* entry = pe + retRva - 10;
        entry[0] = 0x48; entry[1] = 0x83; entry[2] = 0xEC; entry[3] = 0x28; // sub rsp, 40
        entry[4] = 0xFF; entry[5] = 0x15;                                   // call [rel32]
        *(INT32*)(entry + 6) = (INT32)(c.localSlotRva - retRva);
        BYTE* retPoint = pe + retRva;
        retPoint[0] = 0x48; retPoint[1] = 0x83; retPoint[2] = 0xC4; retPoint[3] = 0x28; // add rsp, 40
        retPoint[4] = 0xC3;                                                             // ret
        return (PfnCallStub)entry;
    };

    PfnCallStub stubSrc1 = buildStub(0xCA3BD);
    PfnCallStub stubSrc2 = buildStub(0x12B73D);
    PfnCallStub stubSch1 = buildStub(0xCAA7D);
    PfnCallStub stubSch2 = buildStub(0x12BCED);

    // --- 场景 1: active = 1, 数据源点 0xCA3BD ---
    InterlockedExchange(&g_active, 1);
    InterlockedExchange(&g_kickPending, 1);
    InterlockedExchange(&g_hits, 0);

    SYSTEMTIME st = {};
    SYSTEMTIME beforeTarget = {};
    TargetNow(&beforeTarget);
    stubSrc1(&st);

    SYSTEMTIME expectedTarget = {};
    TargetNow(&expectedTarget);

    if (!WithinTargetWindow(st, beforeTarget, expectedTarget)) {
        printf("  FAIL: src1 did not replace with target timezone time (got %02d:%02d, expected %02d:%02d)\n",
               st.wHour, st.wMinute, expectedTarget.wHour, expectedTarget.wMinute);
        ++failed;
    }
    if (g_hits != 1) {
        printf("  FAIL: src1 did not increment g_hits (hits=%d)\n", g_hits);
        ++failed;
    }
    if (g_kickPending != 1) {
        printf("  FAIL: src1 consumed g_kickPending (must NOT consume kick)\n");
        ++failed;
    }

    // --- 场景 2: active = 1, 第二数据源点 0x12B73D ---
    memset(&st, 0, sizeof(st));
    TargetNow(&beforeTarget);
    stubSrc2(&st);
    TargetNow(&expectedTarget);
    if (!WithinTargetWindow(st, beforeTarget, expectedTarget) || g_hits != 2 || g_kickPending != 1) {
        printf("  FAIL: src2 did not behave identically to src1\n");
        ++failed;
    }

    // --- 场景 3: active = 1, 调度点 0xCAA7D ---
    memset(&st, 0, sizeof(st));
    stubSch1(&st);
    if (st.wHour != 10 || st.wMinute != 30) {
        printf("  FAIL: sch1 altered timezone hours/minutes! (got %02d:%02d)\n", st.wHour, st.wMinute);
        ++failed;
    }
    if (st.wSecond != 59) {
        printf("  FAIL: sch1 did not set seconds to 59 upon consuming kick (got %d)\n", st.wSecond);
        ++failed;
    }
    if (g_kickPending != 0) {
        printf("  FAIL: sch1 did not clear g_kickPending\n");
        ++failed;
    }
    if (g_hits != 2) {
        printf("  FAIL: sch1 incorrectly incremented g_hits\n");
        ++failed;
    }

    // 再次调用 sch1（无 kickPending）：秒数保持原样 15
    memset(&st, 0, sizeof(st));
    stubSch1(&st);
    if (st.wSecond != 15) {
        printf("  FAIL: sch1 without kick changed seconds (got %d)\n", st.wSecond);
        ++failed;
    }

    // --- 场景 4: active = 1, 第二调度点 0x12BCED ---
    InterlockedExchange(&g_kickPending, 1);
    memset(&st, 0, sizeof(st));
    stubSch2(&st);
    if (st.wSecond != 59 || g_kickPending != 0 || st.wHour != 10) {
        printf("  FAIL: sch2 did not behave correctly\n");
        ++failed;
    }

    // --- 场景 5: off 状态 (active = 0) ---
    InterlockedExchange(&g_active, 0);
    InterlockedExchange(&g_kickPending, 1);
    InterlockedExchange(&g_hits, 0);

    memset(&st, 0, sizeof(st));
    stubSrc1(&st);
    if (st.wHour != 10 || st.wMinute != 30 || st.wSecond != 15) {
        printf("  FAIL: src1 in off state altered time! (got %02d:%02d:%02d)\n",
               st.wHour, st.wMinute, st.wSecond);
        ++failed;
    }
    if (g_hits != 0 || g_kickPending != 1) {
        printf("  FAIL: src1 in off state altered hits or kick\n");
        ++failed;
    }

    memset(&st, 0, sizeof(st));
    stubSch1(&st);
    if (st.wSecond != 59 || g_kickPending != 0 || st.wHour != 10) {
        printf("  FAIL: sch1 in off state did not consume kick or changed hour\n");
        ++failed;
    }

    // --- 场景 6: 未知调用点 (透传) ---
    InterlockedExchange(&g_active, 1);
    InterlockedExchange(&g_kickPending, 1);
    memset(&st, 0, sizeof(st));
    HookGetLocalTime(&st); // _ReturnAddress 位于本测试函数内部，属于未知调用点
    if (st.wHour != 10 || st.wMinute != 30 || st.wSecond != 15 || g_kickPending != 1) {
        printf("  FAIL: unknown call site altered time or consumed kick\n");
        ++failed;
    }

    DestroyFakePe(pe);
    if (!failed) printf("  PASS: Test 14 ok\n");
    return failed ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 测试用例 15: 附加时钟格式化透传测试 (SystemTray.dll)
// ---------------------------------------------------------------------------
static int Test_SystemTray_AdditionalClock_Formatting_Passthrough() {
    printf("[TEST 15] Additional clock formatting passthrough (SystemTray layout)...\n");
    PeBuilderConfig c = SystemTrayConfig();
    BYTE* pe = BuildFakePe(c);
    g_modBase = pe; g_patched = FALSE; g_active = 0;
    if (!ActivateHooks()) { DestroyFakePe(pe); return 1; }

    int failed = 0;

    // 验证 time 和 date 槽在 STRATEGY_LOCAL_SOURCE 下完全未被写入
    if (*(void**)(pe + c.timeSlotRva) != g_dummyTimePtr) {
        printf("  FAIL: timeSlot was modified in STRATEGY_LOCAL_SOURCE!\n"); ++failed;
    }
    if (*(void**)(pe + c.dateSlotRva) != g_dummyDatePtr) {
        printf("  FAIL: dateSlot was modified in STRATEGY_LOCAL_SOURCE!\n"); ++failed;
    }

    // 直接调用 Hook 格式化函数，输入附加时钟的独立时区时间
    SYSTEMTIME inputTime = { 2026, 11, 1, 2, 18, 45, 30, 123 };
    WCHAR actualTime[64] = {}, expectedTime[64] = {};
    GetTimeFormatEx(LOCALE_NAME_INVARIANT, 0, &inputTime, L"HH':'mm':'ss", expectedTime, 64);
    HookGetTimeFormatEx(LOCALE_NAME_INVARIANT, 0, &inputTime, L"HH':'mm':'ss", actualTime, 64);
    if (wcscmp(actualTime, expectedTime) != 0) {
        printf("  FAIL: HookGetTimeFormatEx altered additional clock time!\n"); ++failed;
    }

    WCHAR actualDate[64] = {}, expectedDate[64] = {};
    GetDateFormatEx(LOCALE_NAME_INVARIANT, 0, &inputTime, L"yyyy'-'MM'-'dd", expectedDate, 64, nullptr);
    HookGetDateFormatEx(LOCALE_NAME_INVARIANT, 0, &inputTime, L"yyyy'-'MM'-'dd", actualDate, 64, nullptr);
    if (wcscmp(actualDate, expectedDate) != 0) {
        printf("  FAIL: HookGetDateFormatEx altered additional clock date!\n"); ++failed;
    }

    DestroyFakePe(pe);
    if (!failed) printf("  PASS: Test 15 ok\n");
    return failed ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 测试用例 7: 真实写入失败与回滚保护 (放在最后执行，避免毒化全局状态)
// ---------------------------------------------------------------------------
static int Test_RealWriteFailures() {
    printf("[TEST 7] Actual IAT write failure and failed rollback...\n");
    int failed = 0;
    PeBuilderConfig c = KnownConfig();
    BYTE* pe = BuildFakePe(c);
    g_modBase = pe; g_patched = FALSE; g_active = 0;
    g_protectCalls = 0;
    g_failProtectMask = (1ULL << 2); // 第二个槽写入失败
    BOOL ok = ActivateHooks();
    g_failProtectMask = 0;
    if (ok || *(void**)(pe + c.timeSlotRva) != g_dummyTimePtr || g_active ||
        !g_origTimeFmt || !g_origDateFmt || !g_origLocalTime) {
        printf("  FAIL: rollback must restore slots and retain in-flight originals\n");
        ++failed;
    }
    DestroyFakePe(pe);

    pe = BuildFakePe(c);
    g_modBase = pe; g_patched = FALSE; g_active = 0;
    g_protectCalls = 0;
    g_failProtectMask = (1ULL << 2) | (1ULL << 3); // 写入及其回滚同时失败
    ok = ActivateHooks();
    g_failProtectMask = 0;
    if (ok || g_active || g_origTimeFmt != GetTimeFormatEx) {
        printf("  FAIL: failed rollback left an unsafe original pointer\n");
        ++failed;
    }
    unsigned beforeRetry = g_protectCalls;
    if (ActivateHooks() || g_protectCalls != beforeRetry) {
        printf("  FAIL: installation retried after incomplete rollback\n");
        ++failed;
    }
    *(void**)(pe + c.timeSlotRva) = g_dummyTimePtr;
    DestroyFakePe(pe);
    return failed ? 1 : 0;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    printf("=== Starting LocalTimeTzClock Regression Test Suite ===\n");
    int failed = 0;
    failed += Test_ImportProbing_MissingOFT_And_EmptyImports();
    failed += Test_CallSites_ExceedLimit_Rejected();
    failed += Test_VerifiedLayout_Vs_UnknownLayout();
    failed += Test_CandidateFallback_FirstFails_SecondSucceeds();
    failed += Test_InstallFailure_Rollback_And_PublishOrder();
    failed += Test_UnauthorizedCallSites_And_SystemTimePassthrough();
    failed += Test_RejectForeignHooks_And_BoundedNames();
    failed += Test_FormattingPassthrough_And_AddressRange();
    failed += Test_LocalSystemImage();
    failed += Test_SystemTrayRealImage_ReadOnlyProbe();
    failed += Test_SystemTray_Acceptance_And_Rejection();
    failed += Test_SystemTray_Activation_And_SlotStrategy();
    failed += Test_SystemTray_HookGetLocalTime_Behavior_And_Stub();
    failed += Test_SystemTray_AdditionalClock_Formatting_Passthrough();
    failed += Test_RealWriteFailures(); // 最后一个用例故意模拟不可恢复状态。

    printf("\n=== Regression Test Results: %s (Total Failed: %d) ===\n",
           failed == 0 ? "ALL PASSED" : "FAILED", failed);
    return failed == 0 ? 0 : 1;
}
