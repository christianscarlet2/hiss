Write-Output "=== hiss_watchdog.py : nn_driver handling ==="
if (Test-Path C:\tmp\hiss_watchdog.py) {
  Select-String -Path C:\tmp\hiss_watchdog.py -Pattern 'nn_driver|bot-url|27654|27655|Popen|respawn|sleep|CREATE_NEW|start' -ErrorAction SilentlyContinue |
    ForEach-Object { $_.LineNumber.ToString() + ': ' + $_.Line.Trim() } | Select-Object -First 30
} else { Write-Output "  (C:\tmp\hiss_watchdog.py not found)" }
Write-Output "=== launch_hiss.py : nn_driver spawn ==="
Select-String -Path C:\www\openholdembot_old\mcp\launch_hiss.py -Pattern 'nn_driver|bot-url|27654|27655|Popen|CREATE_NEW|start|session' -ErrorAction SilentlyContinue |
  ForEach-Object { $_.LineNumber.ToString() + ': ' + $_.Line.Trim() } | Select-Object -First 30
