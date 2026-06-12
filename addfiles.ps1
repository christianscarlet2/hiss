$p = 'C:\www\openholdembot_old\Hiss\Hiss.vcxproj'
$x = Get-Content $p -Raw
if ($x -notmatch 'CScarletBeast\.cpp') {
  $x = $x -replace '(\s*<ClCompile Include="CScraper\.cpp" />)', "`$1`r`n    <ClCompile Include=`"CScarletBeast.cpp`" />"
  $x = $x -replace '(\s*<ClInclude Include="CScraper\.h" />)', "`$1`r`n    <ClInclude Include=`"CScarletBeast.h`" />"
  Set-Content -Path $p -Value $x -NoNewline
  Write-Output "ADDED CScarletBeast to vcxproj"
} else { Write-Output "already present" }
# verify
Select-String -Path $p -Pattern 'CScarletBeast' | Select-Object -ExpandProperty Line
