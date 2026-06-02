#include "stdafx.h"
#include "TrainerWebWindow.h"
#include "TrainerDB.h"
#include <wrl.h>
#include "WebView2.h"

using namespace Microsoft::WRL;

const int kTrainerWebWidth = 1000;
const int kTrainerWebHeight = 700;

IMPLEMENT_DYNAMIC(CTrainerWebWindow, CWnd)

BEGIN_MESSAGE_MAP(CTrainerWebWindow, CWnd)
	ON_WM_CREATE()
	ON_WM_SIZE()
	ON_WM_CLOSE()
	ON_MESSAGE(WM_EXITSIZEMOVE, &CTrainerWebWindow::OnExitSizeMove)
END_MESSAGE_MAP()

CTrainerWebWindow::CTrainerWebWindow()
{
	_owner = NULL;
	_controller = NULL;
	_webview = NULL;
	_port = 0;
	_path = "/trainer/";
}

CTrainerWebWindow::~CTrainerWebWindow()
{
	if (_webview != NULL) {
		_webview->Release();
		_webview = NULL;
	}
	if (_controller != NULL) {
		_controller->Release();
		_controller = NULL;
	}
}

BOOL CTrainerWebWindow::Create(CWnd *owner, unsigned short port, const CString &path, const CString &title)
{
	_owner = owner;
	_port = port;
	_path = path;
	// Two instances share this class (the table page and the fonts editor); key
	// their saved placement on the page so each remembers its own position/size.
	_settings_field = (path.Find("fonts") >= 0) ? "web_fonts" : "web_table";
	CString class_name = AfxRegisterWndClass(
		CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW,
		AfxGetApp()->LoadStandardCursor(IDC_ARROW),
		(HBRUSH)(COLOR_WINDOW + 1),
		::LoadIcon(NULL, IDI_APPLICATION));

	BOOL ok = CWnd::CreateEx(
		0,
		class_name,
		title,
		WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		kTrainerWebWidth,
		kTrainerWebHeight,
		owner == NULL ? NULL : owner->GetSafeHwnd(),
		NULL);
	if (ok) {
		TrainerDB_RestoreWindowRect(GetSafeHwnd(), CStringA(_settings_field));
	}
	return ok;
}

// Persist position + size once the user finishes a move/resize drag.
LRESULT CTrainerWebWindow::OnExitSizeMove(WPARAM, LPARAM)
{
	TrainerDB_SaveWindowRect(GetSafeHwnd(), CStringA(_settings_field));
	return 0;
}

// Closing HIDES the window instead of destroying it, so the window and its WebView2
// survive and the tool reopens instantly (the trainer's Open buttons just re-show it).
// We deliberately do NOT call CWnd::OnClose(), which would DestroyWindow(). The window
// is finally torn down when its owner (the main dialog) is destroyed at app exit.
void CTrainerWebWindow::OnClose()
{
	if (::IsWindow(GetSafeHwnd())) {
		TrainerDB_SaveWindowRect(GetSafeHwnd(), CStringA(_settings_field));   // remember placement
		ShowWindow(SW_HIDE);
	}
}

int CTrainerWebWindow::OnCreate(LPCREATESTRUCT lpCreateStruct)
{
	if (CWnd::OnCreate(lpCreateStruct) == -1) {
		return -1;
	}

	HWND hwnd = GetSafeHwnd();
	HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
		NULL,
		NULL,
		NULL,
		Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
			[this, hwnd](HRESULT result, ICoreWebView2Environment *environment) -> HRESULT {
				if (FAILED(result) || environment == NULL || !::IsWindow(hwnd)) {
					return S_OK;
				}
				environment->CreateCoreWebView2Controller(
					hwnd,
					Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
						[this](HRESULT controller_result, ICoreWebView2Controller *controller) -> HRESULT {
							if (FAILED(controller_result) || controller == NULL) {
								return S_OK;
							}
							_controller = controller;
							_controller->AddRef();
							_controller->get_CoreWebView2(&_webview);
							ResizeBrowser();
							NavigateToTrainer(_port);
							return S_OK;
						}).Get());
				return S_OK;
			}).Get());

	if (FAILED(hr)) {
		MessageBox("Unable to initialize Microsoft Edge WebView2.\nInstall the WebView2 Runtime.", "Tesseract Trainer", MB_OK | MB_ICONERROR);
	}
	return 0;
}

void CTrainerWebWindow::OnSize(UINT nType, int cx, int cy)
{
	CWnd::OnSize(nType, cx, cy);
	ResizeBrowser();
}

void CTrainerWebWindow::ResizeBrowser(void)
{
	if (_controller == NULL || !::IsWindow(GetSafeHwnd())) {
		return;
	}
	CRect bounds;
	GetClientRect(&bounds);
	_controller->put_Bounds(bounds);
}

void CTrainerWebWindow::NavigateToTrainer(unsigned short port)
{
	if (_webview == NULL || port == 0) {
		return;
	}
	CString url;
	url.Format("http://127.0.0.1:%u%s", port, _path.GetString());
	_webview->Navigate(CStringW(url));
}
