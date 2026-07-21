# stop windowless driver
Get-CimInstance Win32_Process -Filter "name='pythonw.exe' OR name='python.exe'" | Where-Object { $_.CommandLine -like '*nn_driver*' } | ForEach-Object { Write-Output ("stopping PID " + $_.ProcessId); Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 2
# repoint the task at the console launcher, interactive in scarl's session (visible window)
$act = New-ScheduledTaskAction -Execute "cmd.exe" -Argument "/c C:\www\openholdembot_old\mcp\nn_driver_console.bat" -WorkingDirectory "C:\www\openholdembot_old\mcp"
$pr  = New-ScheduledTaskPrincipal -UserId "scarl" -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName "NNDriverRestart" -Action $act -Principal $pr -Force | Out-Null
Start-ScheduledTask -TaskName "NNDriverRestart"
Start-Sleep -Seconds 6
$now = Get-CimInstance Win32_Process -Filter "name='python.exe'" | Where-Object { $_.CommandLine -like '*nn_driver*' }
if ($now) { $now | ForEach-Object { Write-Output ("ALIVE python.exe PID " + $_.ProcessId + " sess " + (Get-CimInstance Win32_Process -Filter "ProcessId=$($_.ProcessId)").SessionId) } } else { Write-Output "!! no python.exe nn_driver" }
# is there a visible cmd window hosting it?
Get-Process cmd -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -like '*NN driver*' } | ForEach-Object { Write-Output ("WINDOW: '" + $_.MainWindowTitle + "' pid " + $_.Id) }
