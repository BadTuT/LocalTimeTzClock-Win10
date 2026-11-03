param(
    [string]$OutputDirectory,
    [switch]$IncludeModules
)

# 仅读取系统和模块；写入新报告目录，不注入、不重启、不修改时区或注册表。
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $OutputDirectory = Join-Path $repoRoot "artifacts\clock-host-$stamp"
}
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $outputPath) {
    throw "输出目录已存在，为避免覆盖请换一个新目录：$outputPath"
}
[IO.Directory]::CreateDirectory($outputPath) | Out-Null

$windowsInfo = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$report = [ordered]@{
    schema_version = 1
    collected_utc = (Get-Date).ToUniversalTime().ToString('o')
    windows_build = "$($windowsInfo.CurrentBuildNumber).$($windowsInfo.UBR)"
    display_version = $windowsInfo.DisplayVersion
    system_timezone = [TimeZoneInfo]::Local.Id
    loaded_clock_modules = @()
    modules = @()
    errors = @()
}

try {
    foreach ($explorer in @(Get-Process -Name explorer -ErrorAction SilentlyContinue)) {
        foreach ($module in $explorer.Modules) {
            if ($module.ModuleName -in @('SystemTray.dll', 'Taskbar.View.dll')) {
                $report.loaded_clock_modules += [ordered]@{
                    name = $module.ModuleName
                    file_version = $module.FileVersionInfo.FileVersion
                }
            }
        }
    }
} catch {
    $report.errors += "无法读取 Explorer 模块列表：$($_.Exception.Message)"
}

$pythonCommand = Get-Command python.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
$moduleDirectory = Join-Path $env:windir 'SystemApps\MicrosoftWindows.Client.Core_cw5n1h2txyewy'
foreach ($name in @('SystemTray.dll', 'Taskbar.View.dll')) {
    $sourcePath = Join-Path $moduleDirectory $name
    if (-not (Test-Path -LiteralPath $sourcePath)) { continue }
    $analysisPath = $sourcePath
    if ($IncludeModules) {
        $analysisPath = Join-Path $outputPath $name
        Copy-Item -LiteralPath $sourcePath -Destination $analysisPath
    }
    $item = Get-Item -LiteralPath $analysisPath
    $entry = [ordered]@{
        name = $name
        file_version = $item.VersionInfo.FileVersion
        size = $item.Length
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $analysisPath).Hash.ToLowerInvariant()
        deep_evidence = '未运行：未找到 Python'
    }
    if ($pythonCommand) {
        $evidencePath = Join-Path $outputPath ($name + '.json')
        & $pythonCommand.Source -B (Join-Path $PSScriptRoot 'clock_host_evidence.py') $analysisPath --out $evidencePath
        if ($LASTEXITCODE -eq 0) {
            $entry.deep_evidence = [IO.Path]::GetFileName($evidencePath)
            $deepReport = Get-Content -Raw -Encoding UTF8 -LiteralPath $evidencePath | ConvertFrom-Json
            if ($deepReport.sha256 -ne $entry.sha256) {
                $report.errors += "$name 采集期间发生变化，哈希不一致，请重新采集。"
            }
        } else {
            $entry.deep_evidence = "采集失败，Python 退出码 $LASTEXITCODE"
        }
    }
    $report.modules += $entry
}
$report | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $outputPath 'environment.json')
Write-Host "证据目录：$outputPath"
Write-Host '采集未执行任务栏挂钩，也未更改系统设置。'
