#ifndef INC_REACT_MAPPINGS_WINDOW_H
#define INC_REACT_MAPPINGS_WINDOW_H

#include "WebView2.h"

// Embedded-browser window that hosts the OCR name-mappings verification page
// (served by CChatTerminalServer at /mappings/). Modeled on CReactTableWindow.
class CReactMappingsWindow : public CWnd {
	DECLARE_DYNAMIC(CReactMappingsWindow)
	DECLARE_MESSAGE_MAP()

public:
	CReactMappingsWindow();
	virtual ~CReactMappingsWindow();

	BOOL Create(CWnd *owner, unsigned short port);
	void NavigateToMappings(unsigned short port);

protected:
	afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
	afx_msg void OnSize(UINT nType, int cx, int cy);

private:
	void ResizeBrowser(void);

	CWnd *_owner;
	ICoreWebView2Controller *_controller;
	ICoreWebView2 *_webview;
	unsigned short _port;
};

extern CReactMappingsWindow *p_react_mappings_window;

#endif // INC_REACT_MAPPINGS_WINDOW_H
