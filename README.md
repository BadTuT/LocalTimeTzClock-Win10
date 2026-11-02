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
  的行为全部不变。

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
| `tzclock.exe` | 147 KB | 控制器（一次性运行，不常驻） |
| `tzclock_v11.dll` | 144 KB | 注入 Explorer 的钩子模块 |
| `tzclock.ini` | 383 B | 配置 |

发布包（3 个必需文件）合计约 **291 KB**。

> 源码里 DLL 文件名带版本号（`tzclock_vN.dll`）。原因：已加载进 Explorer 的 DLL
> 无法覆盖，换名字才能在不重启你资源管理器的情况下重建。改 DLL 时请同步递增
> `build.bat` 里的 `DLLNAME`。

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
tzclock.exe status      查询真实状态
tzclock.exe watch       守护模式：Explorer 重启后自动恢复
tzclock.exe install     启用 + 注册登录自启（HKCU Run，跑 watch）
tzclock.exe uninstall   移除自启
```

停用与卸载：

- `off`：立即生效，不需要重启 Explorer，DLL 留在内存里但只是透传；
- `uninstall`：只删自启注册项；
- 彻底移除：`off` → 删掉目录 → （可选）重启一次 Explorer 让 DLL 释放。
  工具**不会**自己结束你的资源管理器。

---

## 5. 实测验证记录

全部在本机 Windows 11 22631.2861 x64、系统时区
`Pacific Standard Time (Mexico)` 上执行。

已验证：

| 项目 | 结果 |
|---|---|
| 原生时钟显示目标时区 | ✅ 显示 `下午 2:06 / 2/11/2026`，系统时区仍是 Pacific |
| 日期跨天 | ✅ 本地 11/1 22:06 时，时钟显示 11/2 |
| 系统时区未被修改 | ✅ `Get-TimeZone` 仍为 Pacific Standard Time (Mexico) |
| 文件时间戳未受影响 | ✅ 与系统本地时间一致 |
| 停用后恢复原生 | ✅ `off` 后回到 `10:17 / 1/11/2026` |
| 重新启用 | ✅ `on` 后回到北京时间 |
| 无障碍名称同步 | ✅ 时钟按钮 Name 与显示文本同源，同为北京时间 |
| Explorer 重启后自动恢复 | ✅ watchdog 日志：`explorer exited` → `attached to pid …` |
| 时区换算正确性 | ✅ 见下表 |

时区换算验证（`tztest.exe`）：

```
China         UTC 2026-07-01 12:00  ->  2026-07-01 20:00
India (半小时)                         ->  17:30
Nepal (45 分)                          ->  17:45
Chatham (45分+DST)                     ->  2026-07-02 00:45   （跨天）
Lord Howe (30分 DST) 夏/冬             ->  22:30 / 23:00      （DST 生效）
US DST 开始 2026-03-08 09:59Z          ->  01:59
US DST 开始 2026-03-08 10:01Z          ->  03:01              （跳变正确）
US DST 结束 2026-11-01 08:59Z          ->  01:59
US DST 结束 2026-11-01 09:01Z          ->  01:01              （回拨正确）
跨午夜 UTC 2026-11-01 20:00            ->  2026-11-02 04:00
```

**未验证 / 已知限制：**

- **休眠唤醒后的刷新**：DLL 里有 `WM_POWERBROADCAST` 处理并会触发刷新，
  但本次测试会话中**没有真正执行一次休眠/唤醒**，因此这一条**未实测**。
- **启用/停用的即时性**：实测中启用后最坏要等约 60 秒（时钟自身的下一次
  刷新）才更新；`off` 同样。已加入 GetLocalTime 刷新触发，但**没有在每一次
  开关上都稳定缩短到 1 秒内**，这一点按「最长 60 秒」理解更稳妥。
- 只在这一个 build 上验证。call-site RVA 是硬编码的；其它 build 会走
  「扫描调用点」的回退路径，若扫描结果不符合预期（时间 1 处 / 日期 2 处），
  DLL 会**主动还原 IAT 并报错**，不会静默改坏。
- 日历 / 通知中心面板内部仍按系统时区显示「今天」等，**没有**重写。

---

## 6. 资源测量（实测，非估算）

| 项目 | 实测值 | 测量方法 |
|---|---|---|
| 发布包 | 291 KB（3 文件） | `ls -l dist/` |
| 常驻进程 | `tzclock.exe watch` 1 个（仅守护模式） | `Get-Process` |
| 守护进程内存 | 工作集 9.09 MB / 私有 0.89 MB，1 线程 | `WorkingSet64`, `PrivateMemorySize64` |
| **Explorer 内新增** | **172.0 KB** 镜像，43 页，全部驻留 | `wsmeasure.exe`：`QueryWorkingSetEx` 逐页查 Valid 位 |
| 空闲 CPU | 10 秒采样 **0.0 ms**（<0.001%） | `TotalProcessorTime` 差值 |
| 刷新频率 | 沿用原生：每分钟一次（未改成高频） | 观察时钟文本变化 |

```bat
:: 复现 Explorer 内占用
wsmeasure.exe <explorer-pid> tzclock
:: 输出
:: module      : tzclock_v11.dll
:: image size  : 172.0 KB (43 pages)
:: resident    : 172.0 KB (43 pages)
```

说明：

- 守护进程只在 Explorer 进程退出时被唤醒（`WaitForSingleObject`），
  不轮询窗口、不遍历进程。
- Explorer 内的常驻开销就是那 172 KB 的镜像页 + 每次时钟刷新时几次函数调用，
  以及一个 message-only 窗口（用于接收电源事件）。
- 未测量项：DLL 在 Explorer 内的私有堆增量没有单独隔离测量
  （被 shell 正常波动淹没，无法给出可信数字，故不编造）。

---

## 7. 目录结构

```
build.bat              构建脚本
tzclock.ini            配置示例
src/dll/tzclock.cpp    IAT 钩子模块
src/app/tzclock.cpp    控制器 / watchdog
src/app/tztest.cpp     时区换算验证
src/app/wsmeasure.cpp  驻留内存测量
docs/analysis.md       原生时钟路径的实测分析
```
