$before = Get-CimInstance Win32_Process -Filter "name='python.exe'" | Where-Object { $_.CommandLine -like '*nn_driver*' }
foreach ($p in $before) {
  Write-Output ("killing OLD driver PID " + $p.ProcessId + " :: " + $p.CommandLine)
  Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Seconds 2
$pyw = "C:\Users\scarl\AppData\Local\Programs\Python\Python310\pythonw.exe"
$proc = Start-Process -FilePath $pyw `
  -ArgumentList @("C:\www\openholdembot_old\mcp\nn_driver.py","--bot-url","http://127.0.0.1:27654") `
  -WorkingDirectory "C:\www\openholdembot_old\mcp" -PassThru
Start-Sleep -Seconds 4
$now = Get-CimInstance Win32_Process -Filter "name='pythonw.exe' OR name='python.exe'" | Where-Object { $_.CommandLine -like '*nn_driver*' }
foreach ($p in $now) { Write-Output ("NOW RUNNING PID " + $p.ProcessId + "  session " + (Get-CimInstance Win32_Process -Filter "ProcessId=$($p.ProcessId)").SessionId + " :: " + $p.CommandLine) }
if (-not $now) { Write-Output "!! NO DRIVER RUNNING AFTER RESTART" }
