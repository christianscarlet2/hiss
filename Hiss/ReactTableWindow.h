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

protected:
	afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
	afx_msg void OnSize(UINT nType, int cx, int cy);
	// Custom GDI+ title bar (the standard caption is removed via WM_NCCALCSIZE).
	afx_msg void OnNcCalcSize(BOOL bCalcValidRects, NCCALCSIZE_PARAMS *lpncsp);
	afx_msg LRESULT OnNcHitTest(CPoint point);
	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC *pDC);
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg void OnMouseLeave();
	afx_msg void OnGetMinMaxInfo(MINMAXINFO *lpMMI);
	afx_msg BOOL OnNcActivate(BOOL bActive);

private:
	void ResizeBrowser(void);
	void GetButtonRect(int index, CRect *rect);   // client coords; 0=min 1=max 2=close
	int  HitButton(CPoint client_pt);              // -1 if none
	void DoButtonAction(int index);
	void InvalidateTitleBar(void);

	CWnd *_owner;
	ICoreWebView2Controller *_controller;
	ICoreWebView2 *_webview;
	unsigned short _port;
	int _hot_button;
	int _pressed_button;
	bool _tracking_mouse;
	ULONG_PTR _gdiplus_token;
};

extern CReactTableWindow *p_react_table_window;

#endif // INC_REACT_TABLE_WINDOW_H
