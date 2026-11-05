// LocalTimeTzClock：只修改原生任务栏时钟显示，不改变系统时区。
//
// 安全边界：
// 1. 按 SystemTray.dll、Taskbar.View.dll 的顺序只读探测候选模块。
// 2. 写入前核对已验证的 PE 身份、导入槽与完整调用点集合。
// 3. 未知布局、无效导入、重复目标导入及超限扫描均拒绝启用。
// 4. 原函数和白名单先就绪，再发布 IAT 挂钩；写入失败尝试回滚。
//    回滚或页保护恢复失败时保持透传，永久拒绝本次加载中的重试。
// 5. 旧版 (Taskbar.View.dll) 采用格式化 API 挂钩 + 调度 kick 加速。
//    新版 (SystemTray.dll 2609.11000.100.0) 仅挂钩 GetLocalTime 主时钟数据源与调度点，
//    保持格式化 IAT 不变以保护附加时钟独立时区换算与显示。
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#include <shlobj.h>
#include <strsafe.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <intrin.h>

#if defined(__GNUC__)
#undef _ReturnAddress
#define _ReturnAddress() __builtin_return_address(0)
#endif

#define TZCLOCK_VERSION 17

// 候选宿主优先级
static const WCHAR* kTargetModules[] = { L"SystemTray.dll", L"Taskbar.View.dll", L"explorer.exe" };
#define TARGET_MODULE_COUNT (int)(sizeof(kTargetModules) / sizeof(kTargetModules[0]))

#ifdef TZCLOCK_TEST_RUNNER
typedef HMODULE (WINAPI* PFN_GetModuleHandleHook)(LPCWSTR);
static PFN_GetModuleHandleHook g_testModuleResolver = nullptr;
#define RESOLVE_MODULE(name) (g_testModuleResolver ? g_testModuleResolver(name) : GetModuleHandleW(name))
#else
#define RESOLVE_MODULE(name) GetModuleHandleW(name)
#endif

typedef int (WINAPI* PFN_GetTimeFormatEx)(LPCWSTR, DWORD, const SYSTEMTIME*,
                                          LPCWSTR, LPWSTR, int);
typedef int (WINAPI* PFN_GetDateFormatEx)(LPCWSTR, DWORD, const SYSTEMTIME*,
                                          LPCWSTR, LPWSTR, int, LPCWSTR);
typedef VOID (WINAPI* PFN_GetLocalTime)(LPSYSTEMTIME);

#ifndef DEVICE_NOTIFY_CALLBACK
#define DEVICE_NOTIFY_CALLBACK 2
typedef ULONG (CALLBACK *PDEVICE_NOTIFY_CALLBACK_ROUTINE)(PVOID Context, ULONG Type, PVOID Setting);
typedef struct _DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS {
    PDEVICE_NOTIFY_CALLBACK_ROUTINE Callback;
    PVOID Context;
} DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS, *PDEVICE_NOTIFY_SUBSCRIBE_PARAMETERS;
#endif

// ------------------------------------------------------------- 策略与布局定义 ---
enum HookStrategy {
    STRATEGY_FORMAT_REWRITE = 0, // 旧版：格式化 API 重写 + 调度 kick 加速
    STRATEGY_LOCAL_SOURCE   = 1  // 新版：仅挂钩 GetLocalTime 主时钟数据源与调度点，格式化 IAT 完全不修改
};

enum ReturnSiteType {
    SITE_UNKNOWN = 0,
    SITE_FORMAT_TIME,
    SITE_FORMAT_DATE,
    SITE_LOCAL_SOURCE,
    SITE_LOCAL_SCHEDULE
};

#define MAX_CALL_SITES 16

struct VerifiedLayout {
    const WCHAR* moduleName;
    DWORD timeDateStamp;
    DWORD sizeOfImage;
    WORD  machine;
    HookStrategy strategy;
    DWORD iatTimeRva;
    DWORD iatDateRva;
    DWORD iatLocalRva;
    DWORD timeSites[MAX_CALL_SITES];
    int   timeCount;
    DWORD dateSites[MAX_CALL_SITES];
    int   dateCount;
    DWORD localSourceSites[MAX_CALL_SITES];
    int   localSourceCount;
    DWORD localScheduleSites[MAX_CALL_SITES];
    int   localScheduleCount;
    DWORD localPassthroughSites[MAX_CALL_SITES];
    int   localPassthroughCount;
};

// 已验证布局列表
static const VerifiedLayout kVerifiedLayouts[] = {
    // 候选 1：Windows 11 Build 26300.9807/9818 (SystemTray.dll 2609.11000.100.0 x64)
    {
        L"SystemTray.dll",
        0x6AA9E607,
        0x202000,
        IMAGE_FILE_MACHINE_AMD64,
        STRATEGY_LOCAL_SOURCE,
        0x18B288, // iatTimeRva
        0x18B278, // iatDateRva
        0x18B270, // iatLocalRva
        { 0xC8150, 0xC85F3, 0xC8A7B, 0xC8A90, 0x129C9B, 0x129F4F, 0x129F64 }, 7,
        { 0xCC3F9, 0xCC5BE, 0xCC5F7, 0xCC9C6, 0x12CAFE, 0x12CCB9, 0x12CCEF }, 7,
        { 0xCA3BD, 0x12B73D }, 2, // 2 个 RefreshIcon 主时钟数据源点
        { 0xCAA7D, 0x12BCED }, 2, // 2 个 ScheduleNextUpdate 调度点
        { 0 }, 0
    },
    // 候选 2：Windows 11 Taskbar.View.dll 623.27903.70.0 (x64)
    {
        L"Taskbar.View.dll",
        0x654E6445,
        0x758000,
        IMAGE_FILE_MACHINE_AMD64,
        STRATEGY_FORMAT_REWRITE,
        0x617578,
        0x617580,
        0x617880,
        { 0x359FF }, 1,
        { 0x34DF9, 0x34E4F }, 2,
        { 0 }, 0,
        { 0x34C0C }, 1,
        { 0x34CB7, 0x34D94 }, 2
    },
    // Windows 10 22H2 build 19045.2728: explorer.exe 10.0.19041.2311 x64.
    // ClockButton::UpdateTextStringsIfNecessary samples the time once for
    // its time, date and weekday strings. Keep the tooltip's samples native.
    {
        L"explorer.exe",
        0x4990A333,
        0x4FE000,
        IMAGE_FILE_MACHINE_AMD64,
        STRATEGY_LOCAL_SOURCE,
        0x2F8410,
        0x2F8418,
        0x2F8C80,
        { 0x6EACC, 0x6EC24, 0x15601E }, 3,
        { 0x6EB1F, 0x6EB58, 0x6F5DD }, 3,
        { 0x6EA6A }, 1,
        { 0 }, 0,
        { 0x4CCB4, 0x595B6, 0x9FDD8, 0x186102, 0x18D76C, 0x18D8F8 }, 6
    }
};
#define VERIFIED_LAYOUT_COUNT (int)(sizeof(kVerifiedLayouts) / sizeof(kVerifiedLayouts[0]))

// ------------------------------------------------------------------ config --
static HMODULE g_selfModule = nullptr;

struct Config {
    WCHAR timeZoneId[128];
    BOOL  enabled;
    BOOL  log;
};

static Config g_cfg = { L"China Standard Time", TRUE, FALSE };

static void GetOwnDir(WCHAR* out, DWORD cch) {
    GetModuleFileNameW(g_selfModule, out, cch);
    WCHAR* p = wcsrchr(out, L'\\');
    if (p) *p = 0;
}

static WCHAR g_iniPath[MAX_PATH];

static WCHAR* Trim(WCHAR* s) {
    while (*s == L' ' || *s == L'\t') ++s;
    WCHAR* e = s + wcslen(s);
    while (e > s && (e[-1] == L' ' || e[-1] == L'\t' || e[-1] == L'\r' ||
                     e[-1] == L'\n'))
        --e;
    *e = 0;
    return s;
}

static void LoadConfig() {
    GetOwnDir(g_iniPath, MAX_PATH);
    StringCchCatW(g_iniPath, MAX_PATH, L"\\tzclock.ini");

    HANDLE h = CreateFileW(g_iniPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 65536) {
        CloseHandle(h);
        return;
    }
    DWORD n = (DWORD)sz.QuadPart;
    char* raw = (char*)malloc(n + 1);
    if (!raw) { CloseHandle(h); return; }
    DWORD read = 0;
    BOOL ok = ReadFile(h, raw, n, &read, nullptr);
    CloseHandle(h);
    if (!ok) { free(raw); return; }
    raw[read] = 0;

    char* cur = raw;
    if ((unsigned char)cur[0] == 0xEF && (unsigned char)cur[1] == 0xBB &&
        (unsigned char)cur[2] == 0xBF)
        cur += 3;

    while (*cur) {
        char* eol = cur;
        while (*eol && *eol != '\n') ++eol;
        if (*eol == '\n') { *eol = 0; ++eol; }
        char* next = eol;

        char* line = cur;
        cur = next;

        for (char* c = line; *c; ++c)
            if (*c == ';' || *c == '#') { *c = 0; break; }

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;

        WCHAR key[128] = { 0 };
        WCHAR val[512] = { 0 };
        int kn = MultiByteToWideChar(CP_UTF8, 0, line, -1, key, ARRAYSIZE(key));
        int vn = MultiByteToWideChar(CP_UTF8, 0, eq + 1, -1, val, ARRAYSIZE(val));
        if (kn <= 0 || vn <= 0) continue;
        key[ARRAYSIZE(key) - 1] = 0;
        val[ARRAYSIZE(val) - 1] = 0;

        WCHAR* k = Trim(key);
        WCHAR* v = Trim(val);
        if (_wcsicmp(k, L"TimeZoneId") == 0) {
            if (*v) StringCchCopyW(g_cfg.timeZoneId, ARRAYSIZE(g_cfg.timeZoneId), v);
        } else if (_wcsicmp(k, L"Enabled") == 0) {
            g_cfg.enabled = (_wtoi(v) != 0);
        } else if (_wcsicmp(k, L"Log") == 0) {
            g_cfg.log = (_wtoi(v) != 0);
        }
    }
    free(raw);
}

// ---------------------------------------------------------------- logging ---
static WCHAR g_logPath[MAX_PATH];
static CRITICAL_SECTION g_logCs;
static BOOL g_logReady = FALSE;

static void LogF(const WCHAR* fmt, ...) {
    if (!g_cfg.log || !g_logReady || !g_logPath[0]) return;
    WCHAR line[2048];
    va_list ap;
    va_start(ap, fmt);
    StringCchVPrintfW(line, ARRAYSIZE(line), fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_logCs);
    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        SetFilePointer(h, 0, nullptr, FILE_END);
        WriteFile(h, line, (DWORD)(wcslen(line) * sizeof(WCHAR)), &wrote, nullptr);
        WriteFile(h, L"\r\n", 4, &wrote, nullptr);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_logCs);
}

// ------------------------------------------------------------ target zone ---
struct SharedState {
    volatile LONG ready;       // DLL finished init
    volatile LONG supported;   // hooks installed successfully
    volatile LONG active;      // currently rewriting the clock
    volatile LONG hits;        // 时钟调用的重写次数，包含格式化或主数据源调用。
    volatile LONG lastError;   // Win32 error for the controller to report
    DWORD dllVersion;
};

static SharedState* g_state = nullptr;

// 稳定的系统入口；失败回滚后仍可能有线程持有刚读出的 Hook 地址。
static PFN_GetTimeFormatEx g_origTimeFmt = GetTimeFormatEx;
static PFN_GetDateFormatEx g_origDateFmt = GetDateFormatEx;
static PFN_GetLocalTime   g_origLocalTime = GetLocalTime;

static BYTE* g_modBase = nullptr;
static const VerifiedLayout* g_currentLayout = nullptr;

static SRWLOCK g_zoneLock = SRWLOCK_INIT;
static DYNAMIC_TIME_ZONE_INFORMATION g_dtzi;
static BOOL g_dtziValid = FALSE;
static volatile LONG g_active = 0;
static volatile LONG g_hits = 0;
static volatile LONG g_kickPending = 0;

static BOOL LoadTargetZone() {
    DYNAMIC_TIME_ZONE_INFORMATION d;
    for (DWORD i = 0;; ++i) {
        if (EnumDynamicTimeZoneInformation(i, &d) != ERROR_SUCCESS) break;
        if (_wcsicmp(d.TimeZoneKeyName, g_cfg.timeZoneId) == 0) {
            AcquireSRWLockExclusive(&g_zoneLock);
            g_dtzi = d;
            g_dtziValid = TRUE;
            ReleaseSRWLockExclusive(&g_zoneLock);
            return TRUE;
        }
    }
    AcquireSRWLockExclusive(&g_zoneLock);
    g_dtziValid = FALSE;
    ReleaseSRWLockExclusive(&g_zoneLock);
    return FALSE;
}

static BOOL TargetNow(SYSTEMTIME* out) {
    DYNAMIC_TIME_ZONE_INFORMATION tz;
    BOOL valid = FALSE;
    AcquireSRWLockShared(&g_zoneLock);
    if (g_dtziValid) {
        tz = g_dtzi;
        valid = TRUE;
    }
    ReleaseSRWLockShared(&g_zoneLock);
    if (!valid) return FALSE;

    SYSTEMTIME utc;
    GetSystemTime(&utc);
    return SystemTimeToTzSpecificLocalTimeEx(&tz, &utc, out) != FALSE;
}

// 提取的生产 Helper：精确判定返回地址类型
static ReturnSiteType ClassifyReturnAddress(void* retAddr) {
    if (!g_modBase || !g_currentLayout) return SITE_UNKNOWN;
    ULONG_PTR address = (ULONG_PTR)retAddr, base = (ULONG_PTR)g_modBase;
    if (address < base || address - base >= 0x100000000ULL) return SITE_UNKNOWN;
    DWORD rva = (DWORD)(address - base);

    if (g_currentLayout->strategy == STRATEGY_FORMAT_REWRITE) {
        for (int i = 0; i < g_currentLayout->timeCount; ++i) {
            if (rva == g_currentLayout->timeSites[i]) return SITE_FORMAT_TIME;
        }
        for (int i = 0; i < g_currentLayout->dateCount; ++i) {
            if (rva == g_currentLayout->dateSites[i]) return SITE_FORMAT_DATE;
        }
        for (int i = 0; i < g_currentLayout->localScheduleCount; ++i) {
            if (rva == g_currentLayout->localScheduleSites[i]) return SITE_LOCAL_SCHEDULE;
        }
    } else if (g_currentLayout->strategy == STRATEGY_LOCAL_SOURCE) {
        for (int i = 0; i < g_currentLayout->localSourceCount; ++i) {
            if (rva == g_currentLayout->localSourceSites[i]) return SITE_LOCAL_SOURCE;
        }
        for (int i = 0; i < g_currentLayout->localScheduleCount; ++i) {
            if (rva == g_currentLayout->localScheduleSites[i]) return SITE_LOCAL_SCHEDULE;
        }
    }
    return SITE_UNKNOWN;
}

static BOOL IsClockCallSite(void* retAddr) {
    ReturnSiteType t = ClassifyReturnAddress(retAddr);
    return (t == SITE_FORMAT_TIME || t == SITE_FORMAT_DATE);
}

static BOOL IsClockLocalCallSite(void* retAddr) {
    ReturnSiteType t = ClassifyReturnAddress(retAddr);
    return (t == SITE_LOCAL_SCHEDULE);
}

// ------------------------------------------------------------------ hooks ---
static VOID WINAPI HookGetLocalTime(LPSYSTEMTIME st) {
    if (g_origLocalTime) g_origLocalTime(st);
    if (!st) return;

    ReturnSiteType site = ClassifyReturnAddress(_ReturnAddress());
    if (site == SITE_LOCAL_SOURCE) {
        // 新策略下的 RefreshIcon 主数据源返回点：active 时替换为目标时区时间，保持实际秒数与毫秒数，计 hits，不消费 kick
        if (g_active) {
            SYSTEMTIME target;
            if (TargetNow(&target)) {
                // 整体采用同一次取样，避免跨分钟/午夜时拼入旧秒数。
                *st = target;
                InterlockedIncrement(&g_hits);
                if (g_state) InterlockedIncrement(&g_state->hits);
            }
        }
    } else if (site == SITE_LOCAL_SCHEDULE) {
        // 调度点：只消费 kick，把秒改 59，不改时区，不计 hits
        if (InterlockedExchange(&g_kickPending, 0)) {
            st->wSecond = 59;
        }
    }
}

static int WINAPI HookGetTimeFormatEx(LPCWSTR locale, DWORD flags,
                                      const SYSTEMTIME* t, LPCWSTR fmt,
                                      LPWSTR buf, int cch) {
    if (g_active && ClassifyReturnAddress(_ReturnAddress()) == SITE_FORMAT_TIME) {
        SYSTEMTIME target;
        if (TargetNow(&target)) {
            InterlockedIncrement(&g_hits);
            if (g_state) InterlockedIncrement(&g_state->hits);
            return g_origTimeFmt(locale, flags, &target, fmt, buf, cch);
        }
    }
    return g_origTimeFmt(locale, flags, t, fmt, buf, cch);
}

static int WINAPI HookGetDateFormatEx(LPCWSTR locale, DWORD flags,
                                      const SYSTEMTIME* t, LPCWSTR fmt,
                                      LPWSTR buf, int cch, LPCWSTR cal) {
    if (g_active && ClassifyReturnAddress(_ReturnAddress()) == SITE_FORMAT_DATE) {
        SYSTEMTIME target;
        if (TargetNow(&target)) {
            InterlockedIncrement(&g_hits);
            if (g_state) InterlockedIncrement(&g_state->hits);
            return g_origDateFmt(locale, flags, &target, fmt, buf, cch, cal);
        }
    }
    return g_origDateFmt(locale, flags, t, fmt, buf, cch, cal);
}

// ------------------------------------------------------------ IAT 探测与写入 --
struct IatEntry { void** slot; void* original; };

static IatEntry g_iatTime = { nullptr, nullptr };
static IatEntry g_iatDate = { nullptr, nullptr };
static IatEntry g_iatLocal = { nullptr, nullptr };

// 映像来自已加载模块；所有 RVA、字符串及节表仍需在映像内完整有效。
static BOOL ImageRange(DWORD size, SIZE_T rva, SIZE_T length) {
    return rva <= size && length <= (SIZE_T)size - rva;
}

static IMAGE_NT_HEADERS* ImageHeaders(BYTE* base) {
    if (!base) return nullptr;
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000)
        return nullptr;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
        return nullptr;
    DWORD size = nt->OptionalHeader.SizeOfImage;
    if (!ImageRange(size, dos->e_lfanew, sizeof(*nt)) ||
        !ImageRange(size, (BYTE*)IMAGE_FIRST_SECTION(nt) - base,
                    sizeof(IMAGE_SECTION_HEADER) * (SIZE_T)nt->FileHeader.NumberOfSections))
        return nullptr;
    return nt;
}

static const char* ImageString(BYTE* base, DWORD size, SIZE_T rva) {
    if (!ImageRange(size, rva, 1)) return nullptr;
    const char* value = (const char*)(base + rva);
    return memchr(value, 0, size - rva) ? value : nullptr;
}

// 只读查找：唯一匹配返回 1，缺失或无效返回 0，重复匹配返回 -2。
static int FindImportSlot(BYTE* base, const char* moduleName, const char* funcName,
                          void*** outSlot, void** outOriginal) {
    if (!funcName || !outSlot || !outOriginal) return 0;
    *outSlot = nullptr; *outOriginal = nullptr;
    auto nt = ImageHeaders(base);
    if (!nt) return 0;
    DWORD size = nt->OptionalHeader.SizeOfImage;
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
        !ImageRange(size, dir.VirtualAddress, dir.Size)) return 0;
    void** foundSlot = nullptr; void* foundOrig = nullptr;
    for (SIZE_T off = 0; off + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= dir.Size;
         off += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress + off);
        if (!imp->Name) {
            if (!foundSlot || !foundOrig) return 0;
            *outSlot = foundSlot; *outOriginal = foundOrig;
            return 1;
        }
        const char* dll = ImageString(base, size, imp->Name);
        if (!dll) return 0;
        if (moduleName && _stricmp(dll, moduleName) != 0) continue;
        if (!imp->OriginalFirstThunk || !imp->FirstThunk) return 0;
        for (SIZE_T index = 0;; ++index) {
            SIZE_T oftRva = (SIZE_T)imp->OriginalFirstThunk + index * sizeof(IMAGE_THUNK_DATA);
            SIZE_T ftRva = (SIZE_T)imp->FirstThunk + index * sizeof(IMAGE_THUNK_DATA);
            if (!ImageRange(size, oftRva, sizeof(IMAGE_THUNK_DATA)) ||
                !ImageRange(size, ftRva, sizeof(IMAGE_THUNK_DATA))) return 0;
            auto oft = (IMAGE_THUNK_DATA*)(base + oftRva);
            if (!oft->u1.AddressOfData) break;
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            SIZE_T nameRva = (SIZE_T)oft->u1.AddressOfData;
            if (!ImageRange(size, nameRva, sizeof(WORD) + 1)) return 0;
            const char* name = ImageString(base, size, nameRva + sizeof(WORD));
            if (!name) return 0;
            if (strcmp(name, funcName) != 0) continue;
            if (foundSlot) return -2;
            foundSlot = (void**)(base + ftRva);
            foundOrig = *foundSlot;
        }
    }
    return 0; // 导入描述符缺少终止项。
}

#define MAX_SCAN_SITES 16
struct ScanResult {
    DWORD timeSites[MAX_SCAN_SITES];
    int   timeCount;
    DWORD dateSites[MAX_SCAN_SITES];
    int   dateCount;
    DWORD localSites[MAX_SCAN_SITES];
    int   localCount;
};

static void ScanCallSitesSinglePass(BYTE* base, DWORD timeSlotRva, DWORD dateSlotRva, DWORD localSlotRva,
                                    ScanResult* res) {
    memset(res, 0, sizeof(ScanResult));
    if (!base) return;

    auto nt = ImageHeaders(base);
    if (!nt) return;
    auto sec = IMAGE_FIRST_SECTION(nt);

    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (memcmp(sec->Name, ".text", 5) != 0) continue;
        DWORD size = sec->Misc.VirtualSize;
        if (!ImageRange(nt->OptionalHeader.SizeOfImage, sec->VirtualAddress, size)) {
            res->timeCount = res->dateCount = res->localCount = -1;
            return;
        }
        BYTE* p = base + sec->VirtualAddress;
        for (DWORD i = 0; i + 6 <= size; ++i) {
            if (p[i] != 0xFF || p[i + 1] != 0x15) continue;
            INT32 disp = *(INT32*)(p + i + 2);
            DWORD targetRva = sec->VirtualAddress + i + 6 + disp;
            DWORD siteRva = sec->VirtualAddress + i + 6;

            if (timeSlotRva && targetRva == timeSlotRva) {
                if (res->timeCount < MAX_SCAN_SITES) res->timeSites[res->timeCount] = siteRva;
                res->timeCount++;
            } else if (dateSlotRva && targetRva == dateSlotRva) {
                if (res->dateCount < MAX_SCAN_SITES) res->dateSites[res->dateCount] = siteRva;
                res->dateCount++;
            } else if (localSlotRva && targetRva == localSlotRva) {
                if (res->localCount < MAX_SCAN_SITES) res->localSites[res->localCount] = siteRva;
                res->localCount++;
            }
        }
    }
}

struct ProbedModuleInfo {
    const VerifiedLayout* layout;
    void** timeSlot;
    void*  timeOrig;
    void** dateSlot;
    void*  dateOrig;
    void** localSlot;
    void*  localOrig;
};

static BOOL ProbeCandidateModule(BYTE* base, const WCHAR* modName, ProbedModuleInfo* outInfo) {
    if (!base || !outInfo) return FALSE;
    memset(outInfo, 0, sizeof(ProbedModuleInfo));

    auto nt = ImageHeaders(base);
    if (!nt) return FALSE;

    DWORD timeDateStamp = nt->FileHeader.TimeDateStamp;
    DWORD sizeOfImage = nt->OptionalHeader.SizeOfImage;
    WORD machine = nt->FileHeader.Machine;

    const VerifiedLayout* match = nullptr;
    for (int i = 0; i < VERIFIED_LAYOUT_COUNT; ++i) {
        const VerifiedLayout* vl = &kVerifiedLayouts[i];
        if (modName && _wcsicmp(modName, vl->moduleName) != 0) continue;
        if (vl->timeDateStamp == timeDateStamp &&
            vl->sizeOfImage == sizeOfImage &&
            vl->machine == machine) {
            match = vl;
            break;
        }
    }
    if (!match) {
        LogF(L"module '%s' rejected: unknown layout (ts=0x%08X, size=0x%08X, machine=0x%04X)",
             modName ? modName : L"unknown", timeDateStamp, sizeOfImage, machine);
        return FALSE;
    }

    void** slotTime = nullptr; void* origTime = nullptr;
    void** slotDate = nullptr; void* origDate = nullptr;
    void** slotLocal = nullptr; void* origLocal = nullptr;

    if (FindImportSlot(base, nullptr, "GetTimeFormatEx", &slotTime, &origTime) != 1) return FALSE;
    if (FindImportSlot(base, nullptr, "GetDateFormatEx", &slotDate, &origDate) != 1) return FALSE;
    if (FindImportSlot(base, nullptr, "GetLocalTime", &slotLocal, &origLocal) != 1) return FALSE;

    DWORD rvaTime = (DWORD)((BYTE*)slotTime - base);
    DWORD rvaDate = (DWORD)((BYTE*)slotDate - base);
    DWORD rvaLocal = (DWORD)((BYTE*)slotLocal - base);

    if (rvaTime != match->iatTimeRva || rvaDate != match->iatDateRva || rvaLocal != match->iatLocalRva) {
        LogF(L"module '%s' rejected: IAT RVA mismatch", modName ? modName : L"unknown");
        return FALSE;
    }

    // 不链接其他版本或第三方挂钩，否则关闭本版本后仍可能被下层重写。
    void* nativeTime = (void*)GetTimeFormatEx;
    void* nativeDate = (void*)GetDateFormatEx;
    void* nativeLocal = (void*)GetLocalTime;
    if (_wcsicmp(match->moduleName, L"explorer.exe") == 0) {
        // Win10 Explorer's API-set imports resolve to KernelBase implementations.
        HMODULE api = GetModuleHandleW(L"kernelbase.dll");
        nativeTime = (void*)GetProcAddress(api, "GetTimeFormatEx");
        nativeDate = (void*)GetProcAddress(api, "GetDateFormatEx");
        nativeLocal = (void*)GetProcAddress(api, "GetLocalTime");
    }
    if (origTime != nativeTime || origDate != nativeDate || origLocal != nativeLocal) return FALSE;

    ScanResult scan;
    ScanCallSitesSinglePass(base, rvaTime, rvaDate, rvaLocal, &scan);
    if (scan.timeCount < 0 || scan.dateCount < 0 || scan.localCount < 0 ||
        scan.timeCount > MAX_SCAN_SITES || scan.dateCount > MAX_SCAN_SITES ||
        scan.localCount > MAX_SCAN_SITES) return FALSE;

    // 严格核查全部 time 调用点集合
    if (scan.timeCount != match->timeCount) {
        LogF(L"module '%s' rejected: time callsite count mismatch (found %d, expected %d)",
             modName ? modName : L"unknown", scan.timeCount, match->timeCount);
        return FALSE;
    }
    for (int t = 0; t < match->timeCount; ++t) {
        bool found = false;
        for (int s = 0; s < scan.timeCount; ++s) {
            if (scan.timeSites[s] == match->timeSites[t]) { found = true; break; }
        }
        if (!found) return FALSE;
    }

    // 严格核查全部 date 调用点集合
    if (scan.dateCount != match->dateCount) {
        LogF(L"module '%s' rejected: date callsite count mismatch (found %d, expected %d)",
             modName ? modName : L"unknown", scan.dateCount, match->dateCount);
        return FALSE;
    }
    for (int d = 0; d < match->dateCount; ++d) {
        bool found = false;
        for (int s = 0; s < scan.dateCount; ++s) {
            if (scan.dateSites[s] == match->dateSites[d]) { found = true; break; }
        }
        if (!found) return FALSE;
    }

    // 严格核查全部 local 调用点集合
    int expectedLocalCount = match->localSourceCount + match->localScheduleCount + match->localPassthroughCount;
    if (scan.localCount != expectedLocalCount) {
        LogF(L"module '%s' rejected: local callsite count mismatch (found %d, expected %d)",
             modName ? modName : L"unknown", scan.localCount, expectedLocalCount);
        return FALSE;
    }
    for (int i = 0; i < match->localSourceCount; ++i) {
        bool found = false;
        for (int s = 0; s < scan.localCount; ++s) {
            if (scan.localSites[s] == match->localSourceSites[i]) { found = true; break; }
        }
        if (!found) return FALSE;
    }
    for (int i = 0; i < match->localScheduleCount; ++i) {
        bool found = false;
        for (int s = 0; s < scan.localCount; ++s) {
            if (scan.localSites[s] == match->localScheduleSites[i]) { found = true; break; }
        }
        if (!found) return FALSE;
    }
    for (int i = 0; i < match->localPassthroughCount; ++i) {
        bool found = false;
        for (int s = 0; s < scan.localCount; ++s) {
            if (scan.localSites[s] == match->localPassthroughSites[i]) { found = true; break; }
        }
        if (!found) return FALSE;
    }

    outInfo->layout = match;
    outInfo->timeSlot = slotTime;
    outInfo->timeOrig = origTime;
    outInfo->dateSlot = slotDate;
    outInfo->dateOrig = origDate;
    outInfo->localSlot = slotLocal;
    outInfo->localOrig = origLocal;
    return TRUE;
}

// 控制线程和回归测试共用同一条候选回退路径，探测不修改导入表。
static BOOL FindSupportedCandidate(HMODULE* module, const WCHAR** name, ProbedModuleInfo* info) {
    *module = nullptr; *name = nullptr;
    for (int m = 0; m < TARGET_MODULE_COUNT; ++m) {
        HMODULE candidate = RESOLVE_MODULE(kTargetModules[m]);
        if (candidate && ProbeCandidateModule((BYTE*)candidate, kTargetModules[m], info)) {
            *module = candidate; *name = kTargetModules[m];
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL g_writeProtectionError = FALSE;
static BOOL g_installPoisoned = FALSE;
static DWORD g_installError = ERROR_SUCCESS;

static BOOL WriteSlotChecked(void** slot, void* newVal, void* expected) {
    DWORD oldProt = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt)) return FALSE;
    BOOL changed = InterlockedCompareExchangePointer(slot, newVal, expected) == expected;
    DWORD ignored = 0;
    if (!VirtualProtect(slot, sizeof(void*), oldProt, &ignored)) g_writeProtectionError = TRUE;
    if (changed) FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    else SetLastError(ERROR_BUSY);
    return changed;
}

static void WriteSlotInternal(void** slot, void* value) {
    if (slot) WriteSlotChecked(slot, value, *slot);
}

static BOOL g_patched = FALSE;

static BOOL ActivateHooks() {
    if (g_installPoisoned) {
        g_installError = ERROR_WRITE_FAULT;
        return FALSE;
    }
    if (g_patched) {
        InterlockedExchange(&g_active, 1);
        InterlockedExchange(&g_kickPending, 1);
        return TRUE;
    }

    g_installError = ERROR_PROC_NOT_FOUND;
    if (!g_modBase) return FALSE;

    ProbedModuleInfo info;
    if (!ProbeCandidateModule(g_modBase, nullptr, &info)) {
        LogF(L"ActivateHooks: probe verification failed for base %p", g_modBase);
        return FALSE;
    }

    // 在发布任何槽之前准备原函数及白名单配置；失败时也不清空原函数。
    g_origTimeFmt = (PFN_GetTimeFormatEx)info.timeOrig;
    g_origDateFmt = (PFN_GetDateFormatEx)info.dateOrig;
    g_origLocalTime = (PFN_GetLocalTime)info.localOrig;
    g_currentLayout = info.layout;

    InterlockedExchange(&g_active, 0);
    InterlockedExchange(&g_kickPending, 0);
    g_writeProtectionError = FALSE;

    if (info.layout->strategy == STRATEGY_FORMAT_REWRITE) {
        // 旧策略：修改 time, date, local 三个槽
        BOOL okTime = WriteSlotChecked(info.timeSlot, (void*)HookGetTimeFormatEx, info.timeOrig);
        BOOL okDate = okTime && WriteSlotChecked(info.dateSlot, (void*)HookGetDateFormatEx, info.dateOrig);
        BOOL okLocal = okDate && WriteSlotChecked(info.localSlot, (void*)HookGetLocalTime, info.localOrig);

        if (!okLocal || g_writeProtectionError) {
            g_installError = GetLastError();
            if (!g_installError) g_installError = ERROR_WRITE_FAULT;
            LogF(L"ERROR: IAT slot update failed, rolling back");
            BOOL restored = TRUE;
            if (okLocal && !WriteSlotChecked(info.localSlot, info.localOrig, (void*)HookGetLocalTime)) restored = FALSE;
            if (okDate && !WriteSlotChecked(info.dateSlot, info.dateOrig, (void*)HookGetDateFormatEx)) restored = FALSE;
            if (okTime && !WriteSlotChecked(info.timeSlot, info.timeOrig, (void*)HookGetTimeFormatEx)) restored = FALSE;
            if (!restored || g_writeProtectionError) {
                g_installPoisoned = TRUE;
                g_installError = ERROR_WRITE_FAULT;
            }
            return FALSE;
        }

        g_iatTime.slot = info.timeSlot;
        g_iatTime.original = info.timeOrig;
        g_iatDate.slot = info.dateSlot;
        g_iatDate.original = info.dateOrig;
        g_iatLocal.slot = info.localSlot;
        g_iatLocal.original = info.localOrig;
    } else if (info.layout->strategy == STRATEGY_LOCAL_SOURCE) {
        // 新策略：绝不修改 time 和 date 槽，只修改 local 槽
        BOOL okLocal = WriteSlotChecked(info.localSlot, (void*)HookGetLocalTime, info.localOrig);
        if (!okLocal || g_writeProtectionError) {
            g_installError = GetLastError();
            if (!g_installError) g_installError = ERROR_WRITE_FAULT;
            LogF(L"ERROR: local IAT slot update failed, rolling back");
            BOOL restored = TRUE;
            if (okLocal && !WriteSlotChecked(info.localSlot, info.localOrig, (void*)HookGetLocalTime)) restored = FALSE;
            if (!restored || g_writeProtectionError) {
                g_installPoisoned = TRUE;
                g_installError = ERROR_WRITE_FAULT;
            }
            return FALSE;
        }

        g_iatTime.slot = nullptr;
        g_iatTime.original = nullptr;
        g_iatDate.slot = nullptr;
        g_iatDate.original = nullptr;
        g_iatLocal.slot = info.localSlot;
        g_iatLocal.original = info.localOrig;
    }

    g_patched = TRUE;
    MemoryBarrier();

    InterlockedExchange(&g_active, 1);
    InterlockedExchange(&g_kickPending, 1);
    g_installError = ERROR_SUCCESS;
    LogF(L"IAT patched and activated on %p (strategy %d): local=%p",
         g_modBase, (int)info.layout->strategy, g_iatLocal.slot);
    return TRUE;
}

static void DeactivateHooks() {
    InterlockedExchange(&g_active, 0);
    InterlockedExchange(&g_kickPending, 1);
}

// Force the clock to re-read and repaint now.
static void RequestClockRefresh() {
    DWORD_PTR res = 0;

    SendMessageTimeoutW(HWND_BROADCAST, WM_TIMECHANGE, 0, 0,
                        SMTO_ABORTIFHUNG | SMTO_NOTIMEOUTIFNOTHUNG, 300, &res);

    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray) {
        HWND notify = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
        HWND targets[4] = {
            tray,
            notify,
            notify ? FindWindowExW(notify, nullptr, L"TrayClockWClass", nullptr) : nullptr,
            FindWindowExW(tray, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr)
        };
        for (int i = 0; i < ARRAYSIZE(targets); ++i) {
            if (!targets[i]) continue;
            SendMessageTimeoutW(targets[i], WM_TIMECHANGE, 0, 0,
                                SMTO_ABORTIFHUNG | SMTO_NOTIMEOUTIFNOTHUNG, 300, &res);
            RedrawWindow(targets[i], nullptr, nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_FRAME);
        }
    }

    // Secondary monitors (multi-display taskbars)
    HWND secTray = nullptr;
    while ((secTray = FindWindowExW(nullptr, secTray, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
        HWND bridge = FindWindowExW(secTray, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr);
        HWND clock = FindWindowExW(secTray, nullptr, L"ClockButton", nullptr);
        SendMessageTimeoutW(secTray, WM_TIMECHANGE, 0, 0,
                            SMTO_ABORTIFHUNG | SMTO_NOTIMEOUTIFNOTHUNG, 300, &res);
        RedrawWindow(secTray, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_FRAME);
        if (clock) {
            SendMessageTimeoutW(clock, WM_TIMECHANGE, 0, 0,
                                SMTO_ABORTIFHUNG | SMTO_NOTIMEOUTIFNOTHUNG, 300, &res);
            RedrawWindow(clock, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_FRAME);
        }
        if (bridge) {
            SendMessageTimeoutW(bridge, WM_TIMECHANGE, 0, 0,
                                SMTO_ABORTIFHUNG | SMTO_NOTIMEOUTIFNOTHUNG, 300, &res);
            RedrawWindow(bridge, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_FRAME);
        }
    }
}

// ------------------------------------------------------------ power events --
static HPOWERNOTIFY g_powerNotify = nullptr;

static ULONG CALLBACK SuspendResumeCallback(PVOID, ULONG type, PVOID) {
    if (type == PBT_APMRESUMEAUTOMATIC || type == PBT_APMRESUMESUSPEND ||
        type == PBT_APMRESUMECRITICAL) {
        LogF(L"power resume event %lu -> refresh clock", type);
        InterlockedExchange(&g_kickPending, 1);
        RequestClockRefresh();
    }
    return ERROR_SUCCESS;
}

static void InitPowerNotification() {
    if (g_powerNotify) return;
    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS params = {};
    params.Callback = SuspendResumeCallback;
    params.Context = nullptr;
    g_powerNotify = RegisterSuspendResumeNotification((HANDLE)&params, DEVICE_NOTIFY_CALLBACK);
    LogF(L"registered suspend-resume callback: %p", g_powerNotify);
}

static void UninitPowerNotification() {
    if (g_powerNotify) {
        UnregisterSuspendResumeNotification(g_powerNotify);
        g_powerNotify = nullptr;
    }
}

// ------------------------------------------------------- control plumbing ---
static WCHAR g_evtEnable[96];
static WCHAR g_evtDisable[96];
static WCHAR g_evtQuery[96];
static WCHAR g_mapName[96];

static void BuildControlNames() {
    DWORD pid = GetCurrentProcessId();
    StringCchPrintfW(g_evtEnable, ARRAYSIZE(g_evtEnable),
                     L"Local\\LocalTimeTzClock.v%u.Enable.%lu", TZCLOCK_VERSION, pid);
    StringCchPrintfW(g_evtDisable, ARRAYSIZE(g_evtDisable),
                     L"Local\\LocalTimeTzClock.v%u.Disable.%lu", TZCLOCK_VERSION, pid);
    StringCchPrintfW(g_evtQuery, ARRAYSIZE(g_evtQuery),
                     L"Local\\LocalTimeTzClock.v%u.Query.%lu", TZCLOCK_VERSION, pid);
    StringCchPrintfW(g_mapName, ARRAYSIZE(g_mapName),
                     L"Local\\LocalTimeTzClock.v%u.State.%lu", TZCLOCK_VERSION, pid);
}

static HANDLE g_map = nullptr;

static DWORD WINAPI ControlThread(LPVOID) {
    HANDLE enable  = CreateEventW(nullptr, FALSE, FALSE, g_evtEnable);
    HANDLE disable = CreateEventW(nullptr, FALSE, FALSE, g_evtDisable);
    HANDLE query   = CreateEventW(nullptr, FALSE, FALSE, g_evtQuery);

    LoadConfig();
    if (g_cfg.log) {
        WCHAR dir[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, dir)))
            StringCchPrintfW(g_logPath, MAX_PATH, L"%s\\tzclock.log", dir);
    }
    InitializeCriticalSection(&g_logCs);
    g_logReady = TRUE;
    LogF(L"--- attach (pid %lu, v%d) ---", GetCurrentProcessId(), TZCLOCK_VERSION);
    LogF(L"config: TimeZoneId='%s' Enabled=%d Log=%d", g_cfg.timeZoneId,
         g_cfg.enabled, g_cfg.log);

    // Explorer 异步加载任务栏，按优先级检查所有候选。
    HMODULE mod = nullptr;
    const WCHAR* modName = nullptr;
    ProbedModuleInfo probedInfo;

    for (int i = 0; i < 300; ++i) {           // up to ~60 s
        if (FindSupportedCandidate(&mod, &modName, &probedInfo)) break;
        Sleep(200);
    }

    if (!mod) {
        LogF(L"ERROR: no supported taskbar clock module appeared; staying inert");
        if (g_state) {
            bool anyCandidateLoaded = false;
            for (int m = 0; m < TARGET_MODULE_COUNT; ++m) {
                if (RESOLVE_MODULE(kTargetModules[m])) { anyCandidateLoaded = true; break; }
            }
            g_state->lastError = anyCandidateLoaded ? ERROR_PROC_NOT_FOUND : ERROR_MOD_NOT_FOUND;
            g_state->supported = 0;
            g_state->active = 0;
        }
        return 0;
    }
    g_modBase = (BYTE*)mod;
    LogF(L"supported host module '%s' selected, base=%p", modName, mod);

    if (!LoadTargetZone()) {
        LogF(L"ERROR: time zone '%s' not found; staying inert", g_cfg.timeZoneId);
        if (g_state) {
            g_state->lastError = ERROR_INVALID_PARAMETER;
            g_state->supported = 0;
            g_state->active = 0;
        }
        return 0;
    }
    LogF(L"target zone resolved: %s", g_dtzi.TimeZoneKeyName);

    BOOL ok = (g_cfg.enabled && ActivateHooks());
    if (g_state) {
        g_state->supported = g_patched ? 1 : 0;
        g_state->active = (g_cfg.enabled && ok) ? 1 : 0;
        g_state->lastError = (g_cfg.enabled && !ok) ? g_installError : 0;
    }

    InitPowerNotification();
    LogF(L"initial state: enabled=%d patched=%d", g_cfg.enabled, g_patched);
    RequestClockRefresh();

    for (;;) {
        HANDLE waits[3] = { enable, disable, query };
        DWORD r = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0) {
            LoadConfig();
            if (!LoadTargetZone()) {
                LogF(L"enable requested but zone '%s' invalid", g_cfg.timeZoneId);
                DeactivateHooks();
                if (g_state) {
                    g_state->active = 0;
                    g_state->lastError = ERROR_INVALID_PARAMETER;
                }
                RequestClockRefresh();
                continue;
            }
            BOOL a = ActivateHooks();
            LogF(L"enable requested -> active=%d", a);
            if (g_state) {
                g_state->supported = g_patched ? 1 : 0;
                g_state->active = a ? 1 : 0;
                if (!a) {
                    g_state->lastError = g_installError;
                } else {
                    g_state->lastError = 0;
                }
            }
            RequestClockRefresh();
        } else if (r == WAIT_OBJECT_0 + 1) {
            DeactivateHooks();
            LogF(L"disable requested -> hooks pass through");
            if (g_state) g_state->active = 0;
            RequestClockRefresh();
        } else if (r == WAIT_OBJECT_0 + 2) {
            if (g_state) {
                g_state->active = g_active;
                g_state->hits = g_hits;
                g_state->supported = g_patched ? 1 : 0;
            }
        } else {
            break;
        }
    }
    return 0;
}

#ifndef TZCLOCK_TEST_RUNNER
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_selfModule = inst;
        DisableThreadLibraryCalls(inst);
        BuildControlNames();

        g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                   PAGE_READWRITE, 0, sizeof(SharedState), g_mapName);
        if (g_map)
            g_state = (SharedState*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0,
                                                  sizeof(SharedState));
        if (g_state) {
            g_state->ready = 1;
            g_state->dllVersion = TZCLOCK_VERSION;
            g_state->hits = 0;
        }

        HANDLE h = CreateThread(nullptr, 0, ControlThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (reserved == nullptr) {
            UninitPowerNotification();
            if (g_patched && g_iatTime.slot) WriteSlotInternal(g_iatTime.slot, g_iatTime.original);
            if (g_patched && g_iatDate.slot) WriteSlotInternal(g_iatDate.slot, g_iatDate.original);
            if (g_patched && g_iatLocal.slot) WriteSlotInternal(g_iatLocal.slot, g_iatLocal.original);
            LogF(L"--- detach ---");
        }
    }
    return TRUE;
}
#endif
