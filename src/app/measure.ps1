# Resource measurement for the delivery report. Run with the tool active.
$ErrorActionPreference='Stop'
$watch = Get-Process tzclock -ErrorAction SilentlyContinue
$expl  = Get-Process explorer
$exp = $expl | Select-Object -First 1

"=== resident components ==="
foreach($p in $watch){ "tzclock.exe (watchdog) pid={0} working={1:N2} MB private={2:N2} MB threads={3}" -f $p.Id, ($p.WorkingSet64/1MB), ($p.PrivateMemorySize64/1MB), $p.Threads.Count }
"explorer.exe pid=$($exp.Id) working=$([math]::Round($exp.WorkingSet64/1MB,2)) MB private=$([math]::Round($exp.PrivateMemorySize64/1MB,2)) MB threads=$($exp.Threads.Count)"

"`n=== modules loaded by us inside explorer ==="
$exp.Modules | Where-Object {$_.ModuleName -match 'tzclock'} | ForEach-Object {
  "  {0}  base=0x{1:X}  size={2:N1} KB" -f $_.ModuleName, [int64]$_.BaseAddress, ($_.ModuleMemorySize/1KB)
}

"`n=== idle CPU sample (10 s) ==="
function Cpu($p){ $p.Refresh(); return $p.TotalProcessorTime.TotalMilliseconds }
$w0 = if($watch){ Cpu ($watch | Select-Object -First 1) } else { 0 }
$e0 = Cpu $exp
Start-Sleep -Seconds 10
$w1 = if($watch){ Cpu ($watch | Select-Object -First 1) } else { 0 }
$e1 = Cpu $exp
"watchdog CPU over 10s: {0:N1} ms ({1:N3}% of one core)" -f ($w1-$w0), (($w1-$w0)/10000*100)
"explorer CPU over 10s: {0:N1} ms (baseline includes all shell work)" -f ($e1-$e0)
