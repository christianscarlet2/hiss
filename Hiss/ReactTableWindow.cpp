#include "stdafx.h"
#include "ReactTableWindow.h"
#include "CopenHoldemStatusbar.h"
#include <wrl.h>
#include "WebView2.h"
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

using namespace Microsoft::WRL;
using namespace Gdiplus;

const int kReactTableWidth = 760;
const int kReactTableHeight = 560;
const int kTitleBarHeight = 38;
const int kCaptionButtonWidth = 46;

CReactTableWindow *p_react_table_window = NULL;

IMPLEMENT_DYNAMIC(CReactTableWindow, CWnd)

BEGIN_MESSAGE_MAP(CReactTableWindow, CWnd)
	ON_WM_CREATE()
	ON_WM_SIZE()
	ON_WM_NCCALCSIZE()
	ON_WM_NCHITTEST()
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONUP()
	ON_WM_MOUSEMOVE()
	ON_WM_MOUSELEAVE()
	ON_WM_GETMINMAXINFO()
	ON_WM_NCACTIVATE()
END_MESSAGE_MAP()

CReactTableWindow::CReactTableWindow()
{
	_owner = NULL;
	_controller = NULL;
	_webview = NULL;
	_port = 0;
	_hot_button = -1;
	_pressed_button = -1;
	_tracking_mouse = false;
	_gdiplus_token = 0;
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
	if (_gdiplus_token != 0) {
		GdiplusShutdown(_gdiplus_token);
		_gdiplus_token = 0;
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

	GdiplusStartupInput gdiplus_startup_input;
	GdiplusStartup(&_gdiplus_token, &gdiplus_startup_input, NULL);

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
	InvalidateTitleBar();
}

void CReactTableWindow::ResizeBrowser(void)
{
	if (_controller == NULL || !::IsWindow(GetSafeHwnd())) {
		return;
	}
	CRect bounds;
	GetClientRect(&bounds);
	bounds.top += kTitleBarHeight;   // leave room for the custom title bar
	if (bounds.top > bounds.bottom) {
		bounds.top = bounds.bottom;
	}
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

// ===== Custom GDI+ title bar =================================================

// Remove the standard caption (so we can draw our own) while keeping the side/bottom
// resize borders and the DWM shadow.
void CReactTableWindow::OnNcCalcSize(BOOL bCalcValidRects, NCCALCSIZE_PARAMS *lpncsp)
{
	if (!bCalcValidRects) {
		CWnd::OnNcCalcSize(bCalcValidRects, lpncsp);
		return;
	}
	LONG original_top = lpncsp->rgrc[0].top;
	CWnd::OnNcCalcSize(bCalcValidRects, lpncsp);   // default frame insets
	lpncsp->rgrc[0].top = original_top;            // ... but reclaim the caption strip
	if (IsZoomed()) {
		// Maximized windows need the frame padding back on top or the content is clipped.
		int frame_y = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
		lpncsp->rgrc[0].top = original_top + frame_y;
	}
}

LRESULT CReactTableWindow::OnNcHitTest(CPoint point)
{
	CPoint client_pt = point;
	ScreenToClient(&client_pt);
	CRect client;
	GetClientRect(&client);

	// Top resize edge / corners (only when not maximized).
	if (!IsZoomed() && client_pt.y >= 0 && client_pt.y < 6
			&& client_pt.x >= 0 && client_pt.x < client.Width()) {
		if (client_pt.x < 10) return HTTOPLEFT;
		if (client_pt.x >= client.Width() - 10) return HTTOPRIGHT;
		return HTTOP;
	}

	// Title bar: buttons are client (so we get clicks); the rest is draggable caption.
	if (client_pt.y >= 0 && client_pt.y < kTitleBarHeight
			&& client_pt.x >= 0 && client_pt.x < client.Width()) {
		if (HitButton(client_pt) >= 0) {
			return HTCLIENT;
		}
		return HTCAPTION;
	}

	return CWnd::OnNcHitTest(point);
}

BOOL CReactTableWindow::OnEraseBkgnd(CDC *pDC)
{
	return TRUE;   // painted in OnPaint; WebView2 covers the rest
}

BOOL CReactTableWindow::OnNcActivate(BOOL bActive)
{
	InvalidateTitleBar();
	return TRUE;   // skip the default caption repaint (we have none)
}

void CReactTableWindow::GetButtonRect(int index, CRect *rect)
{
	CRect client;
	GetClientRect(&client);
	int from_right = 2 - index;   // index 2 (close) is right-most
	int right = client.right - from_right * kCaptionButtonWidth;
	int left = right - kCaptionButtonWidth;
	rect->SetRect(left, 0, right, kTitleBarHeight);
}

int CReactTableWindow::HitButton(CPoint client_pt)
{
	if (client_pt.y < 0 || client_pt.y >= kTitleBarHeight) {
		return -1;
	}
	for (int i = 0; i < 3; ++i) {
		CRect br;
		GetButtonRect(i, &br);
		if (br.PtInRect(client_pt)) {
			return i;
		}
	}
	return -1;
}

void CReactTableWindow::InvalidateTitleBar(void)
{
	if (!::IsWindow(GetSafeHwnd())) {
		return;
	}
	CRect client;
	GetClientRect(&client);
	CRect strip(0, 0, client.Width(), kTitleBarHeight);
	InvalidateRect(strip, FALSE);
}

void CReactTableWindow::OnPaint()
{
	CPaintDC dc(this);
	CRect client;
	GetClientRect(&client);
	int width = client.Width();
	int H = kTitleBarHeight;

	Graphics g(dc.GetSafeHdc());
	g.SetSmoothingMode(SmoothingModeAntiAlias);
	g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

	// Fill the whole client dark (so it looks right before the WebView2 paints).
	SolidBrush base(Color(255, 14, 16, 20));
	g.FillRectangle(&base, 0, 0, width, client.Height());

	// Title bar gradient + scarlet underline.
	LinearGradientBrush bar(Point(0, 0), Point(0, H), Color(255, 26, 29, 35), Color(255, 13, 15, 19));
	g.FillRectangle(&bar, 0, 0, width, H);
	SolidBrush underline(Color(255, 0x9a, 0x16, 0x22));
	g.FillRectangle(&underline, 0, H - 2, width, 2);
	SolidBrush underglow(Color(70, 0xd1, 0x1f, 0x33));
	g.FillRectangle(&underglow, 0, H - 4, width, 2);

	// Scarlet diamond mark.
	SolidBrush mark(Color(255, 0xd1, 0x1f, 0x33));
	REAL midy = (REAL)H / 2.0f;
	PointF diamond[4] = { PointF(8, midy), PointF(14, midy - 6), PointF(20, midy), PointF(14, midy + 6) };
	g.FillPolygon(&mark, diamond, 4);

	// Title text.
	FontFamily font_family(L"Segoe UI");
	Font font(&font_family, 12.5f, FontStyleBold, UnitPixel);
	SolidBrush text_brush(Color(255, 0xee, 0xe6, 0xd2));
	StringFormat sf;
	sf.SetLineAlignment(StringAlignmentCenter);
	sf.SetFormatFlags(StringFormatFlagsNoWrap);
	RectF text_rect(28.0f, 0.0f, (REAL)(width - 28 - 3 * kCaptionButtonWidth - 8), (REAL)H);
	g.DrawString(L"Hiss React Table Display", -1, &font, text_rect, &sf, &text_brush);

	// Caption buttons.
	for (int i = 0; i < 3; ++i) {
		CRect br;
		GetButtonRect(i, &br);
		bool hot = (_hot_button == i);
		bool pressed = (_pressed_button == i);
		if (hot) {
			Color bg;
			if (i == 2) {
				bg = pressed ? Color(255, 0xa8, 0x12, 0x26) : Color(255, 0xd1, 0x1f, 0x33);
			} else {
				bg = pressed ? Color(80, 255, 255, 255) : Color(40, 255, 255, 255);
			}
			SolidBrush hb(bg);
			g.FillRectangle(&hb, br.left, br.top, br.Width(), br.Height());
		}
		Color glyph = (hot && i == 2) ? Color(255, 255, 255, 255) : Color(255, 0xcf, 0xc9, 0xb8);
		Pen pen(glyph, 1.4f);
		int cx = br.left + br.Width() / 2;
		int cy = br.top + br.Height() / 2;
		if (i == 0) {
			g.DrawLine(&pen, cx - 6, cy, cx + 6, cy);                 // minimize
		} else if (i == 1) {
			if (IsZoomed()) {                                        // restore (two squares)
				g.DrawRectangle(&pen, cx - 5, cy - 2, 7, 7);
				g.DrawRectangle(&pen, cx - 2, cy - 5, 7, 7);
			} else {
				g.DrawRectangle(&pen, cx - 5, cy - 5, 10, 10);       // maximize
			}
		} else {
			g.DrawLine(&pen, cx - 5, cy - 5, cx + 5, cy + 5);        // close (X)
			g.DrawLine(&pen, cx + 5, cy - 5, cx - 5, cy + 5);
		}
	}
}

void CReactTableWindow::OnLButtonDown(UINT nFlags, CPoint point)
{
	int hit = HitButton(point);
	if (hit >= 0) {
		_pressed_button = hit;
		SetCapture();
		InvalidateTitleBar();
		return;
	}
	CWnd::OnLButtonDown(nFlags, point);
}

void CReactTableWindow::OnLButtonUp(UINT nFlags, CPoint point)
{
	if (_pressed_button >= 0) {
		int pressed = _pressed_button;
		_pressed_button = -1;
		ReleaseCapture();
		InvalidateTitleBar();
		if (HitButton(point) == pressed) {
			DoButtonAction(pressed);
		}
		return;
	}
	CWnd::OnLButtonUp(nFlags, point);
}

void CReactTableWindow::OnMouseMove(UINT nFlags, CPoint point)
{
	int hit = HitButton(point);
	if (hit != _hot_button) {
		_hot_button = hit;
		InvalidateTitleBar();
	}
	if (!_tracking_mouse) {
		TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, GetSafeHwnd(), 0 };
		TrackMouseEvent(&tme);
		_tracking_mouse = true;
	}
	CWnd::OnMouseMove(nFlags, point);
}

void CReactTableWindow::OnMouseLeave()
{
	_tracking_mouse = false;
	if (_hot_button != -1) {
		_hot_button = -1;
		InvalidateTitleBar();
	}
	CWnd::OnMouseLeave();
}

void CReactTableWindow::DoButtonAction(int index)
{
	if (index == 0) {
		ShowWindow(SW_MINIMIZE);
	} else if (index == 1) {
		ShowWindow(IsZoomed() ? SW_RESTORE : SW_MAXIMIZE);
	} else if (index == 2) {
		ShowWindow(SW_HIDE);
	}
}

void CReactTableWindow::OnGetMinMaxInfo(MINMAXINFO *lpMMI)
{
	HMONITOR monitor = MonitorFromWindow(GetSafeHwnd(), MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi = { sizeof(MONITORINFO) };
	if (GetMonitorInfo(monitor, &mi)) {
		CRect work = mi.rcWork;
		CRect mon = mi.rcMonitor;
		lpMMI->ptMaxPosition.x = work.left - mon.left;
		lpMMI->ptMaxPosition.y = work.top - mon.top;
		lpMMI->ptMaxSize.x = work.Width();
		lpMMI->ptMaxSize.y = work.Height();
	}
	lpMMI->ptMinTrackSize.x = 420;
	lpMMI->ptMinTrackSize.y = 280;
	CWnd::OnGetMinMaxInfo(lpMMI);
}
