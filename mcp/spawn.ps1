$c = Get-Content C:\www\openholdembot_old\mcp\launch_hiss.py
for ($i=0; $i -lt $c.Count; $i++) {
  if ($c[$i] -match 'nn_driver|bot-url|bot_url|27654|--bot') {
    $s = [Math]::Max(0,$i-4); $e=[Math]::Min($c.Count-1,$i+4)
    Write-Output "---- around line $($i+1) ----"
    for ($j=$s; $j -le $e; $j++) { Write-Output ("{0}: {1}" -f ($j+1), $c[$j]) }
  }
}
