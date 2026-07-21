#include "stdafx.h"
#include "ReactMappingsWindow.h"
#include <wrl.h>
#include "WebView2.h"

using namespace Microsoft::WRL;

const int kReactMappingsWidth = 900;
const int kReactMappingsHeight = 600;

CReactMappingsWindow *p_react_mappings_window = NULL;

IMPLEMENT_DYNAMIC(CReactMappingsWindow, CWnd)

BEGIN_MESSAGE_MAP(CReactMappingsWindow, CWnd)
	ON_WM_CREATE()
	ON_WM_SIZE()
END_MESSAGE_MAP()

CReactMappingsWindow::CReactMappingsWindow()
{
	_owner = NULL;
	_controller = NULL;
	_webview = NULL;
	_port = 0;
}

CReactMappingsWindow::~CReactMappingsWindow()
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

BOOL CReactMappingsWindow::Create(CWnd *owner, unsigned short port)
{
	_owner = owner;
	_port = port;
	CString class_name = AfxRegisterWndClass(
		CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW,
		AfxGetApp()->LoadStandardCursor(IDC_ARROW),
		(HBRUSH)(COLOR_WINDOW + 1),
		::LoadIcon(NULL, IDI_APPLICATION));

	return CWnd::CreateEx(
		WS_EX_TOOLWINDOW,
		class_name,
		"Hiss — OCR Name Mappings",
		WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		kReactMappingsWidth,
		kReactMappingsHeight,
		owner == NULL ? NULL : owner->GetSafeHwnd(),
		NULL);
}

int CReactMappingsWindow::OnCreate(LPCREATESTRUCT lpCreateStruct)
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
							NavigateToMappings(_port);
							return S_OK;
						}).Get());
				return S_OK;
			}).Get());

	if (FAILED(hr)) {
		MessageBox("Unable to initialize Microsoft Edge WebView2.", "Hiss — OCR Name Mappings", MB_OK | MB_ICONERROR);
	}
	return 0;
}

void CReactMappingsWindow::OnSize(UINT nType, int cx, int cy)
{
	CWnd::OnSize(nType, cx, cy);
	ResizeBrowser();
}

void CReactMappingsWindow::ResizeBrowser(void)
{
	if (_controller == NULL || !::IsWindow(GetSafeHwnd())) {
		return;
	}
	CRect bounds;
	GetClientRect(&bounds);
	_controller->put_Bounds(bounds);
}

// Navigate this embedded browser to any path on the local terminal server.
void CReactMappingsWindow::NavigateToPath(unsigned short port, const CString &path) {
	if (_webview == NULL) {
		return;
	}
	CString url;
	url.Format("http://127.0.0.1:%u%s", port, path.GetString());
	CStringW urlw(url);
	_webview->Navigate(urlw);
}

void CReactMappingsWindow::NavigateToMappings(unsigned short port)
{
	if (_webview == NULL || port == 0) {
		return;
	}
	CString url;
	url.Format("http://127.0.0.1:%u/mappings/", port);
	_webview->Navigate(CStringW(url));
}
