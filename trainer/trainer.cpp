#include "stdafx.h"
#include "trainer.h"
#include "TrainerDlg.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

CTrainerApp theApp;

CTrainerApp::CTrainerApp()
{
}

BOOL CTrainerApp::InitInstance()
{
	// COM is required by WebView2.
	::CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

	CWinApp::InitInstance();

	CTrainerDlg dlg;
	m_pMainWnd = &dlg;
	dlg.DoModal();

	::CoUninitialize();

	// Dialog closed: end the app.
	return FALSE;
}
