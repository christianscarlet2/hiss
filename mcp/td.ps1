$paths = @("C:\tmp\terminate_daemons.ps1","C:\www\openholdembot_old\mcp\terminate_daemons.ps1")
$f = $paths | Where-Object { Test-Path $_ } | Select-Object -First 1
Write-Output ("teardown script: " + $f)
if ($f) {
  Select-String -Path $f -Pattern 'Stop-Process|taskkill|Get-Process|-Name|nn_driver|ail_server|brass_serpent|Hiss|Automation|Vision|python|claude|pythonw|Stop-ScheduledTask' -ErrorAction SilentlyContinue |
    ForEach-Object { $_.LineNumber.ToString() + ': ' + $_.Line.Trim() } | Select-Object -First 40
}
Write-Output "=== suppress flag present? ==="
Write-Output ("suppress: " + (Test-Path C:\tmp\hiss_no_teardown.flag))
Write-Output ("marker:   " + (Test-Path C:\tmp\hiss_was_up.marker))
