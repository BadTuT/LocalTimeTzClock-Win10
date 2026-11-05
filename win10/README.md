# LocalTimeTzClock — Windows 10 adaptation

为 Windows 10 原生任务栏主时钟提供独立的显示时区，系统时区和实际 UTC 时间保持原样。

基于 [ana52070/LocalTimeTzClock](https://github.com/ana52070/LocalTimeTzClock)，沿用 MIT 许可证。
本分支版本为 v17，新增 Windows 10 22H2 x64 的定向适配。

## 适配范围

| 项目 | 当前目标 |
| --- | --- |
| 系统 | Windows 10 专业版 22H2，OS Build 19045.2728，x64 |
| 任务栏宿主 | `explorer.exe`，文件版本 `10.0.19041.2311` |
| 默认显示时区 | `China Standard Time`（北京时间） |
| 主时钟 | 时间、日期、星期采用目标时区 |
| 悬停提示与附加时钟 | 沿用 Windows 原有取时逻辑 |
| 点击后的日历面板 | 保留 Windows 原生交互与显示逻辑 |

原有两套 Windows 11 布局继续保留；本分支新增的运行确认聚焦上述 Win10 版本。
其他 Explorer 版本需要对应的布局适配。

## 使用

将 `tzclock.exe`、`tzclock_v17.dll`、`tzclock.ini` 保持在同一文件夹。

```powershell
.\tzclock.exe on       # 启用目标时区显示
.\tzclock.exe status   # 查询运行状态
.\tzclock.exe off      # 切回系统时区显示
```

系统设置为 `Pacific Standard Time` 时，应用和系统继续采用美西时间，
任务栏主时钟采用配置中的北京时间。程序本身保留系统时区原值。

配置文件：

```ini
TimeZoneId=China Standard Time
Enabled=1
Log=0
```

原项目的可选登录自启功能继续保留：

```powershell
.\tzclock.exe install
.\tzclock.exe uninstall
```

普通 `on` 模式在控制器完成后退出，时钟逻辑留在当前 Explorer 会话中。
Explorer 重启或重新登录后，可再次执行 `on`，或使用原项目的自启功能。

## 构建

准备 Visual Studio x64 C++ 工具链和 Windows SDK，在仓库根目录执行：

```bat
build.bat dist
```

构建脚本通过 Visual Studio Installer 的 `vswhere.exe` 定位工具链。
产物为 `dist\tzclock.exe`、`dist\tzclock_v17.dll`、`dist\tzclock.ini`。

## Win10 实现

- 新增 Explorer 布局，复用 `STRATEGY_LOCAL_SOURCE`。
- 仅在 `ClockButton::UpdateTextStringsIfNecessary(bool*)` 中的指定
  `GetLocalTime` 返回位置提供目标时区时间。
- 主时钟的时间、日期、星期共用同一份 `SYSTEMTIME`。
- 其余六处 `GetLocalTime` 调用保持原有结果，时间与日期格式化槽保持原样。
- 针对 Win10 的 API-set 导入，以 KernelBase 实际函数地址核对原始入口。
- 向 Win10 主、副任务栏时钟窗口发送既有刷新消息。

详细说明见 [docs/Windows10.md](docs/Windows10.md)。

## 升级

控制器与 DLL 成套使用。已加载的 DLL 随 Explorer 会话结束而释放；
切换 DLL 版本时先保存工作并重新登录，再启用新版本。

## 许可证与来源

[MIT](LICENSE)。保留原作者版权声明；原项目和提交历史见上游仓库。
