# ensure none running
Get-CimInstance Win32_Process -Filter "name='pythonw.exe' OR name='python.exe'" | Where-Object { $_.CommandLine -like '*nn_driver*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 1
$act = New-ScheduledTaskAction -Execute "C:\Users\scarl\AppData\Local\Programs\Python\Python310\pythonw.exe" `
  -Argument "C:\www\openholdembot_old\mcp\nn_driver.py --bot-url http://127.0.0.1:27654" `
  -WorkingDirectory "C:\www\openholdembot_old\mcp"
$pr = New-ScheduledTaskPrincipal -UserId "scarl" -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName "NNDriverRestart" -Action $act -Principal $pr -Force | Out-Null
Start-ScheduledTask -TaskName "NNDriverRestart"
Start-Sleep -Seconds 5
$now = Get-CimInstance Win32_Process -Filter "name='pythonw.exe'" | Where-Object { $_.CommandLine -like '*nn_driver*' }
if ($now) { $now | ForEach-Object { Write-Output ("ALIVE PID " + $_.ProcessId + " sess " + (Get-CimInstance Win32_Process -Filter "ProcessId=$($_.ProcessId)").SessionId + " :: " + $_.CommandLine) } }
else { Write-Output "!! STILL DEAD" ; (Get-ScheduledTask NNDriverRestart | Get-ScheduledTaskInfo) | Select-Object LastRunTime,LastTaskResult | Format-List }
