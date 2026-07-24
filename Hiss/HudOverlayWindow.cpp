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
#include "CHandresetDetector.h"   // p_handreset_detector -> the hand number the copy tile shows

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
static const COLORREF kHudBalance   = RGB(60, 255, 90);   // bright green: player balance/stack

// HUD overlay opacity (layered-window global alpha, 0..255). Kept FAINT by default so the HUD never
// obscures the table; holding CTRL makes it SOLID for reading. [Emrald request]
static const BYTE kHudAlphaFaint = 31;    // 12% opacity per request (alpha = 0.12*255) -- faint at rest
static const BYTE kHudAlphaSolid = 255;   // FULLY opaque while CTRL is held [Emrald: "should be 1"]

// RED decision overlay opacity. Its own constant, NOT kHudAlphaSolid: the decision sits directly
// over the table (above the hero's cards), so at 92% it blotted out the felt underneath. 20%
// opaque per request -- readable as a flash without hiding the cards/board it covers. [Emrald]
static const BYTE kActionAlpha = 179;     // 0.70 * 255 -- covers BOTH the red action and the blue
                                          // engine line, which share this window's global alpha.
                                          // [Emrald: 0.20 -> 0.40 -> 0.60 -> 0.70]

static const int kHudBoxWidth = 210;   // wide enough for two |-separated stat lines
static const int kHudLineH    = 13;
static const int kHudCols     = 3;    // stats drawn in a |-separated grid: 3 columns
static const int kHudColW     = 66;   // px per stat column

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
	// Borderless popup; layered (colour-key transparency), topmost, no taskbar, never activated.
	// WS_EX_TRANSPARENT makes the WHOLE overlay hit-test-transparent: every click passes straight through
	// to the table / scrcpy mirror beneath -- ALWAYS, even where the HUD or the RED decision is drawn SOLID
	// (the hero-name box stopped being click-through when the decision forced it opaque). This also stops the
	// overlay from ever eating the bot's OWN autoplayer button clicks. The overlay is now purely visual.
	// [Emrald: HUD + RED DECISION must be click-through so clicks reach scrcpy]
	BOOL created = CreateEx(
		WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
		class_name, _T("HissHUD"),
		WS_POPUP,
		0, 0, 100, 100,
		owner == NULL ? NULL : owner->GetSafeHwnd(),
		NULL);
	return created;
}

int CHudOverlayWindow::OnCreate(LPCREATESTRUCT lpCreateStruct) {
	if (CWnd::OnCreate(lpCreateStruct) == -1) return -1;
	// Colour-key for the transparent (click-through) background, PLUS a global alpha. Start FAINT;
	// TrackTableWindow (200ms timer) raises it to solid while CTRL is held. [Emrald request]
	::SetLayeredWindowAttributes(GetSafeHwnd(), kHudColorKey, kHudAlphaFaint, LWA_COLORKEY | LWA_ALPHA);
	return 0;
}

BOOL CHudOverlayWindow::OnEraseBkgnd(CDC * /*pDC*/) {
	return TRUE;   // fully painted in OnPaint (double-buffered)
}

// ---------------------------------------------------------------------------
// Geometry: keep the overlay covering the scrcpy client area.
// ---------------------------------------------------------------------------
// CTRL -> HUD opacity, on its own fast cadence.
//
// The full TrackTableWindow runs at 200ms because repositioning and re-styling do not need to be
// faster. But the CTRL response DOES: at 200ms a press shorter than a tick is missed entirely, and
// with two instances each polling on their own timer the two HUDs visibly disagree (measured:
// alpha=31,255 while CTRL was held). This does nothing but read the key and correct the alpha, so it
// is cheap to run at 50ms and makes holding CTRL feel immediate. [Emrald]
void CHudOverlayWindow::RefreshCtrlAlpha() {
	if (!::IsWindow(GetSafeHwnd()) || !IsWindowVisible()) return;
	BYTE want_alpha = ((::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
	                ? kHudAlphaSolid : kHudAlphaFaint;
	BYTE actual_alpha = 0; COLORREF actual_key = 0; DWORD actual_flags = 0;
	bool readable = (::GetLayeredWindowAttributes(GetSafeHwnd(), &actual_key, &actual_alpha,
	                                              &actual_flags) != FALSE);
	if (!readable || actual_alpha != want_alpha) {
		BOOL applied = ::SetLayeredWindowAttributes(GetSafeHwnd(), kHudColorKey, want_alpha,
		                                            LWA_COLORKEY | LWA_ALPHA);
		// GROUND TRUTH for the recurring "CTRL does not make the tiles opaque" report. Two
		// measurements disagreed -- an external probe saw the alpha reach 255 while the tiles still
		// looked see-through -- so log what we asked for, what the window HAD, whether the call
		// succeeded, and what it reads back immediately after. Only fires on an actual change (a few
		// lines per CTRL press), so it cannot flood. [Emrald]
		BYTE after = 0; COLORREF k2 = 0; DWORD f2 = 0;
		BOOL reread = ::GetLayeredWindowAttributes(GetSafeHwnd(), &k2, &after, &f2);
		write_log(k_always_log_basic_information,
			"[HudAlpha] ctrl=%d want=%u had=%s setattr=%s -> reads back %s (key=0x%06X flags=0x%X)\n",
			(want_alpha == kHudAlphaSolid) ? 1 : 0, (unsigned)want_alpha,
			readable ? "readable" : "UNREADABLE", applied ? "ok" : "FAILED",
			reread ? "ok" : "UNREADABLE", (unsigned)k2, (unsigned)f2);
		if (reread && after != want_alpha) {
			write_log(k_always_log_errors,
				"[HudAlpha] MISMATCH: asked for %u but the window reports %u immediately after the "
				"call -- something else is overwriting it.\n", (unsigned)want_alpha, (unsigned)after);
		}
	}
}

void CHudOverlayWindow::TrackTableWindow() {
	if (!::IsWindow(GetSafeHwnd())) return;
	HWND table = (p_autoconnector != NULL) ? p_autoconnector->attached_hwnd() : NULL;
	// Hide the overlay whenever the scrcpy table window is hidden or minimized, so the HUD
	// never floats orphaned over the desktop / other apps; the 200ms timer restores it as
	// soon as scrcpy is visible again.
	bool table_visible = (table != NULL && ::IsWindow(table)
		&& ::IsWindowVisible(table) && !::IsIconic(table));
	bool show = (table_visible
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
	bool ctrl_held = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
	// Click-through gate: the overlay is hit-test-transparent by DEFAULT so every click (the bot's own
	// autoplayer clicks and the user's) passes through to scrcpy. WS_EX_TRANSPARENT does that at a level
	// BELOW WM_NCHITTEST, so while it is set OnNcHitTest never runs and a box can never be grabbed. Drop
	// it only while CTRL is held -- then OnNcHitTest is consulted and returns HTCLIENT over a box (for
	// left-drag) / right-click (context menu), HTTRANSPARENT everywhere else. [task #15: CTRL+drag HUD]
	//
	// This MUST run BEFORE the opacity block below: changing GWL_EXSTYLE on a layered window makes
	// Windows discard the attributes set by SetLayeredWindowAttributes. Done in the other order, every
	// CTRL press set the alpha solid and then immediately clobbered it -- and since the cache below had
	// already recorded "solid", it never re-applied and the tiles stayed faint the whole time CTRL was
	// held. Hence exstyle_changed, which forces the alpha to be re-applied after any style change.
	static int s_clickthrough = -1;               // -1 unknown; 1 = transparent set; 0 = capturing
	int want_ct = ctrl_held ? 0 : 1;
	bool exstyle_changed = false;
	if (want_ct != s_clickthrough) {
		LONG ex = ::GetWindowLong(GetSafeHwnd(), GWL_EXSTYLE);
		if (want_ct) ex |= WS_EX_TRANSPARENT; else ex &= ~WS_EX_TRANSPARENT;
		::SetWindowLong(GetSafeHwnd(), GWL_EXSTYLE, ex);
		s_clickthrough = want_ct;
		exstyle_changed = true;
	}
	// HUD opacity: faint by default, solid while CTRL is held (so it never obscures the table unless
	// Emrald wants to read it). Only re-applies when the level actually changes, or when the ex-style
	// change above dropped the layered attributes. [Emrald request]
	static BYTE s_hud_alpha = 0;
	// A trailing RED decision no longer forces this window solid. It used to, so the action would read
	// without holding CTRL -- but this window's alpha is GLOBAL, so it dragged every HUD tile opaque with
	// it and hid the table underneath. The decision now renders in its own always-solid CHudActionWindow,
	// leaving the tiles faint unless CTRL is held. [Emrald: "leave the HUD tiles transparent"]
	BYTE want_alpha = ctrl_held ? kHudAlphaSolid : kHudAlphaFaint;
	// RE-APPLY WHENEVER THE WINDOW DISAGREES, not just when our own cached value changes.
	//
	// This used to skip the call whenever want_alpha == s_hud_alpha. But Windows DISCARDS the layered
	// attributes on an ex-style change (the very reason the exstyle_changed flag exists), and the
	// click-through toggle is not the only thing that can touch style/visibility -- SWP_SHOWWINDOW and
	// the topmost re-assert run on this same 200ms tick. When the attributes were dropped by any path
	// we did not flag, the cache still said "already 31" and the real window sat at whatever Windows
	// reset it to. CTRL then did nothing until something else happened to force a change, which is
	// exactly the intermittent "it stopped working again" symptom.
	//
	// Ask the window what it actually has and correct it. GetLayeredWindowAttributes is a cheap local
	// call at 5Hz, and trusting the window over our own bookkeeping removes the whole class of bug.
	BYTE actual_alpha = 0; COLORREF actual_key = 0; DWORD actual_flags = 0;
	bool readable = (::GetLayeredWindowAttributes(GetSafeHwnd(), &actual_key, &actual_alpha,
	                                              &actual_flags) != FALSE);
	if (!readable || actual_alpha != want_alpha || want_alpha != s_hud_alpha || exstyle_changed) {
		::SetLayeredWindowAttributes(GetSafeHwnd(), kHudColorKey, want_alpha, LWA_COLORKEY | LWA_ALPHA);
		s_hud_alpha = want_alpha;
	}
	Invalidate(FALSE);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
void CHudOverlayWindow::DefaultFractionForChair(int chair, int nchairs, int client_w, int client_h, double *fx, double *fy) const {
	// Prefer the tablemap's actual pNname region (the real on-screen name-plate) so default
	// boxes land on the CORRECT seats. The generic pc[][] display layout does not match the
	// scrcpy table and put HUDs on the wrong players.
	if (p_tablemap != NULL && client_w > 0 && client_h > 0) {
		CString rn; rn.Format("p%dname", chair);
		RMapCI it = p_tablemap->r$()->find(rn.GetString());
		if (it != p_tablemap->r$()->end()) {
			*fx = (double)it->second.left / (double)client_w;
			*fy = (double)it->second.top  / (double)client_h;
			if (*fx < 0.0) *fx = 0.0; if (*fx > 0.85) *fx = 0.85;
			if (*fy < 0.0) *fy = 0.0; if (*fy > 0.92) *fy = 0.92;
			return;
		}
	}
	// Fallback: generic seat layout.
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
		// Show the hero's own box too (samples come from the same own-data stats).
		if (!seated || samples <= 0) continue;

		double fx = _fx[chair], fy = _fy[chair];
		if (fx < 0.0 || fy < 0.0) DefaultFractionForChair(chair, nchairs, client_w, client_h, &fx, &fy);

		std::vector<SHudStatValue> stats = p_hud_manager->StatsForChair(chair);
		int nlines = 1 + (stats.empty() ? 0 : 3);   // name+balance line, then 3 |-separated stat lines
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
		// Line 1: player name (blue, left) + balance (BRIGHT GREEN, right) on the same line.
		mem.SetTextColor(kHudName);
		CRect nr(x, y, r.right - 64, y + kHudLineH);   // leave room on the right for the balance
		mem.DrawText(p_table_state->Player(chair)->name(), -1, &nr,
			DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
		double bal = p_table_state->Player(chair)->_balance.GetValue();
		CString balstr;
		if (bal == (double)(long long)bal) balstr.Format("%lld", (long long)bal);
		else balstr.Format("%.2f", bal);
		mem.SetTextColor(kHudBalance);
		CRect br(r.right - 64, y, r.right - 3, y + kHudLineH);
		mem.DrawText(balstr, -1, &br, DT_RIGHT | DT_SINGLELINE);
		y += kHudLineH;
		// Stats: THREE pipe(|)-separated lines underneath the name (n= seeds the first line).
		int samples = p_hud_manager->SamplesForChair(chair);
		std::vector<SHudStatValue> stats = p_hud_manager->StatsForChair(chair);
		CString rows[3];
		rows[0].Format("n=%d", samples < 0 ? 0 : samples);
		int per = (int)((stats.size() + 2) / 3);     // spread the stats across 3 rows
		if (per < 1) per = 1;
		for (size_t i = 0; i < stats.size(); ++i) {
			int ridx = (int)i / per; if (ridx > 2) ridx = 2;
			CString tok;
			tok.Format("%s %s", stats[i].abbreviation.GetString(), stats[i].value.GetString());
			if (!rows[ridx].IsEmpty()) rows[ridx] += " | ";
			rows[ridx] += tok;
		}
		mem.SetTextColor(kHudText);
		for (int ri = 0; ri < 3; ++ri) {
			if (rows[ri].IsEmpty() || y + kHudLineH > r.bottom) continue;
			CRect lr(x, y, r.right - 2, y + kHudLineH);
			mem.DrawText(rows[ri], -1, &lr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
			y += kHudLineH;
		}
	}

	// NOTE: the on-table RED decision is NOT drawn here any more -- it lives in CHudActionWindow so it
	// can be solid while these tiles stay faint. See CHudActionWindow::OnPaint below.

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

// Anchor the RED decision on the hero's HUD box if it exists, else on the tablemap hero-seat
// position, so the decision ALWAYS shows on scrcpy -- it must NOT depend on HUD samples existing
// (a fresh account like scarletchrist has none, and the action still needs to flash).
// [Emrald: decision across scrcpy]
bool CHudOverlayWindow::HeroAnchorRect(int client_w, int client_h, CRect *out) {
	if (out == NULL || client_w <= 0 || client_h <= 0) return false;
	int hero = (p_engine_container != NULL && p_engine_container->symbol_engine_userchair() != NULL)
		? p_engine_container->symbol_engine_userchair()->userchair() : -1;
	if (hero < 0 || hero >= kMaxNumberOfPlayers) return false;
	// The action window paints independently of the HUD window, so the box rects may not have been
	// recomputed for this size yet. Cheap and idempotent -- just recompute.
	ComputeBoxRects(client_w, client_h);
	if (_box_visible[hero]) {
		*out = _box_rect[hero];
		return true;
	}
	double fx = _fx[hero], fy = _fy[hero];
	if (fx < 0.0 || fy < 0.0) {
		int nch = (p_tablemap != NULL) ? p_tablemap->nchairs() : 0;
		DefaultFractionForChair(hero, nch, client_w, client_h, &fx, &fy);
	}
	int hx = (int)(fx * client_w), hy = (int)(fy * client_h);
	*out = CRect(hx, hy, hx + kHudBoxWidth, hy + kHudLineH);
	return true;
}

// ===========================================================================
// CHudActionWindow -- the RED decision, on its own always-solid layered window
// ===========================================================================

CHudActionWindow *p_hud_action_window = NULL;

IMPLEMENT_DYNAMIC(CHudActionWindow, CWnd)

BEGIN_MESSAGE_MAP(CHudActionWindow, CWnd)
	ON_WM_NCHITTEST()
	ON_WM_LBUTTONDOWN()
	ON_WM_CREATE()
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
END_MESSAGE_MAP()

CHudActionWindow::CHudActionWindow() {
	_owner = NULL; _copy_rect.SetRectEmpty(); _copy_rect_prev.SetRectEmpty();
	_stop_rect.SetRectEmpty();
	_copied_tick = 0; _copied_which = 0;
}
CHudActionWindow::~CHudActionWindow() {}

BOOL CHudActionWindow::Create(CWnd *owner) {
	_owner = owner;
	CString class_name = AfxRegisterWndClass(
		CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW,
		AfxGetApp()->LoadStandardCursor(IDC_ARROW),
		NULL,   // no background brush -- we paint everything
		NULL);
	// WS_EX_TRANSPARENT is NO LONGER set here. It made the whole window hit-test-transparent below the
	// WM_NCHITTEST level, so OnNcHitTest never ran and nothing on this overlay could be clicked. The
	// copy-hand-number tile needs exactly one clickable rect, so the flag is dropped and OnNcHitTest
	// returns HTTRANSPARENT everywhere EXCEPT that tile -- which preserves the original guarantee
	// (the bot's own autoplayer clicks pass straight through to the felt) while making one small
	// control usable. The tile sits at the TOP of the felt beside the table pills, far from the
	// action buttons at the bottom. [Emrald: one-click hand-number copy]
	return CreateEx(
		WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
		class_name, _T("HissAction"),
		WS_POPUP,
		0, 0, 100, 100,
		owner == NULL ? NULL : owner->GetSafeHwnd(),
		NULL);
}

int CHudActionWindow::OnCreate(LPCREATESTRUCT lpCreateStruct) {
	if (CWnd::OnCreate(lpCreateStruct) == -1) return -1;
	// Colour-key for the transparent background + a FIXED alpha. This window carries only the
	// decision text, so it never needs the HUD's faint/solid switching; the text additionally fades
	// by lerping its colour toward the colour key as it ages (see OnPaint).
	::SetLayeredWindowAttributes(GetSafeHwnd(), kHudColorKey, kActionAlpha, LWA_COLORKEY | LWA_ALPHA);
	return 0;
}


// ---- COPY-HAND-NUMBER TILE ---------------------------------------------------------------------
// Hit-test: HTTRANSPARENT everywhere EXCEPT the tile, so every other click -- including the bot's
// own autoplayer clicks -- passes through to the felt exactly as before this window became
// interactive. Only the small tile beside the table pills is grabbable.
LRESULT CHudActionWindow::OnNcHitTest(CPoint point) {
	CPoint c(point);
	ScreenToClient(&c);
	if (!_stop_rect.IsRectEmpty() && _stop_rect.PtInRect(c)) return HTCLIENT;   // the STOP SIGN
	if (!_copy_rect.IsRectEmpty() && _copy_rect.PtInRect(c)) return HTCLIENT;
	if (!_copy_rect_prev.IsRectEmpty() && _copy_rect_prev.PtInRect(c)) return HTCLIENT;
	return HTTRANSPARENT;
}

void CHudActionWindow::OnLButtonDown(UINT nFlags, CPoint point) {
	// STOP SIGN first: toggle the global halt. While halted both action chokepoints (CAutoplayer's OHF
	// path and the heartbeat's NN/MCP path) refuse to commit chips, so this one click freezes the bot
	// and a second click resumes it. [stop sign]
	if (!_stop_rect.IsRectEmpty() && _stop_rect.PtInRect(point)) {
		extern volatile bool g_halt_acting; void ClearPendingAction();
		g_halt_acting = !g_halt_acting;
		if (g_halt_acting) ClearPendingAction();
		write_log(k_always_log_basic_information, "[StopSign] bot acting %s by overlay click.\n",
			g_halt_acting ? "HALTED" : "RESUMED");
		Invalidate(FALSE);
		return;
	}
	// Which row was hit? Current on top, previous underneath -- each copies its own number.
	int which = 0;
	if (!_copy_rect.IsRectEmpty() && _copy_rect.PtInRect(point)) which = 1;
	else if (!_copy_rect_prev.IsRectEmpty() && _copy_rect_prev.PtInRect(point)) which = 2;
	if (which == 0) {
		CWnd::OnLButtonDown(nFlags, point);
		return;
	}
	CString hn;
	if (which == 2) {
		hn = _prev_hand;
	} else {
		hn = (p_handreset_detector != NULL) ? p_handreset_detector->GetHandNumber() : CString("");
	}
	hn.Trim();
	if (!hn.IsEmpty() && ::OpenClipboard(GetSafeHwnd())) {
		::EmptyClipboard();
		int bytes = hn.GetLength() + 1;
		HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
		if (mem != NULL) {
			char *dst = (char *)::GlobalLock(mem);
			if (dst != NULL) {
				strcpy_s(dst, bytes, CStringA(hn).GetString());
				::GlobalUnlock(mem);
				if (::SetClipboardData(CF_TEXT, mem) != NULL) {
					_copied_tick = GetTickCount();       // drives the brief COPIED confirmation
					_copied_which = which;               // flash only the row that was clicked
					write_log(k_always_log_basic_information,
						"[HandCopy] %s hand number \"%s\" copied to the clipboard.\n",
						(which == 2) ? "previous" : "current", hn.GetString());
				} else {
					::GlobalFree(mem);
				}
			} else {
				::GlobalFree(mem);
			}
		}
		::CloseClipboard();
	}
	Invalidate(FALSE);
	CWnd::OnLButtonDown(nFlags, point);
}

BOOL CHudActionWindow::OnEraseBkgnd(CDC * /*pDC*/) {
	return TRUE;   // fully painted in OnPaint (double-buffered)
}

void CHudActionWindow::TrackTableWindow() {
	if (!::IsWindow(GetSafeHwnd())) return;
	HWND table = (p_autoconnector != NULL) ? p_autoconnector->attached_hwnd() : NULL;
	bool table_visible = (table != NULL && ::IsWindow(table)
		&& ::IsWindowVisible(table) && !::IsIconic(table));
	// Unlike the HUD this window is shown ONLY while a decision is trailing, so it is not left sitting
	// topmost-and-empty over scrcpy the rest of the time. It is also independent of the HUD being
	// enabled: the action must still flash when the HUD is switched off.
	bool decision_active = (g_hero_decision_text[0] != '\0'
		&& (GetTickCount() - g_hero_decision_tick) < 10000);
	// A live HAND NUMBER also keeps this window up, not just a trailing decision.
	//
	// Decision-only meant the window sat hidden -- and un-positioned, at its default 100x100 --
	// whenever no action was flashing. The COPY-HAND-NUMBER TILE is drawn in this window, so it
	// was invisible almost all of the time: a decision only trails for 10s after the bot acts.
	// The tile has to be reachable for the whole hand. Still hidden when there is neither, so the
	// window is never left topmost-and-empty over scrcpy. [Emrald: "i dont see the copy tile"]
	bool have_handnumber = (p_handreset_detector != NULL
		&& !p_handreset_detector->GetHandNumber().Trim().IsEmpty());
	// Keep the overlay up while halted or in manual play even between hands, so the STOP SIGN stays
	// reachable to click OFF again (and the manual state stays visible). [stop sign + manual play]
	extern volatile bool g_halt_acting; extern volatile bool g_manual_play;
	if (!table_visible || (!decision_active && !have_handnumber && !g_halt_acting && !g_manual_play)) {
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
	UINT flags = SWP_NOACTIVATE;
	if (!IsWindowVisible()) flags |= SWP_SHOWWINDOW;
	// Covers the same client area as the HUD overlay, so the hero anchor rect carries over unchanged.
	// MainFrm ticks this AFTER the HUD, so this window ends up above it and the action is never drawn
	// behind a tile.
	SetWindowPos(&wndTopMost, tl.x, tl.y, w, h, flags);
	Invalidate(FALSE);
}

// On-table RED decision: the bot's action drawn BIG + BOLD RED above the hero's box (above the hole
// cards) with the table name under it. It TRAILS ~10s after the decision and FADES out: solid for
// kDecisionHoldMs, then the red lerps to the colour key (=> transparent) by kDecisionTotalMs. The
// 200ms TrackTableWindow timer repaints it so the fade animates smoothly. [Emrald]
void CHudActionWindow::OnPaint() {
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
	mem.SetBkMode(TRANSPARENT);

	// ---- COPY-HAND-NUMBER TILE -----------------------------------------------------------------
	// Sits to the RIGHT of ACR's four table pills (measured at tablemap x 162..477, y 53..85), at the
	// top of the felt and far from the action buttons at the bottom, so making it clickable cannot
	// interfere with the bot pressing fold/call/raise. Inherits this window's 0.70 alpha. [Emrald]
	{
		CString hn = (p_handreset_detector != NULL) ? p_handreset_detector->GetHandNumber() : CString("");
		hn.Trim();
		// Roll the history on every hand change. The PREVIOUS number is the one usually wanted: by the
		// time a hand looks wrong it has often already ended, and the current tile has moved on. [Emrald]
		if (!hn.IsEmpty() && hn != _seen_hand) {
			if (!_seen_hand.IsEmpty()) _prev_hand = _seen_hand;
			_seen_hand = hn;
		}
		if (!hn.IsEmpty()) {
			const int kTileX = 487, kTileY = 53, kTileW = 132;    // just right of pill 3
			const int kRowH = 26, kPrevH = 22;                    // current row, then the parenthesised one
			bool has_prev = !_prev_hand.IsEmpty();
			const int kTileH = kRowH + (has_prev ? kPrevH : 0);
			CRect tr(kTileX, kTileY, kTileX + kTileW, kTileY + kTileH);
			if (tr.right > cw) tr.OffsetRect(cw - tr.right, 0);   // keep it on-screen on a narrow mirror
			CRect cur_r(tr.left, tr.top, tr.right, tr.top + kRowH);
			CRect prev_r(tr.left, tr.top + kRowH, tr.right, tr.bottom);
			_copy_rect = cur_r;                                   // each row is independently clickable
			_copy_rect_prev = has_prev ? prev_r : CRect(0, 0, 0, 0);
			bool flash = (_copied_tick != 0 && (GetTickCount() - _copied_tick) < 1500);
			bool cur_copied  = flash && _copied_which == 1;
			bool prev_copied = flash && _copied_which == 2;

			mem.FillSolidRect(&cur_r, cur_copied ? RGB(16, 60, 24) : RGB(18, 22, 30));
			if (has_prev) mem.FillSolidRect(&prev_r, prev_copied ? RGB(16, 60, 24) : RGB(14, 17, 24));
			CBrush edge(flash ? RGB(63, 185, 80) : RGB(90, 100, 120));
			mem.FrameRect(&tr, &edge);

			LOGFONT hf; ZeroMemory(&hf, sizeof(hf));
			hf.lfHeight = -13; hf.lfWeight = FW_BOLD; hf.lfQuality = CLEARTYPE_QUALITY;
			strcpy_s(hf.lfFaceName, 32, "Segoe UI");
			CFont hfont; hfont.CreateFontIndirect(&hf);
			CFont *oldhf = mem.SelectObject(&hfont);
			mem.SetTextColor(cur_copied ? RGB(120, 255, 150) : RGB(215, 225, 240));
			CRect ctext(cur_r); ctext.DeflateRect(4, 1);
			mem.DrawText(cur_copied ? CString("COPIED") : hn, -1, &ctext,
				DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_END_ELLIPSIS);

			if (has_prev) {                                       // dimmer + smaller: it is the secondary row
				LOGFONT pf(hf); pf.lfHeight = -12; pf.lfWeight = FW_NORMAL;
				CFont pfont; pfont.CreateFontIndirect(&pf);
				CFont *oldpf = mem.SelectObject(&pfont);
				mem.SetTextColor(prev_copied ? RGB(120, 255, 150) : RGB(150, 162, 182));
				CString ptxt; ptxt.Format("(%s)", _prev_hand.GetString());
				CRect ptext(prev_r); ptext.DeflateRect(4, 1);
				mem.DrawText(prev_copied ? CString("COPIED") : ptxt, -1, &ptext,
					DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_END_ELLIPSIS);
				mem.SelectObject(oldpf);
			}
			mem.SelectObject(oldhf);
		} else {
			_copy_rect.SetRectEmpty();      // nothing to copy -> nothing clickable
			_copy_rect_prev.SetRectEmpty();
		}
	}

	// ---- STOP SIGN --------------------------------------------------------------------------------
	// A click-to-freeze octagon just LEFT of the hand-number tile. Drawn every paint (independent of a
	// hand number) so it is reachable whenever the overlay is up. Bright red while the bot is halted,
	// dim otherwise; the click is handled in OnLButtonDown, the hit-test in OnNcHitTest. [stop sign]
	{
		extern volatile bool g_halt_acting;
		const int kStopW = 44, kStopH = 44;
		const int kTileX = 487, kTileY = 53;                  // must match the hand-tile block above
		int sx = kTileX - kStopW - 8; if (sx < 2) sx = 2;
		int sy = kTileY - 6;
		CRect sr(sx, sy, sx + kStopW, sy + kStopH);
		if (sr.right > cw) sr.OffsetRect(cw - sr.right - 2, 0);   // keep it on-screen on a narrow mirror
		_stop_rect = sr;
		bool halted = g_halt_acting;
		int cut = (int)(kStopW * 0.30);
		POINT oct[8] = {
			{ sr.left + cut, sr.top }, { sr.right - cut, sr.top },
			{ sr.right, sr.top + cut }, { sr.right, sr.bottom - cut },
			{ sr.right - cut, sr.bottom }, { sr.left + cut, sr.bottom },
			{ sr.left, sr.bottom - cut }, { sr.left, sr.top + cut }
		};
		CBrush fill(halted ? RGB(220, 24, 24) : RGB(70, 18, 18));
		CPen   edge(PS_SOLID, 2, halted ? RGB(255, 210, 210) : RGB(150, 60, 60));
		CBrush *ob = mem.SelectObject(&fill);
		CPen   *op = mem.SelectObject(&edge);
		mem.Polygon(oct, 8);
		mem.SelectObject(ob);
		mem.SelectObject(op);
		LOGFONT sf; ZeroMemory(&sf, sizeof(sf));
		sf.lfHeight = -12; sf.lfWeight = FW_BOLD; sf.lfQuality = CLEARTYPE_QUALITY;
		strcpy_s(sf.lfFaceName, 32, "Segoe UI");
		CFont sfont; sfont.CreateFontIndirect(&sf);
		CFont *osf = mem.SelectObject(&sfont);
		mem.SetTextColor(halted ? RGB(255, 255, 255) : RGB(205, 150, 150));
		CRect stext(sr);
		mem.DrawText(CString("STOP"), -1, &stext, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
		mem.SelectObject(osf);
	}

	const DWORD kDecisionHoldMs = 6000, kDecisionTotalMs = 10000;   // stay solid ~6s, then fade out by 10s [Emrald]
	DWORD dec_elapsed = GetTickCount() - g_hero_decision_tick;
	CRect hb;
	if (g_hero_decision_text[0] != '\0' && dec_elapsed < kDecisionTotalMs
		&& p_hud_overlay_window != NULL && p_hud_overlay_window->HeroAnchorRect(cw, ch, &hb)) {
		double t = (dec_elapsed <= kDecisionHoldMs) ? 0.0
			: (double)(dec_elapsed - kDecisionHoldMs) / (double)(kDecisionTotalMs - kDecisionHoldMs);
		if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
		// Lerp bright red RGB(255,0,0) -> colour key RGB(1,1,1) (which is transparent) as it ages.
		COLORREF dec_color = RGB((int)(255 + (1 - 255) * t), (int)(1 * t), (int)(1 * t));
		int ctrx = hb.CenterPoint().x;
		CString tname = g_table_identity;            // table name = part after the '|'
		int bar = tname.Find('|'); if (bar >= 0) tname = tname.Mid(bar + 1);
		LOGFONT af; ZeroMemory(&af, sizeof(af));
		af.lfHeight = -46; af.lfWeight = FW_HEAVY; af.lfQuality = CLEARTYPE_QUALITY;
		strcpy_s(af.lfFaceName, 32, "Arial Black");
		CFont bigf; bigf.CreateFontIndirect(&af);
		CFont *prevf = mem.SelectObject(&bigf);
		mem.SetTextColor(dec_color);                 // faded red
		CRect ar(ctrx - 220, hb.top - 118, ctrx + 220, hb.top - 70);   // well above the box -> above the hole cards
		mem.DrawText(CString(g_hero_decision_text), -1, &ar, DT_CENTER | DT_SINGLELINE | DT_NOCLIP);
		LOGFONT tf; ZeroMemory(&tf, sizeof(tf));
		tf.lfHeight = -16; tf.lfWeight = FW_BOLD; tf.lfQuality = CLEARTYPE_QUALITY;
		strcpy_s(tf.lfFaceName, 32, "Segoe UI");
		CFont tnf; tnf.CreateFontIndirect(&tf);
		mem.SelectObject(&tnf);
		mem.SetTextColor(dec_color);                 // table name fades with the action
		CRect tr(ctrx - 220, hb.top - 70, ctrx + 220, hb.top - 52);    // table name just under the action
		mem.DrawText(tname, -1, &tr, DT_CENTER | DT_SINGLELINE | DT_NOCLIP | DT_END_ELLIPSIS);
		// WHICH BRAIN ACTED, in blue, directly under the action. Same fade curve as the red text so the
		// pair reads as one flash, and it appears only once an action has been selected (this whole
		// block is gated on g_hero_decision_text). Blue against the red keeps the two instantly
		// separable at a glance. The window's global alpha (kActionAlpha) gives it the same 0.40
		// opacity as the action itself. [Emrald]
		if (g_hero_decision_source[0] != '\0') {
			COLORREF src_color = RGB((int)(1 * t),
			                         (int)(140 + (1 - 140) * t),
			                         (int)(255 + (1 - 255) * t));   // blue -> colour key as it ages
			LOGFONT sf; ZeroMemory(&sf, sizeof(sf));
			sf.lfHeight = -17; sf.lfWeight = FW_BOLD; sf.lfQuality = CLEARTYPE_QUALITY;
			strcpy_s(sf.lfFaceName, 32, "Segoe UI");
			CFont srcf; srcf.CreateFontIndirect(&sf);
			mem.SelectObject(&srcf);
			mem.SetTextColor(src_color);
			CRect sr(ctrx - 220, hb.top - 52, ctrx + 220, hb.top - 34);
			mem.DrawText(CString(g_hero_decision_source), -1, &sr,
				DT_CENTER | DT_SINGLELINE | DT_NOCLIP);
		}
		// Brain detail lines (exploit / branch / vs-villain / confidence / mischief), pushed by the Python
		// brain via /api/decision-detail. Multi-line ('\n'-separated), small, fading with the action so the
		// scrcpy mirror carries the SAME rich context the React table view shows. [Emrald: more lines on scrcpy]
		extern char g_hero_decision_detail[256];
		extern DWORD g_hero_decision_detail_tick;
		if (g_hero_decision_detail[0] != '\0' && (GetTickCount() - g_hero_decision_detail_tick) < 12000) {
			LOGFONT df; ZeroMemory(&df, sizeof(df));
			df.lfHeight = -13; df.lfWeight = FW_SEMIBOLD; df.lfQuality = CLEARTYPE_QUALITY;
			strcpy_s(df.lfFaceName, 32, "Segoe UI");
			CFont detf; detf.CreateFontIndirect(&df);
			mem.SelectObject(&detf);
			mem.SetTextColor(dec_color);              // same fading red as the action
			// Starts BELOW the blue engine line (which occupies top-52..top-34), not at top-50 where it
			// used to -- otherwise the two overprint each other.
			CRect dr(ctrx - 230, hb.top - 32, ctrx + 230, hb.top - 32 + 84);  // up to ~5 lines under the source
			mem.DrawText(CString(g_hero_decision_detail), -1, &dr,
				DT_CENTER | DT_NOPREFIX | DT_NOCLIP | DT_WORDBREAK);
		}
		mem.SelectObject(prevf);
	}

	dc.BitBlt(0, 0, cw, ch, &mem, 0, 0, SRCCOPY);
	mem.SelectObject(oldbmp);
}
