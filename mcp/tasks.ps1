foreach ($t in 'HissWatchdog','HissLaunch') {
  Write-Output "=== $t ==="
  $ti = Get-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue
  if ($ti) {
    foreach ($a in $ti.Actions) { Write-Output ("  EXEC: " + $a.Execute + " " + $a.Arguments) }
    Write-Output ("  Triggers: " + (($ti.Triggers | ForEach-Object { $_.CimClass.CimClassName }) -join ', '))
  }
}
Write-Output "=== files that launch nn_driver with --bot-url ==="
Select-String -Path C:\www\openholdembot_old\mcp\*.ps1,C:\www\openholdembot_old\mcp\*.bat,C:\www\openholdembot_old\*.ps1,C:\www\openholdembot_old\*.bat -Pattern 'nn_driver' -ErrorAction SilentlyContinue |
  ForEach-Object { $_.Path + " :: " + $_.Line.Trim() } | Select-Object -First 20
