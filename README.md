<div align="center">

# LocalTimeTzClock

**为 Windows 11 原生任务栏时钟显示任意自定义时区，保持系统全局时区与时间戳绝对不受影响。**

*Display any custom time zone directly in the native Windows 11 taskbar clock — without altering the system time zone, UTC clock, or file timestamps.*

[![Platform](https://img.shields.io/badge/Platform-Windows%2011%20x64-blue?logo=windows)](https://github.com/ana52070/LocalTimeTzClock)
[![Language](https://img.shields.io/badge/Language-C%20%2F%20C%2B%2B%20%28Win32%29-00599C?logo=c%2B%2B)](https://github.com/ana52070/LocalTimeTzClock)
[![Release Size](https://img.shields.io/badge/Release%20Size-38.2%20KB-success)](https://github.com/ana52070/LocalTimeTzClock)
[![Explorer Overhead](https://img.shields.io/badge/Explorer%20Memory-36.0%20KB-brightgreen)](https://github.com/ana52070/LocalTimeTzClock)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

[功能特性](#-功能特性) • [技术原理](#-技术原理与实现路径) • [快速开始](#-快速开始与用法) • [实测性能指标](#-实测性能与资源开销) • [English Summary](#-english-summary)

</div>

---

## 🌟 核心理念与痛点解决

很多跨国办公、出海业务或留学生用户需要任务栏时钟显示特定时区（如北京时间、美东时间、UTC），但直接修改 Windows 系统时区会导致：
1. **严重的附带影响**：浏览器日历、工作软件聊天记录、Git 提交时间戳、IDE 构建日志全部被强行带偏；
2. **已有工具体验糟糕**：市面上的替代方案要么是在右下角浮动一个“假时钟”窗口覆盖原生控件，导致深色/浅色主题脱节、DPI 缩放模糊、失去原生平滑动画，点击时无法唤出原生通知与日历面板；要么是安装体积庞大（数百兆）的复杂套件。

**LocalTimeTzClock** 是一个极简、原生、轻量的 Win32 原生小工具：
- **真正的原生时钟**：直接改写 `Taskbar.View.dll` 的时钟格式化数据源，**不是**浮窗，**不是**覆盖层，**不是**重制控件；
- **零全局污染**：Windows 系统时区保持原样，文件修改时间、其他应用程序读取系统时间 100% 保持原生；
- **完整保留原生体验**：点击打开日历/通知中心、悬停提示、多显示器副任务栏、DPI 缩放、深浅主题切换全部完美保留；
- **极致轻量**：发布包仅 **38 KB**，在 Explorer 内部常驻内存仅 **36 KB**，空闲 CPU 恒定 **0.00%**。

---

## 🔍 技术原理与实现路径

通过对 Windows 11 XAML 任务栏模块做全量逆向指令扫描分析，精确确定挂钩路径：

### 1. 目标模块与 IAT 定向劫持
Windows 11 23H2 的 XAML 任务栏时钟渲染于：
`C:\Windows\SystemApps\MicrosoftWindows.Client.Core_cw5n1h2txyewy\Taskbar.View.dll`

整个模块内只有两处函数引用了时间格式化 API：
- `0x34BA4`：日期行格式化（引用 `GetDateFormatEx` ×2，`GetLocalTime`）
- `0x35960`：时间行格式化（引用 `GetTimeFormatEx`）

本工具**只修改 `Taskbar.View.dll` 模块自身的导入地址表（IAT）**，绝不装载全局 Hook，绝不污染系统级 `kernelbase.dll` 导出表。

### 2. 返回地址白名单（Call-site Filtering）
Hook 函数内部引入了调用点返回地址二次白名单：
- 时间行：仅放行来自 RVA `0x359FF` 的调用；
- 日期行：仅放行来自 RVA `0x34DF9` 与 `0x34E4F` 的调用；
- 其它任何非目标时钟代码调用这两个 API 时，**原样透传执行**，实现真正的零副作用。

### 3. 高精度时区规则转换
拒绝使用简单的“本地时间 ± 固定小时”：
- 采用 `SystemTimeToTzSpecificLocalTimeEx` 配合 Windows 命名时区的 `DYNAMIC_TIME_ZONE_INFORMATION`；
- 原生处理**跨午夜日期切换**、**冬夏令时（DST）平滑切换**、**半小时/45分钟特殊时区**（如印度 UTC+5:30、尼泊尔 UTC+5:45）。

### 4. 现代电源与多屏通知
- **休眠唤醒即时刷新**：采用 Windows 8+ 原生 `RegisterSuspendResumeNotification(DEVICE_NOTIFY_CALLBACK)` 系统回调，唤醒时自动重绘时钟，免去独立监听窗口与常驻消息循环线程；
- **多显示器支持**：主任务栏（`Shell_TrayWnd`）与副显示器任务栏（`Shell_SecondaryTrayWnd`）广播同步刷新。

---

## 🚀 快速开始与用法

### 1. 下载与解压
从 [Releases](../../releases) 下载便携发布包（包含 `tzclock.exe`、`tzclock_v13.dll`、`tzclock.ini`）。

### 2. 配置时区 (`tzclock.ini`)
与 `tzclock.exe` 放置在同一目录下编辑：
```ini
; 任意 Windows 命名时区（TimeZoneKeyName）
; 常见示例: China Standard Time, Tokyo Standard Time, India Standard Time, UTC
TimeZoneId=China Standard Time

; 1 = 启动即生效, 0 = 仅加载不生效
Enabled=1

; 1 = 输出调试日志至 %LOCALAPPDATA%\tzclock.log (日常使用请保持 0)
Log=0
```

### 3. 命令行操作

打开终端（CMD 或 PowerShell）进入程序所在目录：

| 命令 | 作用说明 |
|---|---|
| `tzclock.exe on` | **立即启用**：必要时注入 Explorer，时钟切换至目标时区 |
| `tzclock.exe off` | **立即停用**：无需重启 Explorer，时钟瞬间恢复回系统本地时区 |
| `tzclock.exe status` | **查询实时状态**：打印当前运行状态、版本号及实时格式化重写计数 |
| `tzclock.exe install` | **注册开机自启**：写入 HKCU Run 注册表，开机静默启动守护进程，无黑框 |
| `tzclock.exe uninstall` | **移除开机自启**：清理注册表并自动停止后台运行的守护进程 |
| `tzclock.exe watch` | **守护模式**：若 Explorer 异常崩溃或重启，自动重新挂接时钟 |
| `tzclock.exe stopwatch`| **停止守护**：向正在后台运行的守护进程发送终止信号使其退出 |

---

## 📊 实测性能与资源开销

全部数据基于本机实测（Windows 11 Pro 23H2 · Build 22631.2861 x64 · PST 墨西哥时区）：

| 性能评测指标 | 实测数值 | 测量与验证手段 |
|---|---|---|
| **便携发布包大小** | **38.2 KB** (38,271 字节) | 3 个必要文件逐字节求和（经 `/MD` 优化，缩减 87%） |
| **Explorer 内新增常驻内存** | **36.0 KB** (9 个页面) | `QueryWorkingSetEx` 遍历模块所有页面 Valid 属性 |
| **系统空闲 CPU 占用** | **0.00%** (10 秒采样 0.0 ms) | `TotalProcessorTime` 差值计算 |
| **额外常驻后台进程** | **0 个** (仅日常使用 `on` 模式时) | 控制器运行完毕即刻退出，绝不留存进程 |
| **开机自启守护进程内存** | 工作集 ~3.2 MB / 私有 0.6 MB | 仅在注册 `install` 自启时启动单个静默线程 |
| **时钟刷新节奏** | 沿用原生：每分钟对齐一次 | 不引入高频高刷唤醒，电池友好 |

---

## 🛠️ 源码构建

### 环境需求
- Visual Studio 2019 / 2022 Build Tools (包含 MSVC x64 编译器)
- Windows 10 / 11 SDK

### 一键构建
在仓库根目录执行：
```bat
build.bat dist
```
脚本将自动调用 `vcvarsall.bat` 并生成高度优化的产物至 `dist` 目录。

---

## 🌐 English Summary

### Overview
**LocalTimeTzClock** is an ultra-lightweight, purely native Win32 utility that enables Windows 11 taskbar clock to display any target time zone (e.g., UTC, Beijing, Tokyo) while keeping the global Windows system time zone and file timestamps completely untouched.

### Why It's Better
- **True Native Clock**: No fake overlay windows, no intrusive Electron frameworks, no replacement widgets. The native taskbar clock UI, Fluent design, DPI scaling, and calendar flyout remain 100% functional.
- **Surgically Scoped**: Patches only the Import Address Table (IAT) slots of `Taskbar.View.dll` with return address whitelist verification. Non-clock components inside Explorer are never affected.
- **Accurate Time Transition**: Uses `SystemTimeToTzSpecificLocalTimeEx` with `DYNAMIC_TIME_ZONE_INFORMATION` to handle Daylight Saving Time (DST), midnight rollovers, and 30/45-minute offset time zones.
- **Extreme Efficiency**: Entire release package is only **~38 KB**, resident memory in Explorer is just **36 KB**, with **0.00%** CPU usage.

### Quick Command Reference
```bat
tzclock.exe on          :: Enable custom time zone (injects on demand)
tzclock.exe off         :: Disable immediately (returns to system time zone)
tzclock.exe status      :: Check live status and rewrite counters
tzclock.exe install     :: Enable and register startup autostart (silent background watch)
tzclock.exe uninstall   :: Remove autostart and stop the running watchdog
tzclock.exe stopwatch   :: Gracefully stop active background watchdog instance
```

---

## 📄 开源许可证 (License)

本项目采用 [MIT 许可证](LICENSE)。欢迎自由分发、修改与商业使用。
