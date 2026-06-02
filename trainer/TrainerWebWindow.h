#pragma once

#include "WebView2.h"

// Embedded-browser window hosting the trainer table page (served by
// CTrainerServer at /trainer/). Modeled on Hiss's CReactMappingsWindow.
class CTrainerWebWindow : public CWnd {
	DECLARE_DYNAMIC(CTrainerWebWindow)
	DECLARE_MESSAGE_MAP()

public:
	CTrainerWebWindow();
	virtual ~CTrainerWebWindow();

	// path: server-relative page, e.g. "/trainer/" or "/trainer/fonts.html".
	BOOL Create(CWnd *owner, unsigned short port, const CString &path = "/trainer/",
		const CString &title = "Tesseract Trainer");
	void NavigateToTrainer(unsigned short port);

protected:
	afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
	afx_msg void OnSize(UINT nType, int cx, int cy);
	afx_msg LRESULT OnExitSizeMove(WPARAM wParam, LPARAM lParam);

private:
	void ResizeBrowser(void);

	CWnd *_owner;
	ICoreWebView2Controller *_controller;
	ICoreWebView2 *_webview;
	unsigned short _port;
	CString _path;
	CString _settings_field;   // window-placement settings field (distinguishes the two instances)
};
