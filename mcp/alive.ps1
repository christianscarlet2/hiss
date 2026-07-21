$p = Get-CimInstance Win32_Process -Filter "name='pythonw.exe'" | Where-Object { $_.CommandLine -like '*nn_driver*' }
if ($p) { $p | ForEach-Object { Write-Output ("ALIVE PID " + $_.ProcessId) } } else { Write-Output "DEAD - no nn_driver pythonw" }
# can the box reach the bot API?
try { $r = Invoke-WebRequest -UseBasicParsing -TimeoutSec 4 "http://127.0.0.1:27654/api/table-state"; Write-Output ("bot API http " + $r.StatusCode) } catch { Write-Output ("bot API ERR: " + $_.Exception.Message) }
