Write-Output "=== nn_driver python processes ==="
Get-CimInstance Win32_Process -Filter "name='python.exe'" |
  Where-Object { $_.CommandLine -like '*nn_driver*' } |
  ForEach-Object {
    $sid = (Get-CimInstance Win32_Process -Filter "ProcessId=$($_.ProcessId)").SessionId
    Write-Output ("PID {0}  session {1}  :: {2}" -f $_.ProcessId, $sid, $_.CommandLine)
  }
Write-Output "=== scheduled tasks matching nn_driver / hiss / freeroll ==="
Get-ScheduledTask -ErrorAction SilentlyContinue |
  Where-Object { $_.TaskName -match 'nn|driver|hiss|freeroll' -or ($_.Actions.Arguments -join ' ') -match 'nn_driver' } |
  Select-Object TaskName, State | Format-Table -AutoSize | Out-String -Width 200
Write-Output "=== who am I over ssh + active console session ==="
Write-Output ("ssh user: " + $env:USERNAME)
query session 2>$null
