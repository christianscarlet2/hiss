#ifndef INC_MAINFRM_H
#define INC_MAINFRM_H

#include "..\CTablemap\CTablemap.h"
#include "..\Shared\CCritSec\CCritSec.h"

#define HWND_CHECK_TIMER				1
#define ENABLE_BUTTONS_TIMER    2
#define UPDATE_STATUS_BAR_TIMER 3

// Scarlet Beast menu command IDs
#define WM_SB_INITMENU  (WM_USER + 0x4201)
#define IDM_SB_CONNECT  0x8801
#define IDM_SB_SETKEY   0x8802
#define IDM_SB_SETTABLE 0x8803
#define IDM_SB_TEST     0x8804
#define IDM_SB_LOBBY    0x8805
#define IDM_SB_GOOGLE  0x8806
#define IDM_SB_AUTOREBUY 0x8807
#define IDM_SB_LOADHUD   0x8808

class CMainFrame : public CFrameWnd 
{
protected: // create from serialization only
	DECLARE_DYNCREATE(CMainFrame)
	DECLARE_MESSAGE_MAP()

	CMainFrame();
	afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
 public:
	// Global panic hotkey (Win+H+Q from any window): broadcasts to every running
	// Hiss instance and shuts off the autoplayer on all of them.
	afx_msg LRESULT OnPanicAutoplayerOff(WPARAM wParam, LPARAM lParam);
 public:
  void AppendScarletBeastMenu();
  afx_msg LRESULT OnSbInitMenu(WPARAM wParam, LPARAM lParam);
  afx_msg void OnSbConnect();
  afx_msg void OnSbSetKey();
  afx_msg void OnSbSetTable();
  afx_msg void OnSbTest();
  afx_msg void OnSbLobby();
  afx_msg void OnSbGoogle();
  afx_msg void OnSbAutoRebuy();
  afx_msg void OnSbLoadHud();
 public:
	afx_msg void OnFileOpen();
	afx_msg void OnEditFormula();
	afx_msg void OnEditForceuserchair();
	afx_msg void OnEditViewLog();
	afx_msg void OnEditTagLog();
  afx_msg void OnEditClearLog();
	afx_msg void OnEditVerifyNameMappings();
	afx_msg void OnScraperOutput();
	afx_msg void OnViewShootreplayframe();
  afx_msg void OnManualMode();
	afx_msg void OnEditPreferences();
	afx_msg void OnFileLoadTableMap();
	afx_msg void OnTimer(UINT_PTR nIDEvent);
	afx_msg void OnMove(int x, int y);
	afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
	afx_msg void OnUpdateStatus(CCmdUI *pCmdUI);
	afx_msg void OnAutoplayer();
	afx_msg void OnNnDriver();
	afx_msg void OnUpdateNnDriver(CCmdUI *pCmdUI);
	afx_msg void OnUltra();
	afx_msg void OnUpdateUltra(CCmdUI *pCmdUI);
	afx_msg void OnSuperstition();
	afx_msg void OnAutomation();
	afx_msg void OnUpdateSuperstition(CCmdUI *pCmdUI);
	afx_msg void OnValidator();
	afx_msg void OnClickedFlags();

	afx_msg void OnUpdateMenuFileNew(CCmdUI* pCmdUI);
	afx_msg void OnUpdateMenuFileOpen(CCmdUI* pCmdUI);
  afx_msg void OnUpdateMenuFileEdit(CCmdUI* pCmdUI);
	afx_msg void OnUpdateMenuFileLoadProfile(CCmdUI* pCmdUI);
	afx_msg void OnUpdateDllLoadspecificfile(CCmdUI *pCmdUI);
	afx_msg void OnUpdateViewMainToolbar(CCmdUI *pCmdUI);
	afx_msg void OnUpdateViewStatusbar(CCmdUI *pCmdUI);
	afx_msg void OnUpdateViewShootreplayframe(CCmdUI *pCmdUI);
  afx_msg void OnUpdateViewScraperOutput(CCmdUI *pCmdUI);
	afx_msg void OnUpdateEditForceuserchair(CCmdUI *pCmdUI);

	afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);
	afx_msg void OnHelpProblemSolver();
public:
	afx_msg void OnHelp();
	afx_msg void OnHelpOpenPPL();
	afx_msg void OnHelpForums();
public:
  void KillTimers();
public:
	virtual BOOL PreCreateWindow(CREATESTRUCT& cs);
	virtual ~CMainFrame();
	virtual BOOL DestroyWindow();
	// Per-instance main-window placement, keyed by CSessionCounter id, in the shared
	// hiss DB ("hiss_window"/<id>). Each running instance keeps its own size/position.
	void RestoreWindowPlacementFromDb();
	CString	_exec_filename;
public:
	// public accessors
	const bool wait_cursor() { return _wait_cursor; }
public:
	void DisableButtonsOnConnect();
	void EnableButtonsOnDisconnect();
	void UnattachOHFromPokerWindow();
public:
	void ResetDisplay();
public:
#define ENT CSLock lock(m_critsec);
	// public mutators
	void set_wait_cursor(const bool b) { ENT _wait_cursor = b; }
#undef ENT

private:
	// private variables - use public accessors and public mutators to address these
	bool			_wait_cursor;	 // Used if we need to display a wait cursor anywhere
	bool			_always_on_top = false;	// system-menu "Always on Top" toggle

private:
	// private functions and variables - not available via accessors or mutators
	int CreateStatusBar(void);

	// Global Win+H+Q panic hotkey (low-level keyboard hook).
	void InstallPanicHotkey();
	void RemovePanicHotkey();
	static HHOOK s_panic_kbd_hook;
	static LRESULT CALLBACK PanicKeyboardProc(int code, WPARAM wParam, LPARAM lParam);

	RECT			_prev_att_rect, _prev_wrect;

	CCritSec		m_critsec;

	// Tracks the loaded tablemap so the two-successive-clicks config (edited in
	// Vision) is reloaded whenever the map changes.
	CString		_loaded_tablemap_name;
};

// used by EnumProcTopLevelWindowList function
extern CArray <STableList, STableList>		g_tlist; 

BOOL CALLBACK EnumProcTopLevelWindowList(HWND hwnd, LPARAM lparam);

// Accessor function because p_mainframe never gets initialized.
CMainFrame* PMainframe();

#endif /* INC_MAINFRM_H */
