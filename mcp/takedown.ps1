# 1) suppress the watchdog brain-teardown so stopping Hiss does NOT cascade-kill ail_server/nn_driver
New-Item -ItemType File -Path C:\tmp\hiss_no_teardown.flag -Force | Out-Null
Remove-Item C:\tmp\hiss_was_up.marker -Force -ErrorAction SilentlyContinue
Write-Output "suppress flag SET, marker cleared"

# 2) hide the driver: stop visible python.exe driver + its cmd window, relaunch windowless (pythonw)
Get-CimInstance Win32_Process -Filter "name='python.exe' OR name='pythonw.exe'" | Where-Object { $_.CommandLine -like '*nn_driver*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Get-CimInstance Win32_Process -Filter "name='cmd.exe'" | Where-Object { $_.CommandLine -like '*nn_driver*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 2
$act = New-ScheduledTaskAction -Execute "C:\Users\scarl\AppData\Local\Programs\Python\Python310\pythonw.exe" -Argument "C:\www\openholdembot_old\mcp\nn_driver.py --bot-url http://127.0.0.1:27654" -WorkingDirectory "C:\www\openholdembot_old\mcp"
$pr = New-ScheduledTaskPrincipal -UserId "scarl" -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName "NNDriverRestart" -Action $act -Principal $pr -Force | Out-Null
Start-ScheduledTask -TaskName "NNDriverRestart"
Start-Sleep -Seconds 4

# 3) take the bot down (Hiss.exe workers + brass_serpent if present)
Get-Process brass_serpent,Hiss -ErrorAction SilentlyContinue | ForEach-Object { Write-Output ("stopping bot " + $_.Name + " pid " + $_.Id); Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 3

Write-Output "=== AFTER ==="
$bot = Get-Process brass_serpent,Hiss -ErrorAction SilentlyContinue
Write-Output ("bot procs remaining: " + @($bot).Count + "  (want 0)")
$win = Get-CimInstance Win32_Process -Filter "name='cmd.exe'" | Where-Object { $_.CommandLine -like '*nn_driver*' }
Write-Output ("driver console windows: " + @($win).Count + "  (want 0)")
$drv = Get-CimInstance Win32_Process -Filter "name='pythonw.exe'" | Where-Object { $_.CommandLine -like '*nn_driver*' }
if ($drv) { $drv | ForEach-Object { Write-Output ("driver windowless ALIVE pid " + $_.ProcessId + " sess " + (Get-CimInstance Win32_Process -Filter "ProcessId=$($_.ProcessId)").SessionId) } } else { Write-Output "driver NOT running" }
foreach ($n in 'ail_server','hud_aggregator') {
  $d = Get-CimInstance Win32_Process -Filter "name='python.exe' OR name='pythonw.exe'" | Where-Object { $_.CommandLine -like "*$n*" }
  Write-Output ($n + " kept alive: " + @($d).Count)
}
