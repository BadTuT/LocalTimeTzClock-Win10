# 目标系统与原生任务栏时钟的实现路径

全部结论来自本机实测，不是推断。

## 实测环境

| 项目 | 值 |
|---|---|
| Windows | Windows 11 Pro 23H2 |
| Build | 10.0.22631.2861 |
| 架构 | x64 |
| 系统时区 | Pacific Standard Time (Mexico)，UTC-08:00，有 DST |
| 任务栏实现 | XAML 任务栏（非 Win10 经典任务栏） |
| 第三方任务栏修改 | 无（未安装 Windhawk / ExplorerPatcher / Rainmeter / StartAllBack） |
| 资源管理器进程 | 单一 explorer.exe，持有 Shell_TrayWnd |

时钟所在的模块：

```
C:\Windows\SystemApps\MicrosoftWindows.Client.Core_cw5n1h2txyewy\Taskbar.View.dll
版本 623.27903.70.0，machine 0x8664
```

本机不存在 `C:\Windows\System32\SystemTray.dll`，因此时钟代码位于 `Taskbar.View.dll`，
而不是较新版本中的 `SystemTray.dll`。

PDB 不在 `msdl.microsoft.com`（请求返回 404），所以没有使用符号名，
也没有把 Windhawk 的函数签名当作前提。

## 原生时钟的显示调用路径

`Taskbar.View.dll` 通过 apiset 导入这些时间 API：

```
api-ms-win-core-sysinfo-l1-1-0.dll     GetLocalTime @ IAT RVA 0x617880
                                       GetSystemTime @ IAT RVA 0x617878
api-ms-win-core-datetime-l1-1-1.dll    GetTimeFormatEx @ IAT RVA 0x617578
                                       GetDateFormatEx @ IAT RVA 0x617580
```

对整个模块做全量指令扫描（`.text` 逐字节查找 `modrm & 0xC7 == 0x05` 的 RIP 相对寻址）
后，**引用这四个导入槽的函数只有两个**：

| 函数 RVA | 大小 | 引用内容 |
|---|---|---|
| `0x34BA4` | 0x3367 | `GetLocalTime` @0x34C06、`GetSystemTime` @0x34D9F、`GetDateFormatEx` @0x34DF3（flags 0x42 = LONGDATE\|AUTOLAYOUT）、`GetDateFormatEx` @0x34E49（flags 0x41 = SHORTDATE\|AUTOLAYOUT） |
| `0x35960` | 0x1A52 | `GetTimeFormatEx` @0x359F9（flags 0x2 = TIME_NOSECONDS），被 `0x34BA4` 调用 |

即：时间行来自 `GetTimeFormatEx`，日期行来自两处 `GetDateFormatEx`。

调用者（直接 `call rel32` 扫描）：

- `0x34BA4` 被 `0x2A8F80`、`0x2A91C0`、`0x3BA690` 调用
- `0x35960` 被 `0x2EC90`、`0x34BA4` 调用

## 实际采用的修改方式

**只改 `Taskbar.View.dll` 自己的导入地址表（IAT）两个槽**，即
`GetTimeFormatEx` 和 `GetDateFormatEx`。不改 `kernelbase` 导出，
不装全局 hook，因此 Explorer 内其它模块（文件时间、其它 Shell 组件）
调用这两个 API 完全不受影响。

Hook 内部再按返回地址做二次白名单，只有命中这三个调用点才替换 `SYSTEMTIME`：

```
时间：0x359F9 + 6 = 0x359FF
日期：0x34DF3 + 6 = 0x34DF9
      0x34E49 + 6 = 0x34E4F
```

（返回地址 = 调用指令之后一条指令的地址，即 RVA + 6。）

这三处返回值在真实运行时已实测命中，见下文「验证记录」。

## 时间换算

不使用「本地时间 ± 固定小时」。做法是：

1. `GetSystemTime` 取 UTC；
2. `SystemTimeToTzSpecificLocalTimeEx` 用目标 Windows 命名时区的
   `DYNAMIC_TIME_ZONE_INFORMATION`（由 `EnumDynamicTimeZoneInformation` 解析）换算。

因此 DST、半小时/45 分钟时区、跨午夜换日全部由 Windows 自己的时区规则处理。

## 未被修改的部分（已知差异）

- 日历 / 通知中心面板内部仍由系统时区驱动，不与任务栏时钟同步。
- 系统时区、`GetLocalTime` 的全局返回值、文件时间戳均未改动。
