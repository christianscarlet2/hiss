#include "stdafx.h"
#include "ReactTableWindow.h"
#include "CopenHoldemStatusbar.h"
#include <wrl.h>
#include "WebView2.h"

using namespace Microsoft::WRL;

const int kReactTableWidth = 760;
const int kReactTableHeight = 560;

CReactTableWindow *p_react_table_window = NULL;

IMPLEMENT_DYNAMIC(CReactTableWindow, CWnd)

BEGIN_MESSAGE_MAP(CReactTableWindow, CWnd)
	ON_WM_CREATE()
	ON_WM_SIZE()
END_MESSAGE_MAP()

CReactTableWindow::CReactTableWindow()
{
	_owner = NULL;
	_controller = NULL;
	_webview = NULL;
	_port = 0;
}

CReactTableWindow::~CReactTableWindow()
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

BOOL CReactTableWindow::Create(CWnd *owner, unsigned short port)
{
	_owner = owner;
	_port = port;
	if (p_openholdem_statusbar != NULL) {
		p_openholdem_statusbar->SetTableViewLoading();
	}
	CString class_name = AfxRegisterWndClass(
		CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW,
		AfxGetApp()->LoadStandardCursor(IDC_ARROW),
		(HBRUSH)(COLOR_WINDOW + 1),
		::LoadIcon(NULL, IDI_APPLICATION));

	BOOL created = CWnd::CreateEx(
		WS_EX_TOOLWINDOW,
		class_name,
		"Hiss React Table Display",
		WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		kReactTableWidth,
		kReactTableHeight,
		owner == NULL ? NULL : owner->GetSafeHwnd(),
		NULL);
	if (created) {
		AttachToOwner();
	}
	return created;
}

int CReactTableWindow::OnCreate(LPCREATESTRUCT lpCreateStruct)
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
							if (_webview != NULL) {
								EventRegistrationToken navigation_completed_token = {};
								_webview->add_NavigationCompleted(
									Callback<ICoreWebView2NavigationCompletedEventHandler>(
										[](ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
											BOOL success = FALSE;
											if (args != NULL) {
												args->get_IsSuccess(&success);
											}
											if (success && p_openholdem_statusbar != NULL) {
												p_openholdem_statusbar->SetTableViewReady();
											}
											return S_OK;
										}).Get(),
									&navigation_completed_token);
							}
							ResizeBrowser();
							NavigateToDisplay(_port);
							return S_OK;
						}).Get());
				return S_OK;
			}).Get());

	if (FAILED(hr)) {
		MessageBox("Unable to initialize Microsoft Edge WebView2.", "Hiss React Table Display", MB_OK | MB_ICONERROR);
	}
	return 0;
}

void CReactTableWindow::OnSize(UINT nType, int cx, int cy)
{
	CWnd::OnSize(nType, cx, cy);
	ResizeBrowser();
}

void CReactTableWindow::ResizeBrowser(void)
{
	if (_controller == NULL || !::IsWindow(GetSafeHwnd())) {
		return;
	}
	CRect bounds;
	GetClientRect(&bounds);
	_controller->put_Bounds(bounds);
}

void CReactTableWindow::AttachToOwner(void)
{
	if (_owner == NULL || !::IsWindow(_owner->GetSafeHwnd()) || !::IsWindow(GetSafeHwnd())) {
		return;
	}

	CRect owner_rect, rect;
	_owner->GetWindowRect(&owner_rect);
	GetWindowRect(&rect);
	int x = owner_rect.right - rect.Width();
	int y = owner_rect.bottom;
	SetWindowPos(NULL, x, y, rect.Width(), rect.Height(), SWP_NOZORDER | SWP_NOACTIVATE);
}

void CReactTableWindow::NavigateToDisplay(unsigned short port)
{
	if (_webview == NULL || port == 0) {
		return;
	}

	CString url;
	url.Format("http://127.0.0.1:%u/table-display/", port);
	if (p_openholdem_statusbar != NULL) {
		p_openholdem_statusbar->SetTableViewLoading();
	}
	_webview->Navigate(CStringW(url));
}
