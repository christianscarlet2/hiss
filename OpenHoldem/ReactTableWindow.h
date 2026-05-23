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

protected:
	afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
	afx_msg void OnSize(UINT nType, int cx, int cy);

private:
	void ResizeBrowser(void);

	ICoreWebView2Controller *_controller;
	ICoreWebView2 *_webview;
	unsigned short _port;
};

extern CReactTableWindow *p_react_table_window;

#endif // INC_REACT_TABLE_WINDOW_H
