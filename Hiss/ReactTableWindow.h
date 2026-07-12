#ifndef INC_REACT_TABLE_WINDOW_H
#define INC_REACT_TABLE_WINDOW_H

#include "WebView2.h"

class CReactTableWindow : public CWnd {
	DECLARE_DYNAMIC(CReactTableWindow)
	DECLARE_MESSAGE_MAP()

public:
	CReactTableWindow();
	virtual ~CReactTableWindow();

	BOOL Create(CWnd *owner, unsigned short port);
	void NavigateToDisplay(unsigned short port);
	void AttachToOwner(void);
	bool IsDocked() const { return _docked; }

protected:
	afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
	afx_msg void OnDestroy();
	afx_msg void OnSize(UINT nType, int cx, int cy);
	afx_msg void OnMoving(UINT fwSide, LPRECT pRect);
	afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
	// Custom GDI+ title bar (the standard caption is removed via WM_NCCALCSIZE).
	afx_msg void OnNcCalcSize(BOOL bCalcValidRects, NCCALCSIZE_PARAMS *lpncsp);
	afx_msg LRESULT OnNcHitTest(CPoint point);
	afx_msg void OnNcRButtonUp(UINT nHitTest, CPoint point);   // custom title-bar context menu
	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC *pDC);
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg void OnMouseLeave();
	afx_msg void OnGetMinMaxInfo(MINMAXINFO *lpMMI);
	afx_msg BOOL OnNcActivate(BOOL bActive);
	afx_msg void OnTimer(UINT_PTR nIDEvent);

private:
	void ResizeBrowser(void);
	void GetButtonRect(int index, CRect *rect);   // client coords; 0=min 1=max 2=close
	int  HitButton(CPoint client_pt);              // -1 if none
	void DoButtonAction(int index);
	void InvalidateChrome(void);
	// Menu bar (File / Edit / Problem Solver / Help) + toolbar copied from the main window.
	void GetMenuRect(int index, CRect *rect);      // client coords
	int  HitMenu(CPoint client_pt);                // -1 if none
	void GetToolRect(int index, CRect *rect);      // client coords
	int  HitTool(CPoint client_pt);                // -1 if none
	void OpenTopMenu(int index);                   // pop up a top-level menu
	void ForwardCommand(unsigned int command_id);  // route a command to the main frame
	int  ChromeHeight(void);

	CWnd *_owner;
	ICoreWebView2Controller *_controller;
	ICoreWebView2 *_webview;
	unsigned short _port;
	int _hot_button;
	int _pressed_button;
	int _hot_menu;
	int _hot_tool;
	bool _tracking_mouse;
	ULONG_PTR _gdiplus_token;
	CString _title;   // mirrors the main window's caption; refreshed by a timer
	bool _docked;          // glued to the owner's side vs detached
	bool _always_on_top;   // system-menu "Always on Top" toggle
};

extern CReactTableWindow *p_react_table_window;

#endif // INC_REACT_TABLE_WINDOW_H
