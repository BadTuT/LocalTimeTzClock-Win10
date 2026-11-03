<div align="center">

# LocalTimeTzClock

**为 Windows 11 原生任务栏时钟显示任意自定义时区，保持系统全局时区与时间戳绝对不受影响。**

*Display any custom time zone directly in the native Windows 11 taskbar clock — without altering the system time zone, UTC clock, or file timestamps.*

[![Platform](https://img.shields.io/badge/Platform-Windows%2011%20x64-blue?logo=windows)](https://github.com/ana52070/LocalTimeTzClock)
[![Language](https://img.shields.io/badge/Language-C%20%2F%20C%2B%2B%20%28Win32%29-00599C?logo=c%2B%2B)](https://github.com/ana52070/LocalTimeTzClock)
[![Release Size](https://img.shields.io/badge/Release%20Size-38.2%20KB-success)](https://github.com/ana52070/LocalTimeTzClock)
[![Explorer Overhead](https://img.shields.io/badge/Explorer%20Memory-36.0%20KB-brightgreen)](https://github.com/ana52070/LocalTimeTzClock)
[![Community](https://img.shields.io/badge/Community-LINUX%20DO-orange?logo=linux)](https://linux.do/)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

[功能特性](#-功能特性) • [技术原理](#-技术原理与实现路径) • [快速开始](#-快速开始与用法) • [实测性能指标](#-实测性能与资源开销) • [致谢](#-致谢-acknowledgements) • [English Summary](#-english-summary)

</div>

---

## 🌟 核心理念与痛点解决

很多跨国办公、出海业务、留学生以及**玩 Claude / AI 怕封号需要改系统时区**的用户，经常需要任务栏时钟与真实生活作息同步，但直接修改 Windows 系统时区会导致：
1. **严重的附带影响**：浏览器日历、工作软件聊天记录、Git 提交时间戳、IDE 构建日志全部被强行带偏；
2. **过检测后的看表痛点**：为了防封把 Windows 系统时区改成了美西（如 `America/Tijuana` 或 `Pacific Standard Time`），通过了 IP/环境防伪装检测（如 `ip.net.coffee/claude`），但看一眼电脑右下角任务栏直接迷茫，根本不知道国内到底几点了！
3. **已有工具体验糟糕**：市面上的替代方案要么是在右下角浮动一个“假时钟”窗口覆盖原生控件，导致深色/浅色主题脱节、DPI 缩放模糊、失去原生平滑动画，点击时无法唤出原生通知与日历面板；要么是安装体积庞大（数百兆）的复杂套件。

**LocalTimeTzClock** 是一个极简、原生、轻量的 Win32 原生小工具：
- **真正的原生时钟**：直接改写任务栏时钟模块数据源，**不是**浮窗，**不是**覆盖层，**不是**重制控件；
- **零全局污染**：Windows 系统时区保持原样（系统底层依旧是美西时区，完美过各种风控环境检测），文件修改时间、其他应用程序读取系统时间 100% 保持原生；
- **完整保留原生体验**：点击打开日历/通知中心、悬停提示、多显示器副任务栏、DPI 缩放、深浅主题切换全部完美保留；
- **极致轻量**：发布包仅 **38 KB**，在 Explorer 内部常驻内存仅 **36 KB**，空闲 CPU 恒定 **0.00%**。

---

## 🔍 技术原理与实现路径

通过真实模块的导入与调用点扫描、匹配微软符号及关键数据流反汇编，限定挂钩路径：

### 1. 目标模块探测与双候选架构
为了支持不同 Windows 11 版本的任务栏架构，工具内置双候选优先级探测机制：
1. `SystemTray.dll`（候选 1，Windows 11 Build 26300 系列的新宿主）
2. `Taskbar.View.dll`（候选 2，现有已验证环境的宿主）

在向目标模块应用挂钩前，工具**必须先完成只读身份与调用点探测**。严格校验 PE 机器架构 (x64)、PE 时间戳 (TimeDateStamp)、映像大小 (SizeOfImage) 以及导入地址表 (IAT) 与调用点布局。**对未经验证的模块布局或超限调用点一律安全拒绝，绝不进行截断挂钩或破坏性写入。**

当前已实现并通过静态与隔离测试的配置如下；**Build 26300 的真实桌面验收尚未执行**，不能把配置匹配视为全部功能已验收。来源、采集步骤和待验收项目见 [Build26300 验证与验收](docs/Build26300验证与验收.md)。

#### 配置 A：Windows 11 Build 26300 (`SystemTray.dll` 2609.11000.100.0)
- **模块身份**：FileVersion `2609.11000.100.0`，TimeDateStamp `0x6AA9E607`，SizeOfImage `0x202000`，Machine `0x8664`（AMD64）。Winbindex 关联 Windows 11 Build 26300.9807 / 26300.9818。
- **挂钩策略**：`STRATEGY_LOCAL_SOURCE`
- **挂钩范围**：保持同模块 IAT，**仅挂钩 `GetLocalTime` 槽 (`0x18B270`)**。
  - **RefreshIcon 主时钟数据源返回点 (`0xCA3BD`, `0x12B73D`)**：启用时用同一次目标时间取样完整替换 `SYSTEMTIME`，包含真实秒数、毫秒数；不拼接旧取样字段，不消费 kick，并记录命中次数。
  - **ScheduleNextUpdate 调度返回点 (`0xCAA7D`, `0x12BCED`)**：不改变时区，仅在有刷新请求时消费一次 kick 并将秒设为 59；无 kick 或非调度点透传。
  - **附加时钟安全隔离**：该版本中 `GetTimeString` 与 `GetShortenedTimeString` 被 `GetTimeToolTipString` 用于附加时钟格式化。本工具**绝对不改写其 `GetTimeFormatEx` (`0x18B288`) 与 `GetDateFormatEx` (`0x18B278`) 槽**，从而完整保留附加时钟的独立时区换算与格式化输出。
  - **全量调用点核验**：只读核对全部 18 个调用点（time 7 个：`0xC8150`, `0xC85F3`, `0xC8A7B`, `0xC8A90`, `0x129C9B`, `0x129F4F`, `0x129F64`；date 7 个：`0xCC3F9`, `0xCC5BE`, `0xCC5F7`, `0xCC9C6`, `0x12CAFE`, `0x12CCB9`, `0x12CCEF`；local 4 个：`0xCA3BD`, `0xCAA7D`, `0x12B73D`, `0x12BCED`），调用点不全或多出额外调用均安全拒绝。
- **支持范围边界**：严格限制于官方符号与真实二进制核验的 `2609.11000.100.0`，不放宽版本匹配，不将 `2609.11000` 视为作者所称 `2609.12001`，亦不宣称支持全部 26300 版本。

#### 配置 B：Windows 11 23H2 (`Taskbar.View.dll` 623.27903.70.0)
- **模块身份**：FileVersion `623.27903.70.0`，TimeDateStamp `0x654E6445`，SizeOfImage `0x758000`，Machine `0x8664`（AMD64）。
- **挂钩策略**：`STRATEGY_FORMAT_REWRITE`
- **挂钩范围**：挂钩 `GetTimeFormatEx` (`0x617578`)、`GetDateFormatEx` (`0x617580`) 与 `GetLocalTime` (`0x617880`)。
  - 时间返回调用点：`0x359FF`
  - 日期返回调用点：`0x34DF9`, `0x34E4F`
  - 调度 kick 返回点：`0x34C0C` (仅此调用点允许消费 kick，其余调用完全透传)

### 2. 返回地址白名单（Call-site Filtering）
Hook 函数内部引入了调用点返回地址二次白名单与严格判定 Helper：
- 仅放行来自已验证白名单 RVAs 的调用；
- 其它非目标调用原样透传，限制修改范围。

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
已发布便携包可从 [Releases](https://github.com/ana52070/LocalTimeTzClock/releases) 下载，DLL 名以对应发布版本为准。当前源码构建产物为 `tzclock.exe`、`tzclock_v16.dll`、`tzclock.ini`（位于 `artifacts/pr2-v16`）。

升级时请成套替换控制器和 DLL。旧版 DLL 即使执行 `off`，也仍留在 Explorer 中并保留透传挂钩；新版本发现已有挂钩会拒绝叠加。请保存工作后自行重新登录或重启 Explorer，再测试 v16；工具不会自动终止 Explorer。

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
build.bat artifacts\pr2-v16
```
脚本将自动调用 `vcvarsall.bat` 并生成高度优化的产物至指定目录。

---

## 📋 新版系统适配与人工验收清单 (Manual Acceptance Checklist)

新增配置先完成模块、符号、数据流和隔离测试核验；发布为稳定支持前，还需在受控的对应 Windows 版本桌面环境完成下列人工验收：

- [ ] **1. 真实宿主定位与导入表确认**：确认目标系统下任务栏时钟所在的真实宿主 DLL（如 `SystemTray.dll`），核实其是否直接导入目标 API，且具备合法的 `OriginalFirstThunk`。
- [ ] **2. 调用点全量语义审计**：静态反汇编分析所有调用点，明确区分主时钟数据源与附加时钟等其他指定时间的显示；非目标调用必须透传。
- [ ] **3. 进程隔离与无副作用注入**：在独立测试环境运行 `tzclock.exe on`，确认仅目标模块 IAT 被定向挂接，`explorer.exe` 内其他模块时间戳与时区 100% 保持原生系统时区。
- [ ] **4. 多显示器副任务栏同步重绘**：在双屏/多屏环境下切换时区，确认主任务栏（`Shell_TrayWnd`）与副任务栏（`Shell_SecondaryTrayWnd`）均能即时完成重绘刷新。
- [ ] **5. 电源休眠唤醒测试**：使系统进入睡眠/休眠状态，唤醒后确认任务栏时钟能通过 `RegisterSuspendResumeNotification` 回调自动唤醒重绘。
- [ ] **6. 跨日及冬夏令时边界跳转**：模拟跨午夜日期变更及 DST 换令时点，验证跨日格式化显示正常无闪烁。
- [ ] **7. 停用与恢复**：运行 `tzclock.exe off`，确认挂钩切换为透传，时钟恢复系统时区且无崩溃。

---

## 💖 致谢 (Acknowledgements)

特别鸣谢 **[LINUX DO](https://linux.do/)** 社区及其热心佬友们的支持与启发！  
*Special thanks to the **[LINUX DO](https://linux.do/)** community for inspiration and support!*

---

## 🌐 English Summary

### Overview
**LocalTimeTzClock** is an ultra-lightweight, purely native Win32 utility that enables Windows 11 taskbar clock to display any target time zone (e.g., UTC, Beijing, Tokyo) while keeping the global Windows system time zone and file timestamps completely untouched. Perfect for bypass scenarios (like anti-fraud or Claude AI environment consistency) where the system timezone is set abroad, but you still want your native taskbar clock to display local time.

### Why It's Better
- **True Native Clock**: No fake overlay windows, no intrusive Electron frameworks, no replacement widgets. The native taskbar clock UI, Fluent design, DPI scaling, and calendar flyout remain 100% functional.
- **修改范围**：只修改已核实宿主的指定 IAT 槽并核对调用点；新版保留附加时钟的独立格式化路径，真实界面验收仍待完成。
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
