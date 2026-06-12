$h = 'C:\www\openholdembot_old\Hiss\MainFrm.h'
$c = 'C:\www\openholdembot_old\Hiss\MainFrm.cpp'
$vp = 'C:\www\openholdembot_old\Hiss\Hiss.vcxproj'

# --- MainFrm.h: defines + handler declarations ---
$x = Get-Content $h -Raw
if ($x -notmatch 'IDM_SB_CONNECT') {
  $defs = @"
// Scarlet Beast menu command IDs
#define WM_SB_INITMENU  (WM_USER + 0x4201)
#define IDM_SB_CONNECT  0x8801
#define IDM_SB_SETKEY   0x8802
#define IDM_SB_SETTABLE 0x8803
#define IDM_SB_TEST     0x8804
#define IDM_SB_LOBBY    0x8805

class CMainFrame
"@
  $x = $x -replace 'class CMainFrame', $defs
  $decls = @"
afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
 public:
  void AppendScarletBeastMenu();
  afx_msg LRESULT OnSbInitMenu(WPARAM wParam, LPARAM lParam);
  afx_msg void OnSbConnect();
  afx_msg void OnSbSetKey();
  afx_msg void OnSbSetTable();
  afx_msg void OnSbTest();
  afx_msg void OnSbLobby();
 public:
"@
  $x = $x -replace 'afx_msg int OnCreate\(LPCREATESTRUCT lpCreateStruct\);', $decls
  Set-Content -Path $h -Value $x -NoNewline
  Write-Output "patched MainFrm.h"
} else { Write-Output "MainFrm.h already patched" }

# --- MainFrm.cpp: message map + OnCreate post ---
$y = Get-Content $c -Raw
if ($y -notmatch 'OnSbConnect') {
  $map = @"
ON_COMMAND(ID_HELP_PROBLEMSOLVER, &CMainFrame::OnHelpProblemSolver)
	ON_MESSAGE(WM_SB_INITMENU, &CMainFrame::OnSbInitMenu)
	ON_COMMAND(IDM_SB_CONNECT, &CMainFrame::OnSbConnect)
	ON_COMMAND(IDM_SB_SETKEY, &CMainFrame::OnSbSetKey)
	ON_COMMAND(IDM_SB_SETTABLE, &CMainFrame::OnSbSetTable)
	ON_COMMAND(IDM_SB_TEST, &CMainFrame::OnSbTest)
	ON_COMMAND(IDM_SB_LOBBY, &CMainFrame::OnSbLobby)
"@
  $y = $y -replace 'ON_COMMAND\(ID_HELP_PROBLEMSOLVER, &CMainFrame::OnHelpProblemSolver\)', $map
  $y = $y -replace '(p_openholdem_statusbar = new COpenHoldemStatusbar\(this\);)', "`$1`r`n	PostMessage(WM_SB_INITMENU);"
  Set-Content -Path $c -Value $y -NoNewline
  Write-Output "patched MainFrm.cpp"
} else { Write-Output "MainFrm.cpp already patched" }

# --- vcxproj: add ScarletBeastMenu.cpp ---
$v = Get-Content $vp -Raw
if ($v -notmatch 'ScarletBeastMenu\.cpp') {
  $v = $v -replace '(\s*<ClCompile Include="CSymbolEngineScarletBeast\.cpp" />)', "`$1`r`n    <ClCompile Include=`"ScarletBeastMenu.cpp`" />"
  Set-Content -Path $vp -Value $v -NoNewline
  Write-Output "added ScarletBeastMenu.cpp to vcxproj"
} else { Write-Output "already in vcxproj" }
