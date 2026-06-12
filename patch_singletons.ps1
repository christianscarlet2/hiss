$p = 'C:\www\openholdembot_old\Hiss\Singletons.cpp'
$x = Get-Content $p -Raw
if ($x -match 'p_scarlet_beast') { Write-Output 'already patched'; exit }
$x = $x -replace '(#include "CAutoConnector\.h")', "`$1`r`n#include `"CScarletBeast.h`""
$x = $x -replace '(p_autoconnector = new CAutoConnector;)', "`$1`r`n  assert(!p_scarlet_beast);`r`n  p_scarlet_beast = new CScarletBeast;"
$x = $x -replace '(DELETE_AND_CLEAR\(p_autoconnector\))', "`$1`r`n  DELETE_AND_CLEAR(p_scarlet_beast)"
Set-Content -Path $p -Value $x -NoNewline
Write-Output 'patched Singletons.cpp'
Select-String -Path $p -Pattern 'p_scarlet_beast' | Select-Object -ExpandProperty Line
