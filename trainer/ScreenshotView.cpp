#include "stdafx.h"
#include "ScreenshotView.h"
#include "TrainerDB.h"

IMPLEMENT_DYNAMIC(CScreenshotView, CWnd)

BEGIN_MESSAGE_MAP(CScreenshotView, CWnd)
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
	ON_WM_LBUTTONDOWN()
	ON_MESSAGE(WM_EXITSIZEMOVE, &CScreenshotView::OnExitSizeMove)
END_MESSAGE_MAP()

CScreenshotView::CScreenshotView()
{
	_owner = NULL;
	_frame = NULL;
	_src_w = 0;
	_src_h = 0;
	_selected = -1;
	SetRectEmpty(&_draw_rect);
}

CScreenshotView::~CScreenshotView()
{
	ClearBitmap();
}

void CScreenshotView::ClearBitmap()
{
	if (_frame != NULL) {
		DeleteObject(_frame);
		_frame = NULL;
	}
}

BOOL CScreenshotView::Create(CWnd *owner)
{
	_owner = owner;
	CString class_name = AfxRegisterWndClass(
		CS_HREDRAW | CS_VREDRAW,
		AfxGetApp()->LoadStandardCursor(IDC_ARROW),
		(HBRUSH)::GetStockObject(DKGRAY_BRUSH),
		::LoadIcon(NULL, IDI_APPLICATION));

	BOOL ok = CWnd::CreateEx(
		0,
		class_name,
		"Table View",
		WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		CW_USEDEFAULT, CW_USEDEFAULT, 900, 600,
		owner == NULL ? NULL : owner->GetSafeHwnd(),
		NULL);
	if (ok) {
		TrainerDB_RestoreWindowRect(GetSafeHwnd(), "screenshot");
	}
	return ok;
}

// Persist position + size once the user finishes a move/resize drag.
LRESULT CScreenshotView::OnExitSizeMove(WPARAM, LPARAM)
{
	TrainerDB_SaveWindowRect(GetSafeHwnd(), "screenshot");
	return 0;
}

void CScreenshotView::UpdateFrame(HBITMAP src, int src_w, int src_h,
	const std::vector<STrainerRegion> &regions, int selected)
{
	if (!::IsWindow(GetSafeHwnd())) {
		return;
	}
	ClearBitmap();
	if (src != NULL && src_w > 0 && src_h > 0) {
		// Copy the source bitmap so we own a stable frame.
		HDC hdcScreen = CreateDC("DISPLAY", NULL, NULL, NULL);
		HDC hdc_src = CreateCompatibleDC(hdcScreen);
		HDC hdc_dst = CreateCompatibleDC(hdcScreen);
		HBITMAP copy = CreateCompatibleBitmap(hdcScreen, src_w, src_h);
		HBITMAP old_src = (HBITMAP)SelectObject(hdc_src, src);
		HBITMAP old_dst = (HBITMAP)SelectObject(hdc_dst, copy);
		BitBlt(hdc_dst, 0, 0, src_w, src_h, hdc_src, 0, 0, SRCCOPY);
		SelectObject(hdc_src, old_src);
		SelectObject(hdc_dst, old_dst);
		DeleteDC(hdc_src);
		DeleteDC(hdc_dst);
		DeleteDC(hdcScreen);
		_frame = copy;
		_src_w = src_w;
		_src_h = src_h;
	}
	_regions = regions;
	_selected = selected;
	Invalidate(FALSE);
}

BOOL CScreenshotView::OnEraseBkgnd(CDC *pDC)
{
	return TRUE;   // we paint the whole client area in OnPaint
}

void CScreenshotView::OnPaint()
{
	CPaintDC dc(this);
	CRect client;
	GetClientRect(&client);

	// Background.
	dc.FillSolidRect(&client, RGB(40, 44, 48));

	if (_frame == NULL || _src_w <= 0 || _src_h <= 0) {
		dc.SetTextColor(RGB(200, 200, 200));
		dc.SetBkMode(TRANSPARENT);
		dc.DrawText("Connect to a window to see its screenshot here.", &client,
			DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		SetRectEmpty(&_draw_rect);
		return;
	}

	// Scale to fit while preserving aspect ratio.
	double sx = (double)client.Width() / _src_w;
	double sy = (double)client.Height() / _src_h;
	double scale = (sx < sy) ? sx : sy;
	if (scale <= 0) scale = 1.0;
	int dw = (int)(_src_w * scale);
	int dh = (int)(_src_h * scale);
	int ox = (client.Width() - dw) / 2;
	int oy = (client.Height() - dh) / 2;
	_draw_rect.left = ox; _draw_rect.top = oy; _draw_rect.right = ox + dw; _draw_rect.bottom = oy + dh;

	HDC hdcScreen = CreateDC("DISPLAY", NULL, NULL, NULL);
	HDC hdc_src = CreateCompatibleDC(hdcScreen);
	HBITMAP old_src = (HBITMAP)SelectObject(hdc_src, _frame);
	SetStretchBltMode(dc.GetSafeHdc(), COLORONCOLOR);
	StretchBlt(dc.GetSafeHdc(), ox, oy, dw, dh, hdc_src, 0, 0, _src_w, _src_h, SRCCOPY);
	SelectObject(hdc_src, old_src);
	DeleteDC(hdc_src);
	DeleteDC(hdcScreen);

	// Region rectangles (map source pixels -> view coords).
	for (size_t i = 0; i < _regions.size(); i++) {
		const RECT &r = _regions[i].rect;
		int rl = ox + (int)(r.left * scale);
		int rt = oy + (int)(r.top * scale);
		int rr = ox + (int)(r.right * scale);
		int rb = oy + (int)(r.bottom * scale);
		bool sel = ((int)i == _selected);
		// Disabled field types (not in the shared scrape_fields list) are drawn dim
		// grey and can't be selected; enabled ones use yellow (selected) / red.
		COLORREF col = !_regions[i].enabled ? RGB(110, 110, 110)
			: (sel ? RGB(255, 220, 0) : RGB(255, 60, 60));
		CPen pen(PS_SOLID, (sel && _regions[i].enabled) ? 2 : 1, col);
		CPen *old_pen = dc.SelectObject(&pen);
		CGdiObject *old_brush = dc.SelectStockObject(NULL_BRUSH);
		dc.Rectangle(rl, rt, rr + 1, rb + 1);
		dc.SelectObject(old_pen);
		dc.SelectObject(old_brush);
	}
}

bool CScreenshotView::ViewToSource(CPoint pt, int *sx, int *sy)
{
	if (IsRectEmpty(&_draw_rect)) return false;
	if (pt.x < _draw_rect.left || pt.x >= _draw_rect.right
		|| pt.y < _draw_rect.top || pt.y >= _draw_rect.bottom) {
		return false;
	}
	int dw = _draw_rect.right - _draw_rect.left;
	int dh = _draw_rect.bottom - _draw_rect.top;
	if (dw <= 0 || dh <= 0) return false;
	*sx = (int)((double)(pt.x - _draw_rect.left) / dw * _src_w);
	*sy = (int)((double)(pt.y - _draw_rect.top) / dh * _src_h);
	return true;
}

void CScreenshotView::OnLButtonDown(UINT nFlags, CPoint point)
{
	int sx = 0, sy = 0;
	if (ViewToSource(point, &sx, &sy)) {
		for (size_t i = 0; i < _regions.size(); i++) {
			if (!_regions[i].enabled) {
				continue;   // disabled field type: not selectable
			}
			const RECT &r = _regions[i].rect;
			if (sx >= (int)r.left && sx <= (int)r.right && sy >= (int)r.top && sy <= (int)r.bottom) {
				_selected = (int)i;
				Invalidate(FALSE);
				if (_owner != NULL && ::IsWindow(_owner->GetSafeHwnd())) {
					_owner->PostMessage(WM_TRAINER_REGION_SELECTED, (WPARAM)i, 0);
				}
				break;
			}
		}
	}
	CWnd::OnLButtonDown(nFlags, point);
}
