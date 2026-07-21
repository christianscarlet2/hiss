Write-Output "=== bot processes ==="
Get-Process brass_serpent,Hiss,Automation,Vision -ErrorAction SilentlyContinue | Select-Object Name,Id,SessionId | Format-Table -AutoSize | Out-String -Width 120
Write-Output "=== brain daemons (python) ==="
Get-CimInstance Win32_Process -Filter "name='python.exe' OR name='pythonw.exe'" | Where-Object { $_.CommandLine -match 'ail_server|nn_driver|synapse_map|decide_engine|hud_aggregator' } | ForEach-Object { Write-Output (($_.Name) + " pid " + $_.ProcessId + " :: " + ($_.CommandLine -replace '.*mcp\\','')) }
Write-Output "=== driver cmd window(s) ==="
Get-CimInstance Win32_Process -Filter "name='cmd.exe'" | Where-Object { $_.CommandLine -like '*nn_driver*' } | ForEach-Object { Write-Output ("cmd pid " + $_.ProcessId) }
Write-Output "=== HissLaunch trigger (would it relaunch Hiss?) ==="
$t = Get-ScheduledTask -TaskName HissLaunch
$t.Triggers | ForEach-Object { Write-Output ("  " + $_.CimClass.CimClassName + "  repeat=" + $_.Repetition.Interval + "  atLogon=" + $_.CimClass.CimClassName) }
(Get-ScheduledTask HissWatchdog).Triggers | ForEach-Object { Write-Output ("  watchdog: " + $_.CimClass.CimClassName + " repeat=" + $_.Repetition.Interval) }
