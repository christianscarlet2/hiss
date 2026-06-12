$vp = 'C:\www\openholdembot_old\Hiss\Hiss.vcxproj'
$v = Get-Content $vp -Raw
if ($v -notmatch 'CScarletBeastLobby\.cpp') {
  $v = $v -replace '(\s*<ClCompile Include="ScarletBeastMenu\.cpp" />)', "`$1`r`n    <ClCompile Include=`"CScarletBeastLobby.cpp`" />"
  $v = $v -replace '(\s*<ClInclude Include="CSymbolEngineScarletBeast\.h" />)', "`$1`r`n    <ClInclude Include=`"CScarletBeastLobby.h`" />"
  Set-Content -Path $vp -Value $v -NoNewline
  Write-Output "added lobby to vcxproj"
} else { Write-Output "already in vcxproj" }
