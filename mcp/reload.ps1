# stop current driver, relaunch via the persistent scheduled task (loads the new file)
Get-CimInstance Win32_Process -Filter "name='pythonw.exe' OR name='python.exe'" | Where-Object { $_.CommandLine -like '*nn_driver*' } | ForEach-Object { Write-Output ("stopping PID " + $_.ProcessId); Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 2
Start-ScheduledTask -TaskName "NNDriverRestart"
Start-Sleep -Seconds 5
$now = Get-CimInstance Win32_Process -Filter "name='pythonw.exe'" | Where-Object { $_.CommandLine -like '*nn_driver*' }
if ($now) { $now | ForEach-Object { Write-Output ("ALIVE PID " + $_.ProcessId + " sess " + (Get-CimInstance Win32_Process -Filter "ProcessId=$($_.ProcessId)").SessionId) } } else { Write-Output "!! DEAD" }
# bot API reachable?
try { $r = Invoke-WebRequest -UseBasicParsing -TimeoutSec 4 "http://127.0.0.1:27654/api/table-state"; Write-Output ("bot API http " + $r.StatusCode) } catch { Write-Output ("bot API ERR: " + $_.Exception.Message) }
