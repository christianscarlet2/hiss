# 1) Register the symbol engine in CEngineContainer.cpp
$ec = 'C:\www\openholdembot_old\Hiss\CEngineContainer.cpp'
$x = Get-Content $ec -Raw
if ($x -notmatch 'CSymbolEngineScarletBeast') {
  # include near other engine includes (after CSymbolEngineTime.h include if present, else top)
  if ($x -match '#include "CSymbolEngineTime\.h"') {
    $x = $x -replace '(#include "CSymbolEngineTime\.h")', "`$1`r`n#include `"CSymbolEngineScarletBeast.h`""
  } else {
    $x = $x -replace '(#include "CEngineContainer\.h")', "`$1`r`n#include `"CSymbolEngineScarletBeast.h`""
  }
  # create + add after the CSymbolEngineTime registration block
  $x = $x -replace '(p_symbol_engine_time = new CSymbolEngineTime\(\);\s*\r?\n\s*AddSymbolEngine\(p_symbol_engine_time\);)', "`$1`r`n  // CSymbolEngineScarletBeast (server scrape source)`r`n  p_symbol_engine_scarlet_beast = new CSymbolEngineScarletBeast();`r`n  AddSymbolEngine(p_symbol_engine_scarlet_beast);"
  Set-Content -Path $ec -Value $x -NoNewline
  Write-Output "registered in CEngineContainer.cpp"
} else { Write-Output "engine already registered" }

# 2) Add the new files to the vcxproj
$vp = 'C:\www\openholdembot_old\Hiss\Hiss.vcxproj'
$v = Get-Content $vp -Raw
if ($v -notmatch 'CSymbolEngineScarletBeast\.cpp') {
  $v = $v -replace '(\s*<ClCompile Include="CScarletBeast\.cpp" />)', "`$1`r`n    <ClCompile Include=`"CSymbolEngineScarletBeast.cpp`" />"
  $v = $v -replace '(\s*<ClInclude Include="CScarletBeast\.h" />)', "`$1`r`n    <ClInclude Include=`"CSymbolEngineScarletBeast.h`" />"
  Set-Content -Path $vp -Value $v -NoNewline
  Write-Output "added engine to vcxproj"
} else { Write-Output "engine already in vcxproj" }

Select-String -Path $ec -Pattern 'scarlet_beast' | Select-Object -ExpandProperty Line
