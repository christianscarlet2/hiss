$h = 'C:\www\openholdembot_old\Hiss\MainFrm.h'
$c = 'C:\www\openholdembot_old\Hiss\MainFrm.cpp'
$x = Get-Content $h -Raw
if ($x -notmatch 'IDM_SB_GOOGLE') {
  $x = $x -replace '(#define IDM_SB_LOBBY   0x8805)', "`$1`r`n#define IDM_SB_GOOGLE  0x8806"
  $x = $x -replace '(afx_msg void OnSbLobby\(\);)', "`$1`r`n  afx_msg void OnSbGoogle();"
  Set-Content -Path $h -Value $x -NoNewline
  Write-Output "MainFrm.h patched"
} else { Write-Output "h already" }
$y = Get-Content $c -Raw
if ($y -notmatch 'OnSbGoogle') {
  $y = $y -replace '(ON_COMMAND\(IDM_SB_LOBBY, &CMainFrame::OnSbLobby\))', "`$1`r`n	ON_COMMAND(IDM_SB_GOOGLE, &CMainFrame::OnSbGoogle)"
  Set-Content -Path $c -Value $y -NoNewline
  Write-Output "MainFrm.cpp patched"
} else { Write-Output "cpp already" }
