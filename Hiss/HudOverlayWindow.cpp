//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//
//******************************************************************************
//
// Purpose: see HudOverlayWindow.h
//
//******************************************************************************

#include "stdafx.h"
#include "HudOverlayWindow.h"

#include "CAutoConnector.h"
#include "CScraper.h"             // g_hud_calibrate_request / g_hud_positions_json / g_dump_scrapes_once
#include "CTableState.h"
#include "HudManager.h"
#include "..\CTablemap\CTablemap.h"
#include "..\CTablemap\CTablemapDB.h"
#include "CEngineContainer.h"
#include "CSymbolengineUserchair.h"

// Seat-layout percentage table (defined in OpenHoldemView.cpp); used for default
// per-seat HUD positions before the first MCP recalibration.
extern double pc[kMaxNumberOfPlayers + 1][kMaxNumberOfPlayers][2];

// Right-click menu command ids.
#define IDM_HUD_RECALIBRATE 1
#define IDM_HUD_LOCK        2
#define IDM_HUD_SAVE        3

// Transparent sentinel colour (filled background becomes click-through-invisible).
static const COLORREF kHudColorKey  = RGB(1, 1, 1);
static const COLORREF kHudBoxBack   = RGB(12, 16, 24);
static const COLORREF kHudBoxBorder = RGB(70, 80, 100);
static const COLORREF kHudEditBorder= RGB(255, 210, 0);
static const COLORREF kHudText      = RGB(235, 235, 235);
static const COLORREF kHudName      = RGB(120, 200, 255);

static const int kHudBoxWidth = 100;
static const int kHudLineH    = 13;

CHudOverlayWindow *p_hud_overlay_window = NULL;

IMPLEMENT_DYNAMIC(CHudOverlayWindow, CWnd)

BEGIN_MESSAGE_MAP(CHudOverlayWindow, CWnd)
	ON_WM_CREATE()
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
	ON_WM_NCHITTEST()
	ON_WM_LBUTTONDOWN()
	ON_WM_MOUSEMOVE()
	ON_WM_LBUTTONUP()
	ON_WM_RBUTTONUP()
	ON_MESSAGE(WM_HUD_APPLY_POSITIONS, &CHudOverlayWindow::OnApplyPositions)
END_MESSAGE_MAP()

CHudOverlayWindow::CHudOverlayWindow() {
	_owner = NULL;
	_locked = false;
	_loaded = false;
	_loaded_for = "";
	_drag_chair = -1;
	_drag_grab = CPoint(0, 0);
	for (int i = 0; i < kMaxNumberOfPlayers; ++i) {
		_fx[i] = -1.0;
		_fy[i] = -1.0;
		_box_visible[i] = false;
		_box_rect[i] = CRect(0, 0, 0, 0);
	}
}

CHudOverlayWindow::~CHudOverlayWindow() {
}

BOOL CHudOverlayWindow::Create(CWnd *owner) {
	_owner = owner;
	CString class_name = AfxRegisterWndClass(
		CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW,
		AfxGetApp()->LoadStandardCursor(IDC_ARROW),
		NULL,   // no background brush -- we paint everything
		NULL);
	// Borderless popup; layered (colour-key transparency), topmost, no taskbar,
	// never activated (so clicking a box doesn't steal focus from scrcpy).
	BOOL created = CreateEx(
		WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
		class_name, _T("HissHUD"),
		WS_POPUP,
		0, 0, 100, 100,
		owner == NULL ? NULL : owner->GetSafeHwnd(),
		NULL);
	return created;
}

int CHudOverlayWindow::OnCreate(LPCREATESTRUCT lpCreateStruct) {
	if (CWnd::OnCreate(lpCreateStruct) == -1) return -1;
	// Colour-key transparency: kHudColorKey pixels become fully transparent + click-through.
	::SetLayeredWindowAttributes(GetSafeHwnd(), kHudColorKey, 0, LWA_COLORKEY);
	return 0;
}

BOOL CHudOverlayWindow::OnEraseBkgnd(CDC * /*pDC*/) {
	return TRUE;   // fully painted in OnPaint (double-buffered)
}

// ---------------------------------------------------------------------------
// Geometry: keep the overlay covering the scrcpy client area.
// ---------------------------------------------------------------------------
void CHudOverlayWindow::TrackTableWindow() {
	if (!::IsWindow(GetSafeHwnd())) return;
	HWND table = (p_autoconnector != NULL) ? p_autoconnector->attached_hwnd() : NULL;
	bool show = (table != NULL && ::IsWindow(table)
		&& p_hud_manager != NULL && p_hud_manager->IsEnabled());
	if (!show) {
		if (IsWindowVisible()) ShowWindow(SW_HIDE);
		return;
	}
	RECT cr = { 0 };
	::GetClientRect(table, &cr);
	POINT tl = { cr.left, cr.top };
	POINT br = { cr.right, cr.bottom };
	::ClientToScreen(table, &tl);
	::ClientToScreen(table, &br);
	int w = br.x - tl.x;
	int h = br.y - tl.y;
	if (w <= 0 || h <= 0) {
		if (IsWindowVisible()) ShowWindow(SW_HIDE);
		return;
	}
	EnsureLoaded();
	UINT flags = SWP_NOACTIVATE;
	if (!IsWindowVisible()) flags |= SWP_SHOWWINDOW;
	SetWindowPos(&wndTopMost, tl.x, tl.y, w, h, flags);
	Invalidate(FALSE);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
void CHudOverlayWindow::DefaultFractionForChair(int chair, int nchairs, double *fx, double *fy) const {
	if (nchairs >= 2 && nchairs <= kMaxNumberOfPlayers && chair >= 0 && chair < nchairs) {
		*fx = pc[nchairs][chair][0] - 0.06;          // shift left so the box centres under the seat
		*fy = pc[nchairs][chair][1] + 0.05;          // a touch below the seat (name/balance area)
	} else {
		*fx = 0.45; *fy = 0.45;
	}
	if (*fx < 0.0) *fx = 0.0; if (*fx > 0.85) *fx = 0.85;
	if (*fy < 0.0) *fy = 0.0; if (*fy > 0.92) *fy = 0.92;
}

void CHudOverlayWindow::ComputeBoxRects(int client_w, int client_h) {
	int hero = -1;
	if (p_engine_container != NULL && p_engine_container->symbol_engine_userchair() != NULL) {
		hero = p_engine_container->symbol_engine_userchair()->userchair();
	}
	int nchairs = (p_tablemap != NULL) ? p_tablemap->nchairs() : 0;
	for (int chair = 0; chair < kMaxNumberOfPlayers; ++chair) {
		_box_visible[chair] = false;
		bool seated = (p_table_state != NULL) && (chair < nchairs)
			&& p_table_state->Player(chair)->seated();
		int samples = (p_hud_manager != NULL) ? p_hud_manager->SamplesForChair(chair) : -1;
		if (!seated || samples <= 0 || chair == hero) continue;

		double fx = _fx[chair], fy = _fy[chair];
		if (fx < 0.0 || fy < 0.0) DefaultFractionForChair(chair, nchairs, &fx, &fy);

		std::vector<SHudStatValue> stats = p_hud_manager->StatsForChair(chair);
		int nlines = 2 + (int)stats.size();         // name + n= + stats
		if (nlines > 9) nlines = 9;                 // safety cap
		int bw = kHudBoxWidth;
		int bh = nlines * kHudLineH + 4;
		int x = (int)(fx * client_w);
		int y = (int)(fy * client_h);
		if (x < 0) x = 0; if (x + bw > client_w) x = client_w - bw; if (x < 0) x = 0;
		if (y < 0) y = 0; if (y + bh > client_h) y = client_h - bh; if (y < 0) y = 0;
		_box_rect[chair] = CRect(x, y, x + bw, y + bh);
		_box_visible[chair] = true;
	}
}

int CHudOverlayWindow::BoxIndexAtClientPoint(CPoint pt) const {
	for (int chair = 0; chair < kMaxNumberOfPlayers; ++chair) {
		if (_box_visible[chair] && _box_rect[chair].PtInRect(pt)) return chair;
	}
	return -1;
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------
void CHudOverlayWindow::OnPaint() {
	CPaintDC dc(this);
	CRect client;
	GetClientRect(&client);
	int cw = client.Width(), ch = client.Height();
	if (cw <= 0 || ch <= 0) return;

	CDC mem;
	mem.CreateCompatibleDC(&dc);
	CBitmap bmp;
	bmp.CreateCompatibleBitmap(&dc, cw, ch);
	CBitmap *oldbmp = mem.SelectObject(&bmp);

	mem.FillSolidRect(0, 0, cw, ch, kHudColorKey);   // transparent background

	ComputeBoxRects(cw, ch);
	bool ctrl = (GetKeyState(VK_CONTROL) < 0);

	LOGFONT lf; ZeroMemory(&lf, sizeof(lf));
	lf.lfHeight = -11; lf.lfWeight = FW_NORMAL; lf.lfQuality = CLEARTYPE_QUALITY;
	strcpy_s(lf.lfFaceName, 32, "Segoe UI");
	CFont font; font.CreateFontIndirect(&lf);
	CFont *oldfont = mem.SelectObject(&font);
	mem.SetBkMode(TRANSPARENT);

	for (int chair = 0; chair < kMaxNumberOfPlayers; ++chair) {
		if (!_box_visible[chair]) continue;
		CRect r = _box_rect[chair];
		mem.FillSolidRect(&r, kHudBoxBack);
		// border (edit highlight while CTRL held)
		CBrush bb(ctrl ? kHudEditBorder : kHudBoxBorder);
		mem.FrameRect(&r, &bb);

		int x = r.left + 3, y = r.top + 2;
		// Player name
		mem.SetTextColor(kHudName);
		CRect nr(x, y, r.right - 2, y + kHudLineH);
		mem.DrawText(p_table_state->Player(chair)->name(), -1, &nr,
			DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
		y += kHudLineH;
		// Sample size
		mem.SetTextColor(kHudText);
		int samples = p_hud_manager->SamplesForChair(chair);
		CString s; s.Format("n=%d", samples < 0 ? 0 : samples);
		CRect sr(x, y, r.right - 2, y + kHudLineH);
		mem.DrawText(s, -1, &sr, DT_LEFT | DT_SINGLELINE);
		y += kHudLineH;
		// Stats
		std::vector<SHudStatValue> stats = p_hud_manager->StatsForChair(chair);
		for (size_t i = 0; i < stats.size() && y + kHudLineH <= r.bottom; ++i) {
			mem.SetTextColor(stats[i].important ? kHudName : kHudText);
			CString line;
			line.Format("%s %s", stats[i].abbreviation.GetString(), stats[i].value.GetString());
			CRect lr(x, y, r.right - 2, y + kHudLineH);
			mem.DrawText(line, -1, &lr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
			y += kHudLineH;
		}
	}

	dc.BitBlt(0, 0, cw, ch, &mem, 0, 0, SRCCOPY);
	mem.SelectObject(oldfont);
	mem.SelectObject(oldbmp);
}

// ---------------------------------------------------------------------------
// Mouse: click-through unless CTRL is held over a box.
// ---------------------------------------------------------------------------
LRESULT CHudOverlayWindow::OnNcHitTest(CPoint point) {
	if (GetKeyState(VK_CONTROL) < 0) {
		CPoint c = point;
		ScreenToClient(&c);
		if (BoxIndexAtClientPoint(c) >= 0) return HTCLIENT;
	}
	return HTTRANSPARENT;   // pass the click through to scrcpy
}

void CHudOverlayWindow::OnLButtonDown(UINT nFlags, CPoint point) {
	if (_locked) { CWnd::OnLButtonDown(nFlags, point); return; }
	int chair = BoxIndexAtClientPoint(point);
	if (chair >= 0) {
		_drag_chair = chair;
		_drag_grab = point - _box_rect[chair].TopLeft();
		SetCapture();
		return;
	}
	CWnd::OnLButtonDown(nFlags, point);
}

void CHudOverlayWindow::OnMouseMove(UINT nFlags, CPoint point) {
	if (_drag_chair >= 0) {
		CRect client; GetClientRect(&client);
		int cw = client.Width(), ch = client.Height();
		if (cw > 0 && ch > 0) {
			int nx = point.x - _drag_grab.x;
			int ny = point.y - _drag_grab.y;
			_fx[_drag_chair] = (double)nx / (double)cw;
			_fy[_drag_chair] = (double)ny / (double)ch;
			Invalidate(FALSE);
		}
		return;
	}
	CWnd::OnMouseMove(nFlags, point);
}

void CHudOverlayWindow::OnLButtonUp(UINT nFlags, CPoint point) {
	if (_drag_chair >= 0) {
		_drag_chair = -1;
		ReleaseCapture();
		SavePositions();
		return;
	}
	CWnd::OnLButtonUp(nFlags, point);
}

void CHudOverlayWindow::OnRButtonUp(UINT nFlags, CPoint point) {
	// Only reached when CTRL was held over a box (NcHitTest returned HTCLIENT).
	CMenu menu;
	menu.CreatePopupMenu();
	menu.AppendMenu(MF_STRING, IDM_HUD_RECALIBRATE, _T("Recalibrate all HUDs (Claude)"));
	menu.AppendMenu(MF_STRING | (_locked ? MF_CHECKED : MF_UNCHECKED), IDM_HUD_LOCK, _T("Lock all HUDs"));
	menu.AppendMenu(MF_STRING, IDM_HUD_SAVE, _T("Save positions"));
	CPoint scr = point;
	ClientToScreen(&scr);
	int cmd = menu.TrackPopupMenu(TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN,
		scr.x, scr.y, this);
	if (cmd == IDM_HUD_RECALIBRATE) {
		g_dump_scrapes_once = true;        // refresh _table.bmp for Claude to read
		g_hud_calibrate_request = true;    // MCP polls /api/hud-calibrate-status
	} else if (cmd == IDM_HUD_LOCK) {
		_locked = !_locked;
		SavePositions();
	} else if (cmd == IDM_HUD_SAVE) {
		SavePositions();
	}
}

LRESULT CHudOverlayWindow::OnApplyPositions(WPARAM /*w*/, LPARAM /*l*/) {
	ParsePositions(g_hud_positions_json);
	SavePositions();
	Invalidate(FALSE);
	return 0;
}

// ---------------------------------------------------------------------------
// Persistence (postgres settings: key "hud_positions", field = tablemap name)
// ---------------------------------------------------------------------------
CString CHudOverlayWindow::PositionsField() const {
	CString f = (p_tablemap != NULL) ? p_tablemap->filename() : CString("");
	f.Trim();
	if (f.IsEmpty()) f = "default";
	return f;
}

void CHudOverlayWindow::EnsureLoaded() {
	CString field = PositionsField();
	if (!_loaded || field != _loaded_for) {
		LoadPositions();
		_loaded = true;
		_loaded_for = field;
	}
}

void CHudOverlayWindow::LoadPositions() {
	for (int i = 0; i < kMaxNumberOfPlayers; ++i) { _fx[i] = -1.0; _fy[i] = -1.0; }
	_locked = false;
	if (p_tablemap_db == NULL) return;
	CString v = p_tablemap_db->GetSettingString("hud_positions", PositionsField());
	if (!v.IsEmpty()) ParsePositions(v);
}

void CHudOverlayWindow::ParsePositions(const CString &json) {
	if (json.IsEmpty()) return;
	_locked = (json.Find("\"locked\":1") >= 0) || (json.Find("\"locked\":true") >= 0);
	for (int i = 0; i < kMaxNumberOfPlayers; ++i) {
		CString key; key.Format("\"c%d\":{", i);
		int p = json.Find(key);
		if (p < 0) continue;
		int xs = json.Find("\"x\":", p);
		int ys = json.Find("\"y\":", p);
		if (xs >= 0) _fx[i] = atof(CStringA(json.Mid(xs + 4)).GetString());
		if (ys >= 0) _fy[i] = atof(CStringA(json.Mid(ys + 4)).GetString());
	}
}

void CHudOverlayWindow::SavePositions() {
	if (p_tablemap_db == NULL) return;
	CString out;
	out.Format("{\"locked\":%d", _locked ? 1 : 0);
	for (int i = 0; i < kMaxNumberOfPlayers; ++i) {
		if (_fx[i] < 0.0 || _fy[i] < 0.0) continue;   // only persist customised seats
		CString part;
		part.Format(",\"c%d\":{\"x\":%.4f,\"y\":%.4f}", i, _fx[i], _fy[i]);
		out += part;
	}
	out += "}";
	p_tablemap_db->SetSettingString("hud_positions", PositionsField(), out);
}
