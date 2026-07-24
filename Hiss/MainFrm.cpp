//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose:
//
//******************************************************************************

// MainFrm.cpp : implementation of the CMainFrame class
//

#include "stdafx.h"
#include "MainFrm.h"
#include <io.h>
#include <process.h>
#include <tlhelp32.h>
#include <map>
#include <set>
#include <vector>
#include "..\DLLs\Files_DLL\Files.h"
#include "..\DLLs\StringFunctions_DLL\string_functions.h"
#include "..\DLLs\WindowFunctions_DLL\window_functions.h"
#include "CAutoConnector.h"
#include "CTablePositioner.h"
#include "CAutoplayer.h"
#include "CAutoplayerFunctions.h"
#include "ChatTerminalServer.h"
#include "ChatTerminalWindow.h"
#include "CEngineContainer.h"
#include "CFlagsToolbar.h"
#include "CHeartbeatThread.h"
#include "CIteratorThread.h"
#include "COpenHoldemHopperCommunication.h"
#include "COpenHoldemStatusbar.h"
#include "COpenHoldemTitle.h"

#include "CProblemSolver.h"
#include "CSymbolEngineReplayFrameController.h"
#include "CScraper.h"
#include "CSessionCounter.h"
#include "..\CTablemap\CTablemapDB.h"
#include "CSymbolEngineUserchair.h"
#include "CSymbolEngineTableLimits.h"
#include "..\CTransform\CTransform.h"
#include "CValidator.h"
#include "DialogFormulaScintilla.h"
#include "DialogSAPrefs2.h"
#include "DialogSAPrefs3.h"
#include "DialogSAPrefs4.h"
#include "DialogSAPrefs6.h"
#include "DialogSAPrefs7.h"
#include "DialogSAPrefs8.h"
#include "DialogSAPrefs9.h"
#include "DialogSAPrefs10.h"
#include "DialogSAPrefs11.h"
#include "DialogSAPrefs12.h"
#include "DialogSAPrefs13.h"
#include "DialogSAPrefs14.h"
#include "DialogSAPrefs15.h"
#include "DialogSAPrefs16.h"
#include "DialogSAPrefs17.h"
#include "DialogSAPrefs19.h"
#include "DialogSAPrefs20.h"
#include "DialogSAPrefs21.h"
#include "DialogSAPrefs22.h"
#include "DialogSAPrefs23.h"
#include "DialogSAPrefs24.h"
#include "DialogSAPrefs25.h"
#include "DialogSAPrefs26.h"
#include "DialogScraperOutput.h"
#include "inlines/eval.h"
#include "OpenHoldem.h"
#include "OpenHoldemDoc.h"
#include "ReactTableWindow.h"
#include "HudOverlayWindow.h"
#include "ReactMappingsWindow.h"
#include "WindowDockUtil.h"
#include "SAPrefsDialog.h"
#include "Singletons.h"
#include "CTwoSuccessiveClicks.h"

// CMainFrame

IMPLEMENT_DYNCREATE(CMainFrame, CFrameWnd)

// Cross-instance broadcast: the same string resolves to the same message id in
// every Hiss process, so a HWND_BROADCAST reaches all instances' main windows.
static UINT WM_HISS_PANIC_AUTOPLAYER_OFF =
	::RegisterWindowMessage("HissPanicAutoplayerOff_v1");

// Low-level keyboard hook handle (one main window per process).
HHOOK CMainFrame::s_panic_kbd_hook = NULL;

BEGIN_MESSAGE_MAP(CMainFrame, CFrameWnd)
	ON_WM_CREATE()
	ON_REGISTERED_MESSAGE(WM_HISS_PANIC_AUTOPLAYER_OFF, &CMainFrame::OnPanicAutoplayerOff)

	// Menu updates
	ON_UPDATE_COMMAND_UI(ID_FILE_NEW, &CMainFrame::OnUpdateMenuFileNew)
	ON_UPDATE_COMMAND_UI(ID_FILE_OPEN, &CMainFrame::OnUpdateMenuFileOpen)
  ON_UPDATE_COMMAND_UI(ID_EDIT_FORMULA, &CMainFrame::OnUpdateMenuFileEdit)
	ON_UPDATE_COMMAND_UI(ID_VIEW_SHOOTREPLAYFRAME, &CMainFrame::OnUpdateViewShootreplayframe)
  ON_UPDATE_COMMAND_UI(ID_VIEW_SCRAPEROUTPUT, &CMainFrame::OnUpdateViewScraperOutput)

	//  Menu commands
	ON_COMMAND(ID_FILE_OPEN, &CMainFrame::OnFileOpen)
	ON_COMMAND(ID_EDIT_FORMULA, &CMainFrame::OnEditFormula)
	ON_COMMAND(ID_EDIT_PREFERENCES, &CMainFrame::OnEditPreferences)
	ON_COMMAND(ID_EDIT_VIEWLOG, &CMainFrame::OnEditViewLog)
	ON_COMMAND(ID_EDIT_TAGLOG, &CMainFrame::OnEditTagLog)
  ON_COMMAND(ID_EDIT_CLEARLOG, &CMainFrame::OnEditClearLog)
	ON_COMMAND(ID_EDIT_VERIFYNAMEMAPPINGS, &CMainFrame::OnEditVerifyNameMappings)
	ON_COMMAND(ID_VIEW_SCRAPEROUTPUT, &CMainFrame::OnScraperOutput)
	ON_COMMAND(ID_VIEW_SHOOTREPLAYFRAME, &CMainFrame::OnViewShootreplayframe)
	ON_COMMAND(ID_HELP_HELP, &CMainFrame::OnHelp)
	ON_COMMAND(ID_HELP_OPEN_PPL, &CMainFrame::OnHelpOpenPPL)
	ON_COMMAND(ID_HELP_FORUMS, &CMainFrame::OnHelpForums)
	ON_COMMAND(ID_HELP_PROBLEMSOLVER, &CMainFrame::OnHelpProblemSolver)
	ON_MESSAGE(WM_SB_INITMENU, &CMainFrame::OnSbInitMenu)
	ON_COMMAND(IDM_SB_CONNECT, &CMainFrame::OnSbConnect)
	ON_COMMAND(IDM_SB_SETKEY, &CMainFrame::OnSbSetKey)
	ON_COMMAND(IDM_SB_SETTABLE, &CMainFrame::OnSbSetTable)
	ON_COMMAND(IDM_SB_TEST, &CMainFrame::OnSbTest)
	ON_COMMAND(IDM_SB_LOBBY, &CMainFrame::OnSbLobby)
	ON_COMMAND(IDM_SB_GOOGLE, &CMainFrame::OnSbGoogle)
	ON_COMMAND(IDM_SB_AUTOREBUY, &CMainFrame::OnSbAutoRebuy)
	ON_COMMAND(IDM_SB_LOADHUD, &CMainFrame::OnSbLoadHud)

	// Main toolbar 
	ON_BN_CLICKED(ID_MAIN_TOOLBAR_AUTOPLAYER, &CMainFrame::OnAutoplayer)
	ON_BN_CLICKED(ID_MAIN_TOOLBAR_NN_DRIVER, &CMainFrame::OnNnDriver)
	ON_UPDATE_COMMAND_UI(ID_MAIN_TOOLBAR_NN_DRIVER, &CMainFrame::OnUpdateNnDriver)
	ON_BN_CLICKED(ID_MAIN_TOOLBAR_ULTRA, &CMainFrame::OnUltra)
	ON_UPDATE_COMMAND_UI(ID_MAIN_TOOLBAR_ULTRA, &CMainFrame::OnUpdateUltra)
	ON_BN_CLICKED(ID_MAIN_TOOLBAR_SUPERSTITION, &CMainFrame::OnSuperstition)
	ON_BN_CLICKED(ID_MAIN_TOOLBAR_AUTOMATION, &CMainFrame::OnAutomation)
	ON_UPDATE_COMMAND_UI(ID_MAIN_TOOLBAR_SUPERSTITION, &CMainFrame::OnUpdateSuperstition)
	ON_BN_CLICKED(ID_MAIN_TOOLBAR_FORMULA, &CMainFrame::OnEditFormula)
	ON_BN_CLICKED(ID_MAIN_TOOLBAR_VALIDATOR, &CMainFrame::OnValidator)
	ON_BN_CLICKED(ID_MAIN_TOOLBAR_TAGLOGFILE, &CMainFrame::OnEditTagLog)
	ON_BN_CLICKED(ID_MAIN_TOOLBAR_SCRAPER_OUTPUT, &CMainFrame::OnScraperOutput)
	ON_BN_CLICKED(ID_MAIN_TOOLBAR_SHOOTFRAME, &CMainFrame::OnViewShootreplayframe)
  ON_BN_CLICKED(ID_MAIN_TOOLBAR_MANUALMODE, &CMainFrame::OnManualMode)
	ON_BN_CLICKED(ID_MAIN_TOOLBAR_HELP, &CMainFrame::OnHelp)

	// Hopper
	ON_MESSAGE(WMA_SETWINDOWTEXT, &COpenHoldemHopperCommunication::OnSetWindowText)
	ON_MESSAGE(WMA_DOCONNECT,     &COpenHoldemHopperCommunication::OnConnectMessage)
	ON_MESSAGE(WMA_DODISCONNECT,  &COpenHoldemHopperCommunication::OnDisconnectMessage)
	ON_MESSAGE(WMA_CONNECTEDHWND, &COpenHoldemHopperCommunication::OnConnectedHwndMessage)
	ON_MESSAGE(WMA_SETFLAG,       &COpenHoldemHopperCommunication::OnSetFlagMessage)
	ON_MESSAGE(WMA_RESETFLAG,     &COpenHoldemHopperCommunication::OnResetFlagMessage)
	ON_MESSAGE(WMA_ISREADY,       &COpenHoldemHopperCommunication::OnIsReadyMessage)
    // Receiving strings requires a WM_COPYDATA-message and a special WM_COPYDATA-structure
	ON_MESSAGE(WM_COPYDATA,       &COpenHoldemHopperCommunication::OnGetSymbolMessage)

	// Flags
	ON_BN_CLICKED(ID_NUMBER0,  &CMainFrame::OnClickedFlags)
	ON_BN_CLICKED(ID_NUMBER1,  &CMainFrame::OnClickedFlags)
	ON_BN_CLICKED(ID_NUMBER2,  &CMainFrame::OnClickedFlags)
	ON_BN_CLICKED(ID_NUMBER3,  &CMainFrame::OnClickedFlags)
	ON_BN_CLICKED(ID_NUMBER4,  &CMainFrame::OnClickedFlags)
	ON_BN_CLICKED(ID_NUMBER5,  &CMainFrame::OnClickedFlags)
	ON_BN_CLICKED(ID_NUMBER6,  &CMainFrame::OnClickedFlags)
	ON_BN_CLICKED(ID_NUMBER7,  &CMainFrame::OnClickedFlags)
	ON_BN_CLICKED(ID_NUMBER8,  &CMainFrame::OnClickedFlags)
	ON_BN_CLICKED(ID_NUMBER9,  &CMainFrame::OnClickedFlags)
	ON_BN_CLICKED(ID_NUMBER10, &CMainFrame::OnClickedFlags)
	ON_BN_CLICKED(ID_NUMBER11, &CMainFrame::OnClickedFlags)
	ON_BN_CLICKED(ID_NUMBER12, &CMainFrame::OnClickedFlags)
	ON_BN_CLICKED(ID_NUMBER13, &CMainFrame::OnClickedFlags)
	ON_BN_CLICKED(ID_NUMBER14, &CMainFrame::OnClickedFlags)
	ON_BN_CLICKED(ID_NUMBER15, &CMainFrame::OnClickedFlags)
	ON_BN_CLICKED(ID_NUMBER16, &CMainFrame::OnClickedFlags)
	ON_BN_CLICKED(ID_NUMBER17, &CMainFrame::OnClickedFlags)
	ON_BN_CLICKED(ID_NUMBER18, &CMainFrame::OnClickedFlags)
	ON_BN_CLICKED(ID_NUMBER19, &CMainFrame::OnClickedFlags)

	ON_WM_TIMER()
	ON_WM_MOVE()
  ON_UPDATE_COMMAND_UI(ID_INDICATOR_STATUS_ACTION, OnUpdateStatus)
	ON_UPDATE_COMMAND_UI(ID_INDICATOR_STATUS_HANDRANK,OnUpdateStatus)
	ON_UPDATE_COMMAND_UI(ID_INDICATOR_STATUS_PRWIN,OnUpdateStatus)
	
	ON_WM_SETCURSOR()
	ON_WM_SYSCOMMAND()
END_MESSAGE_MAP()

// CMainFrame construction/destruction
CMainFrame::CMainFrame() {
	_wait_cursor = false;

	_prev_att_rect.bottom = 0;
	_prev_att_rect.left = 0;
	_prev_att_rect.right = 0;
	_prev_att_rect.top = 0;
	_prev_wrect.bottom = 0;
	_prev_wrect.left = 0;
	_prev_wrect.right = 0;
	_prev_wrect.top = 0;
}

// Walks the process tree rooted at our own PID and terminates every
// msedgewebview2.exe descendant (the React-rendering child processes spawned
// by our embedded WebView2 windows, plus their own renderer/gpu children).
// Scoped strictly to descendants by parent-PID matching so we never touch
// WebView2 processes belonging to other Hiss instances or other apps.
static void KillOurWebView2Descendants() {
	DWORD self_pid = GetCurrentProcessId();

	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) {
		return;
	}

	// Snapshot every running process once, indexed both by pid and by parent
	// so we can BFS the descendant tree without re-snapshotting.
	struct ProcInfo {
		DWORD parent_pid;
		CString name;
	};
	std::map<DWORD, ProcInfo> by_pid;
	std::map<DWORD, std::vector<DWORD> > by_parent;

	PROCESSENTRY32 entry;
	memset(&entry, 0, sizeof(entry));
	entry.dwSize = sizeof(entry);
	if (Process32First(snap, &entry)) {
		do {
			ProcInfo info;
			info.parent_pid = entry.th32ParentProcessID;
			info.name = entry.szExeFile;
			info.name.MakeLower();
			by_pid[entry.th32ProcessID] = info;
			by_parent[entry.th32ParentProcessID].push_back(entry.th32ProcessID);
		} while (Process32Next(snap, &entry));
	}
	CloseHandle(snap);

	// BFS from self_pid, terminating any msedgewebview2.exe we descend into.
	// Use a visited set to guard against PID-reuse cycles in the snapshot.
	std::vector<DWORD> stack;
	std::set<DWORD> visited;
	stack.push_back(self_pid);
	visited.insert(self_pid);
	while (!stack.empty()) {
		DWORD pid = stack.back();
		stack.pop_back();
		std::map<DWORD, std::vector<DWORD> >::const_iterator it = by_parent.find(pid);
		if (it == by_parent.end()) {
			continue;
		}
		for (size_t i = 0; i < it->second.size(); ++i) {
			DWORD child_pid = it->second[i];
			if (!visited.insert(child_pid).second) {
				continue;
			}
			stack.push_back(child_pid);
			std::map<DWORD, ProcInfo>::const_iterator pit = by_pid.find(child_pid);
			if (pit != by_pid.end() && pit->second.name == "msedgewebview2.exe") {
				HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, child_pid);
				if (h != NULL) {
					TerminateProcess(h, 0);
					CloseHandle(h);
				}
			}
		}
	}
}

CMainFrame::~CMainFrame() {
	if (p_chat_terminal_server != NULL) {
		p_chat_terminal_server->Stop();
		delete p_chat_terminal_server;
		p_chat_terminal_server = NULL;
	}
	if (p_react_table_window != NULL) {
		if (::IsWindow(p_react_table_window->GetSafeHwnd())) {
			p_react_table_window->DestroyWindow();
		}
		delete p_react_table_window;
		p_react_table_window = NULL;
	}
	// Destroyed BEFORE the HUD overlay: its paint path reads the HUD's hero anchor rect.
	if (p_hud_action_window != NULL) {
		if (::IsWindow(p_hud_action_window->GetSafeHwnd())) {
			p_hud_action_window->DestroyWindow();
		}
		delete p_hud_action_window;
		p_hud_action_window = NULL;
	}
	if (p_hud_overlay_window != NULL) {
		if (::IsWindow(p_hud_overlay_window->GetSafeHwnd())) {
			p_hud_overlay_window->DestroyWindow();
		}
		delete p_hud_overlay_window;
		p_hud_overlay_window = NULL;
	}
	if (p_react_mappings_window != NULL) {
		if (::IsWindow(p_react_mappings_window->GetSafeHwnd())) {
			p_react_mappings_window->DestroyWindow();
		}
		delete p_react_mappings_window;
		p_react_mappings_window = NULL;
	}
	if (p_chat_terminal != NULL) {
		if (::IsWindow(p_chat_terminal->GetSafeHwnd())) {
			p_chat_terminal->DestroyWindow();
		}
		delete p_chat_terminal;
		p_chat_terminal = NULL;
	}
	if (p_flags_toolbar != NULL) {
		delete(p_flags_toolbar);
	}
  if (p_openholdem_statusbar != NULL) {
    delete p_openholdem_statusbar;
  }

	// Sweep any WebView2 (React frontend) processes that descend from us.
	// Scoped to our own subtree by parent-PID matching, so other Hiss
	// instances' React windows are left alone.
	KillOurWebView2Descendants();
}

int CMainFrame::OnCreate(LPCREATESTRUCT lpCreateStruct) {
	CString			t = "";
	lpCreateStruct->dwExStyle |= WS_MINIMIZE;
	if (CFrameWnd::OnCreate(lpCreateStruct) == kUndefined)
		return -1;
	Hiss_AppendAlwaysOnTopMenu(this);
	Hiss_RestoreAlwaysOnTop(this, &_always_on_top, _T("MainFrm"));   // persisted on exit
	// Tool bar
	p_flags_toolbar = new CFlagsToolbar(this);
	// Status bar
	p_openholdem_statusbar = new COpenHoldemStatusbar(this);
	PostMessage(WM_SB_INITMENU);
	// ChatGPT-style terminal companion window
	p_chat_terminal = new CChatTerminalWindow();
	p_chat_terminal->Create(this);
	p_chat_terminal->ShowWindow(SW_HIDE);   // hidden at startup; summon it from the terminal toolbar/menu [Emrald]
	ChatTerminalAppend(kChatTerminalContext, "OpenHoldem terminal attached. Drag it left or right of the main window to choose the side.");
	p_chat_terminal_server = new CChatTerminalServer();
	if (!p_chat_terminal_server->Start()) {
		ChatTerminalAppend(kChatTerminalContext, "Terminal API server failed to start.");
	}
	p_react_table_window = new CReactTableWindow();
	p_react_table_window->Create(this, p_chat_terminal_server->port());
	// Transparent PT4 HUD overlay drawn over the attached scrcpy table window.
	p_hud_overlay_window = new CHudOverlayWindow();
	p_hud_overlay_window->Create(this);
	// The bot's RED decision rides on its OWN solid overlay so the HUD tiles above can stay faint
	// (a layered window's alpha is global). Created after the HUD so it starts above it.
	p_hud_action_window = new CHudActionWindow();
	p_hud_action_window->Create(this);
	// Start timer that checks if we should enable buttons
	SetTimer(ENABLE_BUTTONS_TIMER, 50, 0);
	// Start timer that updates status bar
	SetTimer(UPDATE_STATUS_BAR_TIMER, 500, 0);
  // Start timer that checks for continued existence of attached HWND
  SetTimer(HWND_CHECK_TIMER, 200, 0);
	// Global Win+H+Q panic kill-switch (works from any window).
	InstallPanicHotkey();
	return 0;
}

// Low-level keyboard hook: when Q is pressed while both Win and H are held down,
// broadcast the panic message to every Hiss instance and swallow the Q so it does
// not leak into whatever window has focus.
LRESULT CALLBACK CMainFrame::PanicKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
	if (code == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
		KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lParam;
		if (k != NULL && k->vkCode == 'Q') {
			bool win = ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0)
			        || ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);
			bool h = ((GetAsyncKeyState('H') & 0x8000) != 0);
			if (win && h) {
				::PostMessage(HWND_BROADCAST, WM_HISS_PANIC_AUTOPLAYER_OFF, 0, 0);
				return 1;  // swallow Q
			}
		}
	}
	return CallNextHookEx(s_panic_kbd_hook, code, wParam, lParam);
}

void CMainFrame::InstallPanicHotkey() {
	if (s_panic_kbd_hook != NULL) return;
	s_panic_kbd_hook = SetWindowsHookEx(WH_KEYBOARD_LL, PanicKeyboardProc,
		GetModuleHandle(NULL), 0);
	write_log(k_always_log_basic_information,
		"[MainFrame] Panic hotkey (Win+H+Q) %s\n",
		s_panic_kbd_hook != NULL ? "installed" : "FAILED to install");
}

void CMainFrame::RemovePanicHotkey() {
	if (s_panic_kbd_hook != NULL) {
		UnhookWindowsHookEx(s_panic_kbd_hook);
		s_panic_kbd_hook = NULL;
	}
}

// Handler for the broadcast: shut off this instance's autoplayer.
LRESULT CMainFrame::OnPanicAutoplayerOff(WPARAM wParam, LPARAM lParam) {
	if (p_autoplayer != NULL && p_autoplayer->autoplayer_engaged()) {
		p_autoplayer->EngageAutoplayer(false);
		write_log(k_always_log_basic_information,
			"[MainFrame] PANIC Win+H+Q -> autoplayer DISENGAGED.\n");
		ChatTerminalAppend(kChatTerminalContext, "PANIC (Win+H+Q): autoplayer disengaged.");
	}
	return 0;
}

void CMainFrame::OnSysCommand(UINT nID, LPARAM lParam) {
	if (Hiss_HandleAlwaysOnTopSysCommand(this, nID, &_always_on_top, _T("MainFrm"))) return;
	CFrameWnd::OnSysCommand(nID, lParam);
}

void CMainFrame::OnMove(int x, int y) {
	CFrameWnd::OnMove(x, y);
	// Re-glue to our side only while the window is still DOCKED. Once the user drags
	// it away (detached), leave it where it is.
	if (p_chat_terminal != NULL && p_chat_terminal->IsDocked()) {
		p_chat_terminal->AttachToOwner(true);
	}
	if (p_react_table_window != NULL && p_react_table_window->IsDocked()) {
		p_react_table_window->AttachToOwner();
	}
}

BOOL CMainFrame::PreCreateWindow(CREATESTRUCT& cs) {
	if ( !CFrameWnd::PreCreateWindow(cs) )
		return FALSE;

	int			max_x = 0, max_y = 0;

	WNDCLASS wnd;
	HINSTANCE hInst = AfxGetInstanceHandle();

	// Set class name
	if (!(::GetClassInfo(hInst, Preferences()->window_class_name(), &wnd))) {
		wnd.style			    = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
		wnd.lpfnWndProc		= ::DefWindowProc;
		wnd.cbClsExtra		= wnd.cbWndExtra = 0;
		wnd.hInstance		  = hInst;
		wnd.hIcon			    = AfxGetApp()->LoadIcon(IDI_ICON1);
		wnd.hCursor			  = AfxGetApp()->LoadStandardCursor(IDC_ARROW);
		wnd.hbrBackground	= (HBRUSH) (COLOR_3DFACE + 1);
		wnd.lpszMenuName	= NULL;
		wnd.lpszClassName	= Preferences()->window_class_name();
    // Resizable window: the table view + HUD scale to the client area,
    // and the FCKRA/TIOLP indicators stay locked to the bottom corners.
    cs.dwExStyle &= ~WS_EX_CLIENTEDGE;
	  cs.style |= WS_SIZEBOX;
	  cs.style |= WS_BORDER;
	  cs.style |= WS_MAXIMIZEBOX;

		AfxRegisterClass( &wnd );
	}
	cs.lpszClass = Preferences()->window_class_name();

	// Restore window location and size
  // -32 to avoid placement directly under the taskbar,
  // so that at least a little bit is visible
  // if the values in the ini-file are out of range.
	max_x = GetSystemMetrics(SM_CXSCREEN) - GetSystemMetrics(SM_CXICON - 32);
	max_y = GetSystemMetrics(SM_CYSCREEN) - GetSystemMetrics(SM_CYICON - 32);
  // Make sure that our coordinates are not out of screen
  // (too large or even negative)
	cs.x = min(Preferences()->main_x(), max_x);
	cs.y = min(Preferences()->main_y(), max_y);
  cs.x = max(cs.x, 0);
  cs.y = max(cs.y, 0);
  // GUI size. Default the height to match the React table window (560) so the two
  // windows line up side by side by default; the window stays resizable.
	cs.cx = kMainSizeX;
	cs.cy = 560;   // kReactTableHeight

	return true;
}

// CMainFrame message handlers

void CMainFrame::OnEditFormula() {
	if (m_formulaScintillaDlg) {
		if (m_formulaScintillaDlg->m_dirty)	{
			if (MessageBox_Interactive(
				  "The Formula Editor has un-applied changes.\n"
				  "Really exit?", 
				  "Formula Editor", MB_ICONWARNING|MB_YESNO) == IDNO) {
				p_flags_toolbar->EnableButton(ID_MAIN_TOOLBAR_FORMULA, true);
				return;
			}
		}
    BOOL	bWasShown = ::IsWindow(m_formulaScintillaDlg->m_hWnd) && m_formulaScintillaDlg->IsWindowVisible();
    m_formulaScintillaDlg->DestroyWindow();
    if (bWasShown) {
			return;
    }
	}
  if (p_autoplayer->autoplayer_engaged()) {
    // The menu item Edit->Formula is disabled,
    // this is just an extra failsafe.
    return;
  }
  m_formulaScintillaDlg = new CDlgFormulaScintilla(this);
	m_formulaScintillaDlg->Create(CDlgFormulaScintilla::IDD,this);
	m_formulaScintillaDlg->ShowWindow(SW_SHOW);
	p_flags_toolbar->EnableButton(ID_MAIN_TOOLBAR_FORMULA, true);
}

void CMainFrame::OnEditViewLog() {
  assert(p_sessioncounter != nullptr);
  OpenFileInExternalSoftware(LogFilePath(p_sessioncounter->session_id()));
}

void CMainFrame::OnEditTagLog() {
	write_log(k_always_log_basic_information,
		"[*** ATTENTION ***] User tagged this situation for review\n");
}

void CMainFrame::OnEditClearLog() {
  clear_log();
}

// Menu -> Edit -> Verify Name Mappings...
// Opens the OCR -> actual-name mapping verification page in an embedded
// browser window (served by the chat-terminal HTTP server at /mappings/).
void CMainFrame::OnEditVerifyNameMappings() {
	if (p_chat_terminal_server == NULL) {
		MessageBox("Terminal API server is not running; cannot open mappings window.",
			"Verify Name Mappings", MB_OK | MB_ICONERROR);
		return;
	}
	if (p_react_mappings_window != NULL && ::IsWindow(p_react_mappings_window->GetSafeHwnd())) {
		// Already open — bring it to front and refresh.
		p_react_mappings_window->ShowWindow(SW_SHOW);
		p_react_mappings_window->SetForegroundWindow();
		p_react_mappings_window->NavigateToMappings(p_chat_terminal_server->port());
		return;
	}
	if (p_react_mappings_window != NULL) {
		delete p_react_mappings_window;
		p_react_mappings_window = NULL;
	}
	p_react_mappings_window = new CReactMappingsWindow();
	if (!p_react_mappings_window->Create(this, p_chat_terminal_server->port())) {
		delete p_react_mappings_window;
		p_react_mappings_window = NULL;
		MessageBox("Could not create the mappings window.",
			"Verify Name Mappings", MB_OK | MB_ICONERROR);
	}
}

// Menu -> Edit -> View Scraper Output
void CMainFrame::OnScraperOutput() {
	if (m_ScraperOutputDlg) {
		write_log(Preferences()->debug_gui(), "[GUI] m_ScraperOutputDlg = %i\n", m_ScraperOutputDlg);
		write_log(Preferences()->debug_gui(), "[GUI] Going to destroy existing scraper output dialog\n");

		BOOL	bWasShown = ::IsWindow(m_ScraperOutputDlg->m_hWnd) && m_ScraperOutputDlg->IsWindowVisible();
		write_log(Preferences()->debug_gui(), "[GUI] Scraper output dialog was visible: %s\n", Bool2CString(bWasShown));

    CDlgScraperOutput::DestroyWindowSafely();
		if (bWasShown) {
			write_log(Preferences()->debug_gui(), "[GUI] Scraper output dialog destroyed; going to return\n");
			return;
		}
	}	else {
		write_log(Preferences()->debug_gui(), "[GUI] Scraper output dialog does not yet exist\n");
	}
	
	MessageBox_Interactive("Please note:\n"
	  "OpenScrape scrapes everything, but OpenHoldem is optimized\n"		  
	  "to scrape only necessary info.\n"
	  "\n"
	  "For example:\n"
	  "If a players first card is \"cardback\" we don't even have to scrape the second one.\n"
	  "This is a feature, not a bug.\n",
	  "Info", 0);

	write_log(Preferences()->debug_gui(), "[GUI] Going to create scraper output dialog\n");
	m_ScraperOutputDlg = new CDlgScraperOutput(this);
	write_log(Preferences()->debug_gui(), "[GUI] Scraper output dialog: step 1 finished\n");
	m_ScraperOutputDlg->Create(CDlgScraperOutput::IDD,this);
	write_log(Preferences()->debug_gui(), "[GUI] Scraper output dialog: step 2 finished\n");
	m_ScraperOutputDlg->ShowWindow(SW_SHOW);
	write_log(Preferences()->debug_gui(), "[GUI] Scraper output dialog: step 3 finished\n");
	p_flags_toolbar->EnableButton(ID_MAIN_TOOLBAR_SCRAPER_OUTPUT, true);
	write_log(Preferences()->debug_gui(), "[GUI] Scraper output dialog: step 4 (final) finished\n"); 
}

void CMainFrame::OnViewShootreplayframe() {
	p_engine_container->symbol_engine_replayframe_controller()->ShootReplayFrameIfNotYetDone();
}

void CMainFrame::OnManualMode() {
  // Manualmode usually is in the same directory.
  // No error-checking here~> if it does not work, then we silently fail.
  // http://msdn.microsoft.com/en-us/library/windows/desktop/bb762153%28v=vs.85%29.aspx
  ShellExecute(
    NULL,               // Pointer to parent window; not needed
    "open",             // "open" == "execute" for an executable
    ManualModePath(),
		NULL, 		          // Parameters
		ToolsDirectory(), // Working directory, location of ManualMode
		SW_SHOWNORMAL);		  // Active window, default size
}

void CMainFrame::OnEditPreferences() {
	//CDlgPreferences  myDialog;
	CSAPrefsDialog dlg;

	// the "pages" (all derived from CSAPrefsSubDlg)
	CDlgSAPrefs2 page2;
	CDlgSAPrefs3 page3;
	CDlgSAPrefs4 page4;
	CDlgSAPrefs6 page6;
  CDlgSAPrefs7 page7;
	CDlgSAPrefs8 page8;
  CDlgSAPrefs9 page9;
	CDlgSAPrefs10 page10;
	CDlgSAPrefs11 page11;
	CDlgSAPrefs12 page12;
	CDlgSAPrefs13 page13;
	CDlgSAPrefs14 page14;
	CDlgSAPrefs15 page15;
	CDlgSAPrefs16 page16;
	CDlgSAPrefs17 page17;
	CDlgSAPrefs19 page19;
	CDlgSAPrefs20 page20;
	CDlgSAPrefs21 page21;
	CDlgSAPrefs22 page22;
	CDlgSAPrefs23 page23;
	CDlgSAPrefs24 page24;
	CDlgSAPrefs25 page25;
	CDlgSAPrefs26 page26;

	// add pages
	dlg.AddPage(page14, "Auto-Connector");
	dlg.AddPage(page26, "Audio");
	dlg.AddPage(page2,  "Autoplayer");
  dlg.AddPage(page9, "Auto-starter");
	dlg.AddPage(page10, "Chat");
	dlg.AddPage(page17, "Configuration Check");
	dlg.AddPage(page20, "Debugging");
	dlg.AddPage(page25, "Decimal Splitting");
	dlg.AddPage(page3,  "DLL Extension");
	dlg.AddPage(page15, "GUI");
	dlg.AddPage(page19, "Handhistory Generator");
	dlg.AddPage(page11, "Logging");
	dlg.AddPage(page23, "OCR");
	dlg.AddPage(page24, "Parallel Workers");
	dlg.AddPage(page6,  "Poker Tracker v4");
	dlg.AddPage(page22, "Popup Blocker");
	dlg.AddPage(page16, "Rebuy");
	dlg.AddPage(page8,  "Replay Frames");
	dlg.AddPage(page4,  "Scraper");
	dlg.AddPage(page13, "Stealth");
	dlg.AddPage(page21, "Table Positioner");
	dlg.AddPage(page12, "Validator");	

  // this one will be a child node on the tree
	// (&page3 specifies the parent)
	//dlg.AddPage(dlg4, "Page 4", &page3);

	// the prefs dialog title
	dlg.SetTitle("Hiss Preferences");

	// text drawn on the right side of the shaded
	// page label. this does not change when the
	// pages change, hence "constant".
	dlg.SetConstantText("Preferences");

	// launch it like any other dialog...
	//dlg1.m_csText = m_csStupidCString;
	if (dlg.DoModal()==IDOK) {
		//m_csStupidCString = dlg1.m_csText;
	}
}

// DB field for this instance's window placement: its CSessionCounter id, so multiple
// concurrently-open instances each get their own saved size/position.
static CString HissWindowField() {
	int id = (p_sessioncounter != NULL) ? p_sessioncounter->session_id() : 0;
	CString f; f.Format("%d", id);
	return f;
}

// Restore this instance's saved main-window size/position (if any, and still on a
// visible monitor). Called on startup after the window exists.
void CMainFrame::RestoreWindowPlacementFromDb() {
	if (p_tablemap_db == NULL) return;
	CString v = p_tablemap_db->GetSettingString("hiss_window", HissWindowField());
	if (v.IsEmpty()) return;
	int x = 0, y = 0, w = 0, h = 0;
	if (sscanf_s(v.GetString(), "%d,%d,%d,%d", &x, &y, &w, &h) != 4) return;
	if (w < 80 || h < 60) return;
	RECT r = { x, y, x + w, y + h };
	if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL) == NULL) return;   // off-screen -> ignore
	SetWindowPos(NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

BOOL CMainFrame::DestroyWindow() {
	RemovePanicHotkey();
	// Remember the connected table window's position+size (shared DB) BEFORE the
	// threads stop / we disconnect, so the next launch restores it instead of forcing
	// it onto monitor 1.
	if (p_table_positioner != NULL) {
		p_table_positioner->SaveCurrentPlacement();
	}
	// Save window position to the shared DB BEFORE StopThreads(): stopping the
	// heartbeat thread tears down the DB connection p_tablemap_db rides on, so any
	// write after StopThreads() silently no-ops (which is why hiss_window never
	// persisted before). The registry write is fine either way but kept here too.
	{
		WINDOWPLACEMENT wp;
		GetWindowPlacement(&wp);
		Preferences()->SetValue(k_prefs_main_x, wp.rcNormalPosition.left);
		Preferences()->SetValue(k_prefs_main_y, wp.rcNormalPosition.top);
		// Save this instance's full size+position to the shared DB, keyed by session
		// id, so each of several open instances restores its own window.
		if (p_tablemap_db != NULL) {
			CString v;
			v.Format("%d,%d,%d,%d", wp.rcNormalPosition.left, wp.rcNormalPosition.top,
				wp.rcNormalPosition.right - wp.rcNormalPosition.left,
				wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
			p_tablemap_db->SetSettingString("hiss_window", HissWindowField(), v);
		}
	}
	StopThreads();
  PMainframe()->KillTimers();
  write_log(Preferences()->debug_gui(), "[GUI] Going to delete the GUI\n");
  write_log(Preferences()->debug_gui(), "[GUI] this = [%i]\n", this);
  // All OK here
  assert(AfxCheckMemory());
  // http://www.maxinmontreal.com/forums/viewtopic.php?f=111&t=20459
  // probably caused by incorrect order of deletion,
  // caused by incorrect position of StopThreads and KillTimers.
  bool success = CFrameWnd::DestroyWindow(); 
  write_log(Preferences()->debug_gui(), "[GUI] Window deleted\n");
  return success;
}

void CMainFrame::OnFileOpen() {
  COpenHoldemDoc *pDoc = (COpenHoldemDoc *)this->GetActiveDocument();   
  if (!pDoc->SaveModified()) {
    return;        // leave the original one
  }
	CFileDialog			cfd(true);
  cfd.m_ofn.lpstrInitialDir = "";
  // http://msdn.microsoft.com/en-us/library/windows/desktop/ms646839%28v=vs.85%29.aspx
  cfd.m_ofn.lpstrFilter = "OpenHoldem Formula Files (*.ohf, *.oppl, *.txt)\0*.ohf;*.oppl;*.txt\0All files (*.*)\0*.*\0\0";
	cfd.m_ofn.lpstrTitle = "Select Formula file to OPEN";
	if (cfd.DoModal() == IDOK) {				
		pDoc->OnOpenDocument(cfd.GetPathName());
		pDoc->SetPathName(cfd.GetPathName());
		// Update window title, registry
		p_openholdem_title->UpdateTitle();
		theApp.StoreLastRecentlyUsedFileList();
	}
}

void CMainFrame::OnTimer(UINT_PTR nIDEvent) {
  write_log(Preferences()->debug_timers(), "[GUI] CMainFrame::OnTimer()\n");
  // There was a race-condition in this function during termination 
  // if OnTimer was in progress and p_autoconnector became dangling.
  // This is probably fixed, as we now kill the timers
  // before we delete singleton, but we keep these safety-meassures.
  // It is OK to skip CWnd::OnTimer(nIDEvent); if we terminate.
  if (p_flags_toolbar == NULL) {
    return;
  }
  if (p_openholdem_statusbar == NULL) {
    return;
  }
  if (p_autoconnector == NULL) {
    return;
  }
  if (nIDEvent == HWND_CHECK_TIMER) {
    write_log(Preferences()->debug_timers(), "[GUI] OnTimer checking table connection\n");
    // Important: check is_conected first.
    // Checking only garbage HWND, then disconnecting
    // can lead to freezing if it colludes with Connect()
 	  if (p_autoconnector->IsConnectedToGoneWindow()) {
 	    // Table disappeared
      write_log(Preferences()->debug_timers(), "[GUI] OnTimer found disappeared window()\n");
 	    p_autoconnector->Disconnect("table disappeared");
    }
    // Keep the PT4 HUD overlay covering the scrcpy table window (and hide it when
    // disconnected / HUD off). Cheap; runs every 200ms with the connection check.
    if (p_hud_overlay_window != NULL) {
      p_hud_overlay_window->TrackTableWindow();
    }
    // AFTER the HUD, so the decision overlay lands above it in the topmost band and the action
    // is never drawn behind a stat tile.
    if (p_hud_action_window != NULL) {
      p_hud_action_window->TrackTableWindow();
    }
 	} else if (nIDEvent == ENABLE_BUTTONS_TIMER) {
    // CTRL-held HUD opacity, refreshed on this 50ms timer so it responds immediately rather than
    // waiting for the 200ms HWND_CHECK_TIMER (see CHudOverlayWindow::RefreshCtrlAlpha).
    if (p_hud_overlay_window != NULL) {
      p_hud_overlay_window->RefreshCtrlAlpha();
    }
		// Autoplayer
		// Since OH 4.0.5 we support autoplaying immediatelly after connection
		// without the need to know the userchair to act on secondary formulas.
    write_log(Preferences()->debug_alltherest(), "[GUI] location Johnny_E\n");
		if (p_autoconnector->IsConnectedToAnything()) 	{
      write_log(Preferences()->debug_timers(), "[GUI] OnTimer enabling buttons\n");
      write_log(Preferences()->debug_alltherest(), "[GUI] location Johnny_F\n");
			p_flags_toolbar->EnableButton(ID_MAIN_TOOLBAR_AUTOPLAYER, true);
      write_log(Preferences()->debug_alltherest(), "[GUI] location Johnny_G\n");
      p_flags_toolbar->EnableButton(ID_MAIN_TOOLBAR_SHOOTFRAME, true);
      write_log(Preferences()->debug_alltherest(), "[GUI] location Johnny_L\n");
		}	else {
      write_log(Preferences()->debug_timers(), "[GUI] OnTimer disabling buttons\n");
      write_log(Preferences()->debug_alltherest(), "[GUI] location Johnny_H\n");
			p_flags_toolbar->EnableButton(ID_MAIN_TOOLBAR_AUTOPLAYER, false);
      write_log(Preferences()->debug_alltherest(), "[GUI] location Johnny_I\n");
      p_flags_toolbar->EnableButton(ID_MAIN_TOOLBAR_SHOOTFRAME, false);
      write_log(Preferences()->debug_alltherest(), "[GUI] location Johnny_N\n");
		}
    write_log(Preferences()->debug_alltherest(), "[GUI] location Johnny_O\n");
	}	else if (nIDEvent == UPDATE_STATUS_BAR_TIMER) {
    write_log(Preferences()->debug_timers(), "[GUI] OnTimer updating statusbar\n");
    write_log(Preferences()->debug_alltherest(), "[GUI] location Johnny_P\n");
		p_openholdem_statusbar->OnUpdateStatusbar();
    write_log(Preferences()->debug_alltherest(), "[GUI] location Johnny_Q\n");
    // Reload the two-successive-clicks config (edited in Vision) on tablemap change
    // AND periodically (~2s), so edits are picked up without reconnecting Hiss.
    if (p_two_successive_clicks != NULL && p_tablemap != NULL && p_tablemap->valid()) {
      CString tm = p_tablemap->filename();
      static int two_clicks_reload_tick = 0;
      if (tm != _loaded_tablemap_name || ((++two_clicks_reload_tick % 4) == 0)) {
        _loaded_tablemap_name = tm;
        p_two_successive_clicks->LoadForCurrentTablemap();
      }
    }
	}
  write_log(Preferences()->debug_alltherest(), "[GUI] location Johnny_R\n");
	CWnd::OnTimer(nIDEvent); 
}

void CMainFrame::OnAutoplayer() {
	// Route through the heartbeat request flag, exactly like OnNnDriver and OnUltra below.
	//
	// This used to call EngageAutoplayer() directly, which is why the autoplayer was the ONE mode
	// that did not survive a restart: the persist happens in the heartbeat's request handler (next to
	// ApplyNNDriverEngage / ApplyUltraEngage, which is where the other two save), and calling the
	// setter straight from here bypassed it. Every restarted instance then came up with the
	// autoplayer OFF while looking perfectly healthy -- attached, scraping, frames logging -- and
	// silently sat out every hand.
	//
	// One path for all three modes means one place that persists, one place that applies, and no
	// mode that can quietly diverge again. [Emrald: "so it is consistent"]
	extern int g_mcp_autoplayer_request;
	g_mcp_autoplayer_request = p_autoplayer->autoplayer_engaged() ? 0 : 1;
}

void CMainFrame::OnNnDriver() {
	// Toggle the NN driver via the heartbeat-applied request flag. Engaging it disengages the
	// autoplayer (and vice-versa) -- see CHeartbeatThread ApplyNNDriverEngage / CAutoplayer.
	extern bool g_nn_driver_engaged;
	extern int g_mcp_nn_driver_request;
	g_mcp_nn_driver_request = g_nn_driver_engaged ? 0 : 1;
}

void CMainFrame::OnUpdateNnDriver(CCmdUI *pCmdUI) {
	extern bool g_nn_driver_engaged;
	pCmdUI->SetCheck(g_nn_driver_engaged ? 1 : 0);   // button shows pressed while engaged
}

void CMainFrame::OnUltra() {
	// Toggle ULTRA mode (the audio-driven OHF<->NN meta-controller) via the heartbeat-applied
	// request flag -- see CHeartbeatThread ApplyUltraEngage / mcp\ultra_mode.py.
	extern bool g_ultra_engaged;
	extern int g_mcp_ultra_request;
	g_mcp_ultra_request = g_ultra_engaged ? 0 : 1;
}

void CMainFrame::OnUpdateUltra(CCmdUI *pCmdUI) {
	extern bool g_ultra_engaged;
	pCmdUI->SetCheck(g_ultra_engaged ? 1 : 0);   // button shows pressed while ULTRA is running
}

// Toolbar gear: automation preferences, served as a React page by the local terminal
// server at /automation-prefs/. Reuses CReactMappingsWindow as a generic embedded-browser
// host (see NavigateToPath) rather than cloning a second WebView2 window whose only
// difference would be the URL. Kept in its own window instance so opening preferences
// never disturbs whatever the mappings window is showing.
CReactMappingsWindow *p_react_automation_window = NULL;

void CMainFrame::OnAutomation() {
	if (p_chat_terminal_server == NULL) {
		MessageBox("Terminal API server is not running; cannot open automation preferences.",
			"Automation", MB_OK | MB_ICONERROR);
		return;
	}
	unsigned short port = p_chat_terminal_server->port();
	if (p_react_automation_window != NULL && ::IsWindow(p_react_automation_window->GetSafeHwnd())) {
		p_react_automation_window->ShowWindow(SW_SHOW);
		p_react_automation_window->SetForegroundWindow();
		p_react_automation_window->NavigateToPath(port, "/automation-prefs/");
		return;
	}
	if (p_react_automation_window != NULL) {
		delete p_react_automation_window;
		p_react_automation_window = NULL;
	}
	p_react_automation_window = new CReactMappingsWindow();
	if (!p_react_automation_window->Create(this, port)) {
		delete p_react_automation_window;
		p_react_automation_window = NULL;
		MessageBox("Could not create the automation preferences window.",
			"Automation", MB_OK | MB_ICONERROR);
		return;
	}
	// Create() navigates to /mappings/ by default; point it at the automation page.
	p_react_automation_window->NavigateToPath(port, "/automation-prefs/");
}

void CMainFrame::OnSuperstition() {
	// One-button toggle: first click launches the 666 Card Oracle in --superstition (feeds the OHF +
	// voices omens = feedback); clicking again kills it (kill switch). Applied by the heartbeat thread
	// -- see CHeartbeatThread ApplySuperstitionEngage / mcp\card_oracle.py.
	extern bool g_superstition_engaged;
	extern int g_mcp_superstition_request;
	g_mcp_superstition_request = g_superstition_engaged ? 0 : 1;
}

void CMainFrame::OnUpdateSuperstition(CCmdUI *pCmdUI) {
	extern bool g_superstition_engaged;
	pCmdUI->SetCheck(g_superstition_engaged ? 1 : 0);   // button shows pressed while superstition is live
}

void CMainFrame::OnValidator() {
	if (p_flags_toolbar->IsButtonChecked(ID_MAIN_TOOLBAR_VALIDATOR)) {
		p_flags_toolbar->CheckButton(ID_MAIN_TOOLBAR_VALIDATOR, true);
		p_validator->SetEnabledManually(true);
	}	else {
		p_flags_toolbar->CheckButton(ID_MAIN_TOOLBAR_VALIDATOR, false);
		p_validator->SetEnabledManually(false);
	}
}

void CMainFrame::OnUpdateStatus(CCmdUI *pCmdUI) {
	p_openholdem_statusbar->OnUpdateStatusbar();
}

BOOL CMainFrame::OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message) {
	if (_wait_cursor)	{
		RestoreWaitCursor();
		return TRUE;
	}
	return CFrameWnd::OnSetCursor(pWnd, nHitTest, message);
}

void CMainFrame::OnClickedFlags() {
	p_flags_toolbar->OnClickedFlags();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////
// Menu updaters

void CMainFrame::OnUpdateMenuFileNew(CCmdUI* pCmdUI) {
	pCmdUI->Enable(!p_autoplayer->autoplayer_engaged());
}

void CMainFrame::OnUpdateMenuFileOpen(CCmdUI* pCmdUI) {
	pCmdUI->Enable(!p_autoplayer->autoplayer_engaged());
}

void CMainFrame::OnUpdateMenuFileEdit(CCmdUI* pCmdUI) {
	pCmdUI->Enable(!p_autoplayer->autoplayer_engaged());
}

void CMainFrame::OnUpdateDllLoadspecificfile(CCmdUI *pCmdUI) {
	pCmdUI->Enable(!p_autoplayer->autoplayer_engaged());
}

void CMainFrame::OnUpdateViewShootreplayframe(CCmdUI *pCmdUI) {
	pCmdUI->Enable(p_autoconnector->IsConnectedToAnything());
}

void CMainFrame::OnUpdateViewScraperOutput(CCmdUI *pCmdUI) {
	pCmdUI->Enable(p_autoconnector->IsConnectedToAnything());
}

/////////////////////////////////////////////////////////////////////////////////////////////////////
// Other functions

void CMainFrame::KillTimers() { 		
  // It is very important that we kill all timers as early as possible
  // on termination. otherwise the timer-functions might access
  // objects loke the auto_connector that already are destructed,
  // thus causing a memory-access-error.
  // http://www.maxinmontreal.com/forums/viewtopic.php?f=111&t=20459
 	CFrameWnd::KillTimer(HWND_CHECK_TIMER);
  CFrameWnd::KillTimer(ENABLE_BUTTONS_TIMER);
  CFrameWnd::KillTimer(UPDATE_STATUS_BAR_TIMER);
}

void CMainFrame::ResetDisplay() {
	InvalidateRect(NULL, true); 
}

void CMainFrame::OnHelp() {
  OpenFileInExternalSoftware(OpenHoldemManualpath());
}

void CMainFrame::OnHelpOpenPPL() {
  OpenFileInExternalSoftware(OpenPPLManualpath());
}

void CMainFrame::OnHelpForums() {
	ShellExecute(NULL, "open", "http://www.maxinmontreal.com/forums/index.php", "", "", SW_SHOWDEFAULT);
}

void CMainFrame::OnHelpProblemSolver() {
	CProblemSolver my_problem_solver;
	my_problem_solver.TryToDetectBeginnersProblems();
}


CMainFrame* PMainframe() {
	CMainFrame *p_mainframe = (CMainFrame *) (theApp.m_pMainWnd);
	return p_mainframe;
}



