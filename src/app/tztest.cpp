// Verification harness: compares SystemTimeToTzSpecificLocalTimeEx (the same
// call the DLL uses) against .NET-equivalent expectations across tricky zones
// and DST boundaries. Prints results for the record.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static void Show(const char* label, const wchar_t* tz, const SYSTEMTIME* utc) {
    DYNAMIC_TIME_ZONE_INFORMATION d = {};
    DWORD i = 0;
    BOOL found = FALSE;
    for (;; ++i) {
        DYNAMIC_TIME_ZONE_INFORMATION t;
        if (EnumDynamicTimeZoneInformation(i, &t) != ERROR_SUCCESS) break;
        if (_wcsicmp(t.TimeZoneKeyName, tz) == 0) { d = t; found = TRUE; break; }
    }
    if (!found) { printf("%-28s %-26ls NOT FOUND\n", label, tz); return; }
    SYSTEMTIME l = {};
    SystemTimeToTzSpecificLocalTimeEx(&d, utc, &l);
    printf("%-28s %-26ls %04d-%02d-%02d %02d:%02d (UTC %04d-%02d-%02d %02d:%02d)\n",
           label, tz, l.wYear, l.wMonth, l.wDay, l.wHour, l.wMinute,
           utc->wYear, utc->wMonth, utc->wDay, utc->wHour, utc->wMinute);
}

int main() {
    SYSTEMTIME utc = {};
    // 2026-07-01 12:00 UTC  (northern summer)
    utc.wYear = 2026; utc.wMonth = 7; utc.wDayOfWeek = 0; utc.wDay = 1;
    utc.wHour = 12; utc.wMinute = 0; utc.wSecond = 0; utc.wMilliseconds = 0;
    printf("== northern summer ==\n");
    Show("DST region", L"Pacific Standard Time", &utc);
    Show("China", L"China Standard Time", &utc);
    Show("India (half hour)", L"India Standard Time", &utc);
    Show("Nepal (45 min)", L"Nepal Standard Time", &utc);
    Show("Chatham (45min+DIT)", L"Chatham Islands Standard Time", &utc);
    Show("Lord Howe (30m DST)", L"Lord Howe Standard Time", &utc);

    // 2026-01-15 12:00 UTC (northern winter / southern summer)
    memset(&utc, 0, sizeof(utc));
    utc.wYear = 2026; utc.wMonth = 1; utc.wDay = 15; utc.wHour = 12;
    printf("== northern winter ==\n");
    Show("DST region (winter)", L"Pacific Standard Time", &utc);
    Show("China", L"China Standard Time", &utc);
    Show("Lord Howe (DST)", L"Lord Howe Standard Time", &utc);

    // DST boundary probes for Pacific: 2026-03-08 09:59/10:01 UTC
    SYSTEMTIME b1 = {}, b2 = {};
    b1.wYear = 2026; b1.wMonth = 3; b1.wDay = 8; b1.wHour = 9; b1.wMinute = 59;
    b2.wYear = 2026; b2.wMonth = 3; b2.wDay = 8; b2.wHour = 10; b2.wMinute = 1;
    printf("== DST start boundary (US) ==\n");
    Show("just before", L"Pacific Standard Time", &b1);
    Show("just after", L"Pacific Standard Time", &b2);

    // DST end: 2026-11-01 08:59 / 09:01 UTC
    SYSTEMTIME c1 = {}, c2 = {};
    c1.wYear = 2026; c1.wMonth = 11; c1.wDay = 1; c1.wHour = 8; c1.wMinute = 59;
    c2.wYear = 2026; c2.wMonth = 11; c2.wDay = 1; c2.wHour = 9; c2.wMinute = 1;
    printf("== DST end boundary (US) ==\n");
    Show("just before", L"Pacific Standard Time", &c1);
    Show("just after", L"Pacific Standard Time", &c2);

    // Date rollover: 2026-11-01 20:00 UTC -> China next day 04:00
    SYSTEMTIME r = {};
    r.wYear = 2026; r.wMonth = 11; r.wDay = 1; r.wHour = 20;
    printf("== date rollover ==\n");
    Show("rollover", L"China Standard Time", &r);
    return 0;
}
