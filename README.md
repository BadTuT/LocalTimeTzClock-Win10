# LocalTimeTzClock

让 Windows 自带任务栏时钟显示指定时区的时间与日期，**系统时区保持不变**。

改的是原生「时钟」本身，不是浮窗、不是覆盖层、不是替代控件。

---

## 1. 它到底改了什么

修改对象：`Taskbar.View.dll` 里时钟自己的**导入地址表（IAT）**。

| 项目 | 实测值 |
|---|---|
| 测试系统 | Windows 11 Pro 23H2 |
| Build | 10.0.22631.2861 |
| 架构 | x64 |
| 时钟所在模块 | `C:\Windows\SystemApps\MicrosoftWindows.Client.Core_cw5n1h2txyewy\Taskbar.View.dll` |
| 模块版本 | 623.27903.70.0 |

Win11 23H2 的 XAML 任务栏把时钟渲染在这个模块里。对整个模块做全量指令扫描后，
**引用时间格式化 API 的函数只有两个**：

```
0x34BA4  GetLocalTime / GetSystemTime / GetDateFormatEx ×2   （日期行）
0x35960  GetTimeFormatEx                                     （时间行，由 0x34BA4 调用）
```

被挂钩的导入槽（只这三个 + 一个用于刷新节奏）：

```
GetTimeFormatEx  IAT RVA 0x617578   调用点返回地址 RVA 0x359FF
GetDateFormatEx  IAT RVA 0x617580   调用点返回地址 RVA 0x34DF9 / 0x34E4F
GetLocalTime     IAT RVA 0x617880   仅用于让时钟尽快重绘，不参与时间显示
```

所以：

- 系统时区、UTC 时间、自动对时都未改动；
- 没有装全局 hook。Explorer 里**其它**模块调用 `GetTimeFormatEx` /
  `GetDateFormatEx`（例如资源管理器的文件时间）走的是各自独立的导入槽，
  完全不受影响；
- Hook 内部还有一层返回地址白名单：**只有命中上面那几个时钟调用点才会改写**，
  其它调用原样透传；
- 任务栏时钟仍是原来那个控件，位置、字体、主题、DPI、点击打开日历/通知中心
  的行为全部不变；
- 刷新通知已覆盖多显示器任务栏（`Shell_SecondaryTrayWnd`）。

时间换算用 `SystemTimeToTzSpecificLocalTimeEx` + 目标时区的
`DYNAMIC_TIME_ZONE_INFORMATION`，不是「本地时间 ± 固定小时」，
所以 DST、半小时区、45 分钟区、跨午夜换日都由 Windows 自己的规则处理。

详细分析见 [`docs/analysis.md`](docs/analysis.md)。

---

## 2. 构建

工具链（本机实测可用）：

- Visual Studio 2019 Build Tools，MSVC 14.29.30133
- Windows SDK 10.0.26100.0

```bat
build.bat dist
```

产物：

| 文件 | 大小 | 说明 |
|---|---|---|
| `tzclock.exe` | ~17 KB | 控制器（一次性运行，不常驻） |
| `tzclock_v12.dll` | ~20 KB | 注入 Explorer 的钩子模块 |
| `tzclock.ini` | 383 B | 配置 |

发布包（3 个必需文件）合计 **约 38.2 KB**（经 `/MD` 优化，相比原版的 292.4 KB 缩减了 **87%**）。

> 源码里 DLL 文件名带版本号（`tzclock_vN.dll`）。原因：已加载进 Explorer 的 DLL
> 无法覆盖，换名字才能在不重启资源管理器的情况下热升级。

---

## 3. 配置

`tzclock.ini`，和 EXE 放同一目录：

```ini
; 任意 Windows 命名时区（TimeZoneKeyName）
TimeZoneId=China Standard Time
; 1 = 启动即生效，0 = 只装载不生效
Enabled=1
; 1 = 写 %LOCALAPPDATA%\tzclock.log（默认关闭）
Log=0
```

其它时区写法示例：`Tokyo Standard Time`、`India Standard Time`、
`Nepal Standard Time`、`UTC`。

---

## 4. 用法

```bat
tzclock.exe on          启用（必要时注入）
tzclock.exe off         停用，时钟立刻回到系统时区
tzclock.exe status      查询真实状态（即时显示实时命中次数）
tzclock.exe watch       守护模式：Explorer 重启后自动恢复
tzclock.exe stopwatch   主动停止正在后台运行的守护进程
tzclock.exe install     启用 + 注册登录自启（HKCU Run，无黑框静默运行）
tzclock.exe uninstall   移除自启并停止守护进程
```

停用与卸载：

- `off`：立即生效，不需要重启 Explorer，DLL 留在内存里但只是透传；
- `stopwatch`：优雅停止 watchdog 守护进程；
- `uninstall`：删自启注册项并停止守护；
- 彻底移除：`off` → 删掉目录 → （可选）重启一次 Explorer 让 DLL 释放。
  工具**不会**自己结束你的资源管理器。

---

## 5. 实测验证记录

全部在本机 Windows 11 22631.2861 x64、系统时区
`Pacific Standard Time (Mexico)` 上执行。

已验证：

| 项目 | 结果 |
|---|---|
| 原生时钟显示目标时区 | ✅ 显示目标北京时间，系统时区仍是 Pacific |
| 日期跨天 | ✅ 正确处理跨日换算 |
| 系统时区未被修改 | ✅ `Get-TimeZone` 仍为 Pacific Standard Time (Mexico) |
| 文件时间戳未受影响 | ✅ 与系统本地时间一致 |
| 停用后恢复原生 | ✅ `off` 后回到系统时区 |
| 重新启用 | ✅ `on` 后回到目标时区 |
| 无障碍名称同步 | ✅ 时钟按钮 Name 与显示文本同源 |
| Explorer 重启后自动恢复 | ✅ watchdog 守护进程捕获退出并重连 |
| 多显示器任务栏同步 | ✅ 广播并刷新 `Shell_SecondaryTrayWnd` |
| 休眠唤醒刷新 | ✅ `RegisterSuspendResumeNotification` 回调驱动 |
| 跨桌面隔离执行 | ✅ 支持从后台服务、非交互桌面、子桌面通过会话 PID 探测 Explorer |

---

## 6. 资源测量（实测，非估算）

| 项目 | 实测值 | 测量方法 |
|---|---|---|
| 发布包 | **38.2 KB**（3 文件） | 逐文件字节数求和（相比优化前减少 87%） |
| 常驻进程 | `tzclock.exe watch` 1 个（仅守护模式） | `Get-Process` |
| 守护进程内存 | 工作集 ~3.2 MB / 私有 0.6 MB，1 线程 | `WorkingSet64`, `PrivateMemorySize64` |
| **Explorer 内新增** | **36.0 KB** 镜像，9 页，全部驻留 | `QueryWorkingSetEx` 逐页查 Valid 位（相比优化前减少 79%） |
| 空闲 CPU | 10 秒采样 **0.0 ms**（<0.001%） | `TotalProcessorTime` 差值 |
| 刷新频率 | 沿用原生：每分钟一次（未改成高频） | 观察时钟文本变化 |

说明：

- 守护进程只在 Explorer 进程退出或收到 `stopwatch` 事件时被唤醒（`WaitForMultipleObjects`），不轮询窗口、不遍历进程。
- Explorer 内常驻开销仅 **36 KB**（9 页内存），休眠恢复通过系统回调驱动，无需专用消息循环线程或窗口。

---

## 7. 目录结构

```
build.bat              构建脚本
tzclock.ini            配置示例
src/dll/tzclock.cpp    IAT 钩子模块（单趟特征扫描、原子计数、安全时间回退）
src/app/tzclock.cpp    控制器 / watchdog（支持会话穿透与远程停止）
src/app/tztest.cpp     时区换算验证
src/app/wsmeasure.cpp  驻留内存测量
docs/analysis.md       原生时钟路径的实测分析
```
