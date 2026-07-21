# cmd host windows on the interactive desktop
Get-CimInstance Win32_Process -Filter "name='cmd.exe'" | Where-Object { $_.CommandLine -like '*nn_driver*' -or $_.CommandLine -like '*NN driver*' } | ForEach-Object {
  Write-Output ("cmd host PID " + $_.ProcessId + " sess " + (Get-CimInstance Win32_Process -Filter "ProcessId=$($_.ProcessId)").SessionId + " :: " + $_.CommandLine)
}
Get-Process cmd,python -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle } | ForEach-Object { Write-Output ("WINDOW TITLE: '" + $_.MainWindowTitle + "' (pid " + $_.Id + ")") }
# driver functioning?
try { $r = Invoke-WebRequest -UseBasicParsing -TimeoutSec 4 "http://127.0.0.1:27654/api/table-state"; Write-Output ("bot API http " + $r.StatusCode) } catch { Write-Output ("bot API ERR: " + $_.Exception.Message) }
