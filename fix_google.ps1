$h = 'C:\www\openholdembot_old\Hiss\MainFrm.h'
$x = Get-Content $h -Raw
if ($x -notmatch 'IDM_SB_GOOGLE') {
  # flexible: match IDM_SB_LOBBY define regardless of spacing
  $x = $x -replace '(#define\s+IDM_SB_LOBBY\s+0x8805)', "`$1`r`n#define IDM_SB_GOOGLE  0x8806"
  Set-Content -Path $h -Value $x -NoNewline
  Write-Output "added IDM_SB_GOOGLE"
} else { Write-Output "already defined" }
Select-String -Path $h -Pattern 'IDM_SB_GOOGLE|IDM_SB_LOBBY' | Select-Object -ExpandProperty Line
