# 目标系统与原生时钟实现路径（实测，2026-xx）

## 环境
- Windows 11 Pro 23H2, 10.0.22631.2861, x64
- 系统时区: Pacific Standard Time (Mexico) (UTC-08:00, 有 DST)
- 任务栏实现: XAML 版 (Taskbar.View.dll), 未安装 Windhawk/EP 等
- Taskbar.View.dll: C:\Windows\SystemApps\MicrosoftWindows.Client.Core_cw5n1h2txyewy\Taskbar.View.dll
  版本 623.27903.70.0, machine 0x8664, PDB GUID A7A2092D-CFD84270, age 1 -> key 2D09A2A7D8CF70429518DC6321931E791
  PDB 不在 msdl.microsoft.com (404)，故不使用符号、改用静态导入点定位。

## 导入表
Taskbar.View.dll 导入 GetLocalTime / GetSystemTime / GetTimeFormatEx / GetDateFormatEx
(via api-ms-win-core-sysinfo-l1-1-0.dll 和 api-ms-win-core-datetime-l1-1-1.dll)

## 全模块扫描：引用这些 API 的函数只有两个
- RVA 0x34BA4 (size 0x3367) : GetLocalTime(0x34c06) + GetSystemTime(0x34d9f)
                              + GetDateFormatEx(0x34df3, flags 0x42 = DATE_LONGDATE|AUTOLAYOUT)
                              + GetDateFormatEx(0x34e49, flags 0x41 = DATE_SHORTDATE|AUTOLAYOUT)
- RVA 0x35960 (size 0x1A52, helper called by 0x34BA4) : GetTimeFormatEx(0x359f9, flags 2 = TIME_NOSECONDS)

## 结论
任务栏时钟文本由 RVA 0x34BA4 内的两处 GetDateFormatEx 和 0x359F9 的 GetTimeFormatEx 生成。
方案：只改 Taskbar.View.dll 的 IAT 槽（不 hook 全局 kernelbase 导出），
并在 hook 内用返回地址白名单限定调用点。
