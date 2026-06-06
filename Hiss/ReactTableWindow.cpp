#include "stdafx.h"
#include "ReactTableWindow.h"
#include "CopenHoldemStatusbar.h"
#include <wrl.h>
#include "WebView2.h"
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include "resource.h"

using namespace Microsoft::WRL;
using namespace Gdiplus;

const int kReactTableWidth = 760;
const int kReactTableHeight = 560;
const int kTitleBarHeight = 34;     // row 1: title + window buttons
const int kMenuRowHeight = 30;      // row 2: menu bar + toolbar
const int kCaptionButtonWidth = 46;

// Top-level menus mirrored from IDR_MAINFRAME. submenu_index = position of the popup in
// IDR_MAINFRAME (File=0, Edit=1, Help=3); Problem Solver has no popup -> a direct command.
struct SMenuTop { const wchar_t *label; int submenu_index; UINT direct_command; int width; };
static const SMenuTop kMenuTops[] = {
	{ L"File",            0, 0,                     44 },
	{ L"Edit",            1, 0,                     44 },
	{ L"Problem Solver", -1, ID_HELP_PROBLEMSOLVER, 104 },
	{ L"Help",            3, 0,                     46 },
};
static const int kNumMenuTops = sizeof(kMenuTops) / sizeof(kMenuTops[0]);

// Toolbar buttons mirrored from the IDR_MAINFRAME toolbar (-1 = separator). Each non-
// separator entry draws a Segoe MDL2 Assets glyph and forwards its command to the main frame.
struct SToolItem { int command; const wchar_t *glyph; bool scarlet; };
static const SToolItem kToolItems[] = {
	{ ID_FILE_NEW,                     L"\xE7C3", false },  // page
	{ ID_FILE_OPEN,                    L"\xE838", false },  // folder open
	{ ID_FILE_SAVE,                    L"\xE74E", false },  // save
	{ -1, L"", false },
	{ ID_MAIN_TOOLBAR_AUTOPLAYER,      L"\xE768", true  },  // play (scarlet flair)
	{ ID_MAIN_TOOLBAR_FORMULA,         L"\xE8EF", false },  // calculator / formula
	{ ID_MAIN_TOOLBAR_VALIDATOR,       L"\xE73E", false },  // checkmark
	{ ID_MAIN_TOOLBAR_TAGLOGFILE,      L"\xE8EC", false },  // tag
	{ -1, L"", false },
	{ ID_MAIN_TOOLBAR_SCRAPER_OUTPUT,  L"\xE890", false },  // view / eye
	{ ID_MAIN_TOOLBAR_SHOOTFRAME,      L"\xE722", false },  // camera
	{ ID_MAIN_TOOLBAR_MANUALMODE,      L"\xE815", false },  // touch pointer
	{ -1, L"", false },
	{ ID_MAIN_TOOLBAR_HELP,            L"\xE897", true  },  // help (scarlet flair)
};
static const int kNumToolItems = sizeof(kToolItems) / sizeof(kToolItems[0]);
static const int kToolButtonWidth = 30;
static const int kToolSeparatorWidth = 12;
static const int kMenuStartX = 8;

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
	ON_WM_TIMER()
END_MESSAGE_MAP()

CReactTableWindow::CReactTableWindow()
{
	_owner = NULL;
	_controller = NULL;
	_webview = NULL;
	_port = 0;
	_hot_button = -1;
	_pressed_button = -1;
	_hot_menu = -1;
	_hot_tool = -1;
	_tracking_mouse = false;
	_gdiplus_token = 0;
	_title = "Hiss:";
}

static const UINT_PTR kTitleSyncTimer = 0x52544D54;   // refresh the mirrored title

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
		"Hiss:",
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
	SetTimer(kTitleSyncTimer, 400, NULL);   // keep the title in sync with the main window

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
	InvalidateChrome();
}

void CReactTableWindow::ResizeBrowser(void)
{
	if (_controller == NULL || !::IsWindow(GetSafeHwnd())) {
		return;
	}
	CRect bounds;
	GetClientRect(&bounds);
	bounds.top += ChromeHeight();   // leave room for the title bar + menu/toolbar row
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

	// Title bar (row 1): buttons are client (so we get clicks); the rest is draggable caption.
	if (client_pt.y >= 0 && client_pt.y < kTitleBarHeight
			&& client_pt.x >= 0 && client_pt.x < client.Width()) {
		if (HitButton(client_pt) >= 0) {
			return HTCLIENT;
		}
		return HTCAPTION;
	}

	// Menu/toolbar (row 2): all client so the menus and toolbar buttons receive clicks.
	if (client_pt.y >= kTitleBarHeight && client_pt.y < ChromeHeight()
			&& client_pt.x >= 0 && client_pt.x < client.Width()) {
		return HTCLIENT;
	}

	return CWnd::OnNcHitTest(point);
}

BOOL CReactTableWindow::OnEraseBkgnd(CDC *pDC)
{
	return TRUE;   // painted in OnPaint; WebView2 covers the rest
}

BOOL CReactTableWindow::OnNcActivate(BOOL bActive)
{
	InvalidateChrome();
	return TRUE;   // skip the default caption repaint (we have none)
}

// Mirror the main Hiss window's caption (it changes dynamically); repaint only when it does.
void CReactTableWindow::OnTimer(UINT_PTR nIDEvent)
{
	if (nIDEvent == kTitleSyncTimer) {
		CString current = "Hiss:";
		if (_owner != NULL && ::IsWindow(_owner->GetSafeHwnd())) {
			_owner->GetWindowText(current);
		}
		if (current.IsEmpty()) {
			current = "Hiss:";
		}
		if (current != _title) {
			_title = current;
			CRect client;
			GetClientRect(&client);
			CRect title_strip(0, 0, client.Width(), kTitleBarHeight);
			InvalidateRect(title_strip, FALSE);
		}
		return;
	}
	CWnd::OnTimer(nIDEvent);
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

int CReactTableWindow::ChromeHeight(void)
{
	return kTitleBarHeight + kMenuRowHeight;
}

void CReactTableWindow::InvalidateChrome(void)
{
	if (!::IsWindow(GetSafeHwnd())) {
		return;
	}
	CRect client;
	GetClientRect(&client);
	CRect strip(0, 0, client.Width(), ChromeHeight());
	InvalidateRect(strip, FALSE);
}

void CReactTableWindow::OnPaint()
{
	CPaintDC dc(this);
	CRect client;
	GetClientRect(&client);
	int width = client.Width();
	int H = kTitleBarHeight;
	int chrome = ChromeHeight();
	if (width <= 0) {
		return;
	}

	Graphics screen(dc.GetSafeHdc());
	// Below the chrome stays dark until the WebView2 covers it (single blit, no flicker).
	SolidBrush base(Color(255, 14, 16, 20));
	if (client.Height() > chrome) {
		screen.FillRectangle(&base, 0, chrome, width, client.Height() - chrome);
	}

	// Render the whole chrome into an offscreen buffer, then blit it once. This is what
	// kills the hover flicker (the chrome was previously drawn straight to the window).
	Bitmap buffer(width, chrome, PixelFormat32bppPARGB);
	Graphics g(&buffer);
	g.SetSmoothingMode(SmoothingModeAntiAlias);
	g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

	// ---- Row 1: title bar ----
	LinearGradientBrush bar(Point(0, 0), Point(0, H), Color(255, 28, 31, 37), Color(255, 15, 17, 21));
	g.FillRectangle(&bar, 0, 0, width, H);

	SolidBrush mark(Color(255, 0xd1, 0x1f, 0x33));
	REAL midy = (REAL)H / 2.0f;
	PointF diamond[4] = { PointF(8, midy), PointF(14, midy - 5), PointF(20, midy), PointF(14, midy + 5) };
	g.FillPolygon(&mark, diamond, 4);

	FontFamily ui(L"Segoe UI");
	Font title_font(&ui, 12.5f, FontStyleBold, UnitPixel);
	SolidBrush title_brush(Color(255, 0xee, 0xe6, 0xd2));
	StringFormat sf_l;
	sf_l.SetLineAlignment(StringAlignmentCenter);
	sf_l.SetFormatFlags(StringFormatFlagsNoWrap);
	RectF title_rect(28.0f, 0.0f, (REAL)(width - 28 - 3 * kCaptionButtonWidth - 8), (REAL)H);
	CStringW wtitle(_title);
	g.DrawString(wtitle, wtitle.GetLength(), &title_font, title_rect, &sf_l, &title_brush);

	for (int i = 0; i < 3; ++i) {
		CRect br;
		GetButtonRect(i, &br);
		bool hot = (_hot_button == i);
		bool pressed = (_pressed_button == i);
		if (hot) {
			Color bg = (i == 2) ? (pressed ? Color(255, 0xa8, 0x12, 0x26) : Color(255, 0xd1, 0x1f, 0x33))
				: (pressed ? Color(80, 255, 255, 255) : Color(40, 255, 255, 255));
			SolidBrush hb(bg);
			g.FillRectangle(&hb, br.left, br.top, br.Width(), br.Height());
		}
		Color glyph = (hot && i == 2) ? Color(255, 255, 255, 255) : Color(255, 0xcf, 0xc9, 0xb8);
		Pen pen(glyph, 1.4f);
		int cx = br.left + br.Width() / 2;
		int cy = br.top + br.Height() / 2;
		if (i == 0) {
			g.DrawLine(&pen, cx - 6, cy, cx + 6, cy);
		} else if (i == 1) {
			if (IsZoomed()) {
				g.DrawRectangle(&pen, cx - 5, cy - 2, 7, 7);
				g.DrawRectangle(&pen, cx - 2, cy - 5, 7, 7);
			} else {
				g.DrawRectangle(&pen, cx - 5, cy - 5, 10, 10);
			}
		} else {
			g.DrawLine(&pen, cx - 5, cy - 5, cx + 5, cy + 5);
			g.DrawLine(&pen, cx + 5, cy - 5, cx - 5, cy + 5);
		}
	}

	// ---- Row 2: menu bar + toolbar ----
	LinearGradientBrush bar2(Point(0, H), Point(0, chrome), Color(255, 22, 24, 29), Color(255, 12, 14, 18));
	g.FillRectangle(&bar2, 0, H, width, kMenuRowHeight);
	SolidBrush underline(Color(255, 0x9a, 0x16, 0x22));
	g.FillRectangle(&underline, 0, chrome - 2, width, 2);
	SolidBrush underglow(Color(70, 0xd1, 0x1f, 0x33));
	g.FillRectangle(&underglow, 0, chrome - 4, width, 2);

	// Menu labels.
	Font menu_font(&ui, 12.0f, FontStyleRegular, UnitPixel);
	StringFormat sf_c;
	sf_c.SetAlignment(StringAlignmentCenter);
	sf_c.SetLineAlignment(StringAlignmentCenter);
	sf_c.SetFormatFlags(StringFormatFlagsNoWrap);
	for (int i = 0; i < kNumMenuTops; ++i) {
		CRect mr;
		GetMenuRect(i, &mr);
		bool hot = (_hot_menu == i);
		if (hot) {
			SolidBrush hb(Color(46, 255, 255, 255));
			g.FillRectangle(&hb, mr.left, mr.top + 3, mr.Width(), mr.Height() - 6);
		}
		SolidBrush mb(hot ? Color(255, 255, 246, 232) : Color(255, 0xc9, 0xc3, 0xb4));
		RectF rf((REAL)mr.left, (REAL)mr.top, (REAL)mr.Width(), (REAL)mr.Height());
		g.DrawString(kMenuTops[i].label, -1, &menu_font, rf, &sf_c, &mb);
	}

	// Toolbar icons (Segoe MDL2 Assets glyphs).
	FontFamily mdl(L"Segoe MDL2 Assets");
	Font icon_font(&mdl, 15.0f, FontStyleRegular, UnitPixel);
	StringFormat sf_ic;
	sf_ic.SetAlignment(StringAlignmentCenter);
	sf_ic.SetLineAlignment(StringAlignmentCenter);
	sf_ic.SetFormatFlags(StringFormatFlagsNoWrap);
	for (int i = 0; i < kNumToolItems; ++i) {
		CRect tr;
		GetToolRect(i, &tr);
		if (kToolItems[i].command < 0) {
			SolidBrush sep(Color(55, 255, 255, 255));
			g.FillRectangle(&sep, tr.left + tr.Width() / 2, tr.top + 5, 1, tr.Height() - 10);
			continue;
		}
		bool hot = (_hot_tool == i);
		if (hot) {
			SolidBrush hb(Color(255, 0xd1, 0x1f, 0x33));
			g.FillRectangle(&hb, tr.left, tr.top, tr.Width(), tr.Height());
		}
		Color gc = hot ? Color(255, 255, 255, 255)
			: (kToolItems[i].scarlet ? Color(255, 0xe0, 0x46, 0x55) : Color(255, 0xcf, 0xc9, 0xb8));
		SolidBrush gb(gc);
		RectF rf((REAL)tr.left, (REAL)tr.top, (REAL)tr.Width(), (REAL)tr.Height());
		g.DrawString(kToolItems[i].glyph, -1, &icon_font, rf, &sf_ic, &gb);
	}

	screen.DrawImage(&buffer, 0, 0);
}

void CReactTableWindow::OnLButtonDown(UINT nFlags, CPoint point)
{
	int hit = HitButton(point);
	if (hit >= 0) {
		_pressed_button = hit;
		SetCapture();
		InvalidateChrome();
		return;
	}
	int menu = HitMenu(point);
	if (menu >= 0) {
		OpenTopMenu(menu);
		return;
	}
	int tool = HitTool(point);
	if (tool >= 0) {
		ForwardCommand((unsigned int)kToolItems[tool].command);
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
		InvalidateChrome();
		if (HitButton(point) == pressed) {
			DoButtonAction(pressed);
		}
		return;
	}
	CWnd::OnLButtonUp(nFlags, point);
}

void CReactTableWindow::OnMouseMove(UINT nFlags, CPoint point)
{
	int hb = HitButton(point);
	int hm = HitMenu(point);
	int ht = HitTool(point);
	if (hb != _hot_button || hm != _hot_menu || ht != _hot_tool) {
		_hot_button = hb;
		_hot_menu = hm;
		_hot_tool = ht;
		InvalidateChrome();
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
	if (_hot_button != -1 || _hot_menu != -1 || _hot_tool != -1) {
		_hot_button = -1;
		_hot_menu = -1;
		_hot_tool = -1;
		InvalidateChrome();
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

// ----- menu bar + toolbar (row 2) -------------------------------------------

void CReactTableWindow::GetMenuRect(int index, CRect *rect)
{
	int x = kMenuStartX;
	for (int j = 0; j < index; ++j) {
		x += kMenuTops[j].width;
	}
	rect->SetRect(x, kTitleBarHeight, x + kMenuTops[index].width, kTitleBarHeight + kMenuRowHeight);
}

int CReactTableWindow::HitMenu(CPoint client_pt)
{
	if (client_pt.y < kTitleBarHeight || client_pt.y >= ChromeHeight()) {
		return -1;
	}
	for (int i = 0; i < kNumMenuTops; ++i) {
		CRect mr;
		GetMenuRect(i, &mr);
		if (mr.PtInRect(client_pt)) {
			return i;
		}
	}
	return -1;
}

void CReactTableWindow::GetToolRect(int index, CRect *rect)
{
	int x = kMenuStartX;
	for (int j = 0; j < kNumMenuTops; ++j) {
		x += kMenuTops[j].width;
	}
	x += 18;   // gap between the menu bar and the toolbar
	for (int j = 0; j < index; ++j) {
		x += (kToolItems[j].command < 0) ? kToolSeparatorWidth : kToolButtonWidth;
	}
	int w = (kToolItems[index].command < 0) ? kToolSeparatorWidth : kToolButtonWidth;
	rect->SetRect(x, kTitleBarHeight + 1, x + w, kTitleBarHeight + kMenuRowHeight - 1);
}

int CReactTableWindow::HitTool(CPoint client_pt)
{
	if (client_pt.y < kTitleBarHeight || client_pt.y >= ChromeHeight()) {
		return -1;
	}
	for (int i = 0; i < kNumToolItems; ++i) {
		if (kToolItems[i].command < 0) {
			continue;
		}
		CRect tr;
		GetToolRect(i, &tr);
		if (tr.PtInRect(client_pt)) {
			return i;
		}
	}
	return -1;
}

void CReactTableWindow::OpenTopMenu(int index)
{
	if (index < 0 || index >= kNumMenuTops) {
		return;
	}
	if (kMenuTops[index].submenu_index < 0) {
		ForwardCommand(kMenuTops[index].direct_command);   // "Problem Solver" (no popup)
		return;
	}
	HMENU top = ::LoadMenu(AfxGetResourceHandle(), MAKEINTRESOURCE(IDR_MAINFRAME));
	if (top == NULL) {
		return;
	}
	HMENU sub = ::GetSubMenu(top, kMenuTops[index].submenu_index);
	if (sub != NULL) {
		CRect mr;
		GetMenuRect(index, &mr);
		CPoint pt(mr.left, mr.bottom);
		ClientToScreen(&pt);
		_hot_menu = index;
		InvalidateChrome();
		UINT cmd = ::TrackPopupMenu(sub, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
			pt.x, pt.y, 0, GetSafeHwnd(), NULL);
		_hot_menu = -1;
		InvalidateChrome();
		if (cmd != 0) {
			ForwardCommand(cmd);
		}
	}
	::DestroyMenu(top);
}

// Route a command to the main Hiss window (it owns all the menu/toolbar handlers).
void CReactTableWindow::ForwardCommand(unsigned int command_id)
{
	if (command_id == 0) {
		return;
	}
	if (_owner != NULL && ::IsWindow(_owner->GetSafeHwnd())) {
		_owner->PostMessage(WM_COMMAND, MAKEWPARAM(command_id, 0), 0);
	}
}
