Write-Output "=== hiss_watchdog.py: does it respawn Hiss.exe? ==="
if (Test-Path C:\tmp\hiss_watchdog.py) {
  Select-String -Path C:\tmp\hiss_watchdog.py -Pattern 'Hiss|launch|Popen|respawn|restart|start|schtasks|HissLaunch|\.exe' -ErrorAction SilentlyContinue |
    ForEach-Object { $_.LineNumber.ToString() + ': ' + $_.Line.Trim() } | Select-Object -First 20
} else { Write-Output "(not found)" }
Write-Output "=== hiss-related tasks state ==="
Get-ScheduledTask | Where-Object { $_.TaskName -match 'Hiss' } | Select-Object TaskName,State | Format-Table -AutoSize | Out-String -Width 120
Write-Output "=== running hiss stack procs ==="
Get-Process Hiss,Automation,Vision -ErrorAction SilentlyContinue | Select-Object Name,Id,SessionId | Format-Table -AutoSize | Out-String -Width 120
