#include "stdafx.h"
#include "ChatTerminalWindow.h"
#include "CEngineContainer.h"
#include "CSymbolEngineChipAmounts.h"
#include "CTableState.h"
#include "ChatTerminalServer.h"
#include "HudManager.h"
#include "inlines/eval.h"
#include "..\Shared\CCritSec\CCritSec.h"
#include <shellapi.h>
#include <algorithm>

// ---- Thread-safe mirror of the active screen for the browser extension --------
// The HTTP server runs on its own thread; it reads this snapshot, which the UI
// thread refreshes whenever terminal content changes.
static CCritSec g_browser_snap_cs;
static CString  g_browser_sec[kChatTerminalSectionCount];
static CString  g_browser_pinned;

void TerminalBrowserGetSnapshot(CString out_sections[kChatTerminalSectionCount], CString *out_pinned) {
	CSLock lock(g_browser_snap_cs);
	for (int i = 0; i < kChatTerminalSectionCount; ++i) out_sections[i] = g_browser_sec[i];
	if (out_pinned) *out_pinned = g_browser_pinned;
}

const UINT WM_CHAT_TERMINAL_APPEND = WM_APP + 410;
const UINT WM_CHAT_TERMINAL_CLEAR = WM_APP + 411;

const int kTerminalWidth = 540;
const int kTerminalHeight = 640;
const int kTerminalGap = 8;
const int kControlMargin = 8;
const int kTopHeight = 30;
const int kChatHeight = 44;
const int kRangeSelectorHeight = 258;
const int kRangeCellSize = 24;
const int kRangeHeaderSize = 18;

const UINT IDC_TERMINAL_CLEAR = 24001;
const UINT IDC_TERMINAL_SEND = 24002;
const UINT IDC_TERMINAL_CHAT = 24003;
const UINT IDC_TERMINAL_SCREEN = 24004;
const UINT IDC_TERMINAL_HOLE_CARDS = 24005;
const UINT IDC_TERMINAL_VPIP = 24006;
const UINT IDC_TERMINAL_RANGE_BASE = 24200;
const UINT ID_TERMINAL_FEATURE_POT_ODDS = 24101;
const UINT ID_TERMINAL_FEATURE_IMPLIED_POT_ODDS = 24102;
const UINT ID_TERMINAL_FEATURE_REVERSE_IMPLIED_ODDS = 24103;
const UINT ID_TERMINAL_FEATURE_OPPONENT_RANGE = 24104;
const UINT ID_TERMINAL_FEATURE_LOAD_HUD_PROFILE = 24105;
const UINT ID_TERMINAL_EXTEND_BROWSER = 24106;

struct SChatTerminalMessage {
	CString screen;
	int section;
	CString text;
	bool stream;
	bool clear_screen;
	bool set_pinned;   // replace the screen's pinned (fixed) State block, in place
};

CChatTerminalWindow *p_chat_terminal = NULL;

IMPLEMENT_DYNAMIC(COpponentRangeWindow, CWnd)

BEGIN_MESSAGE_MAP(COpponentRangeWindow, CWnd)
	ON_WM_CREATE()
	ON_WM_SIZE()
	ON_WM_PAINT()
	ON_WM_MOVING()
	ON_WM_CLOSE()
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONUP()
	ON_WM_MOUSEMOVE()
	ON_EN_KILLFOCUS(IDC_TERMINAL_VPIP, &COpponentRangeWindow::OnVpipChanged)
END_MESSAGE_MAP()

COpponentRangeWindow::COpponentRangeWindow()
{
	_owner = NULL;
	_terminal = NULL;
	_layout_ready = false;
	_range_dragging = false;
	_range_drag_value = false;
	_last_drag_range_index = -1;
}

COpponentRangeWindow::~COpponentRangeWindow()
{
}

BOOL COpponentRangeWindow::Create(CWnd *owner, CChatTerminalWindow *terminal)
{
	_owner = owner;
	_terminal = terminal;
	CString class_name = AfxRegisterWndClass(
		CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW,
		AfxGetApp()->LoadStandardCursor(IDC_ARROW),
		(HBRUSH)(COLOR_WINDOW + 1),
		::LoadIcon(NULL, IDI_APPLICATION));

	BOOL created = CWnd::CreateEx(
		WS_EX_TOOLWINDOW,
		class_name,
		"Opponent Range",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		356,
		400,
		owner == NULL ? NULL : owner->GetSafeHwnd(),
		NULL);
	if (created) {
		AttachToOwner(true);
		ShowWindow(SW_SHOW);
	}
	return created;
}

int COpponentRangeWindow::OnCreate(LPCREATESTRUCT lpCreateStruct)
{
	if (CWnd::OnCreate(lpCreateStruct) == -1) {
		return -1;
	}
	_title.Create("Opponent Range", WS_CHILD | WS_VISIBLE | SS_LEFT, CRect(0, 0, 0, 0), this);
	_vpip_label.Create("VPIP", WS_CHILD | WS_VISIBLE | SS_LEFT, CRect(0, 0, 0, 0), this);
	_vpip_input.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, CRect(0, 0, 0, 0), this, IDC_TERMINAL_VPIP);
	_layout_ready = true;
	LayoutControls(lpCreateStruct->cx, lpCreateStruct->cy);
	return 0;
}

void COpponentRangeWindow::OnSize(UINT nType, int cx, int cy)
{
	CWnd::OnSize(nType, cx, cy);
	LayoutControls(cx, cy);
}

void COpponentRangeWindow::OnPaint()
{
	CPaintDC dc(this);
	DrawRangeSelector(&dc);
}

void COpponentRangeWindow::OnMoving(UINT fwSide, LPRECT pRect)
{
	CWnd::OnMoving(fwSide, pRect);
	if (_owner == NULL || !::IsWindow(_owner->GetSafeHwnd())) {
		return;
	}
	CRect owner_rect;
	_owner->GetWindowRect(&owner_rect);
	int width = pRect->right - pRect->left;
	int height = pRect->bottom - pRect->top;
	pRect->right = owner_rect.left - kTerminalGap;
	pRect->left = pRect->right - width;
	pRect->top = owner_rect.top;
	pRect->bottom = pRect->top + height;
}

void COpponentRangeWindow::OnClose()
{
	ShowWindow(SW_HIDE);
}

void COpponentRangeWindow::AttachToOwner(bool force)
{
	if (_owner == NULL || !::IsWindow(_owner->GetSafeHwnd()) || !::IsWindow(GetSafeHwnd())) {
		return;
	}
	CRect owner_rect, rect;
	_owner->GetWindowRect(&owner_rect);
	GetWindowRect(&rect);
	int x = owner_rect.left - rect.Width() - kTerminalGap;
	SetWindowPos(NULL, x, owner_rect.top, rect.Width(), rect.Height(), SWP_NOZORDER | SWP_NOACTIVATE);
}

void COpponentRangeWindow::LayoutControls(int cx, int cy)
{
	if (!_layout_ready || cx <= 0 || cy <= 0) {
		return;
	}
	int left = kControlMargin;
	int top = kControlMargin;
	_title.MoveWindow(left, top + 5, 150, 18);
	_vpip_label.MoveWindow(left + 180, top + 5, 32, 18);
	_vpip_input.MoveWindow(left + 216, top, 48, 22);
	_range_grid_rect.SetRect(left + kRangeHeaderSize, top + kTopHeight + kRangeHeaderSize,
		left + kRangeHeaderSize + 13 * kRangeCellSize,
		top + kTopHeight + kRangeHeaderSize + 13 * kRangeCellSize);
	Invalidate(FALSE);
}

void COpponentRangeWindow::DrawRangeSelector(CDC *dc)
{
	if (dc == NULL || _terminal == NULL || _range_grid_rect.IsRectEmpty()) {
		return;
	}
	const char *ranks = "AKQJT98765432";
	CFont *old_font = dc->SelectObject(GetFont());
	int old_mode = dc->SetBkMode(TRANSPARENT);
	COLORREF old_text = dc->GetTextColor();
	CPen grid_pen(PS_SOLID, 1, RGB(80, 80, 80));
	CPen border_pen(PS_SOLID, 1, RGB(145, 145, 145));
	CBrush enabled_brush(RGB(36, 116, 70));
	CBrush disabled_brush(RGB(48, 48, 48));
	CBrush triangle_brush(RGB(210, 210, 210));
	CBrush *old_brush = dc->SelectObject(&disabled_brush);
	CPen *old_pen = dc->SelectObject(&grid_pen);

	CRect selector_rect(_range_grid_rect.left - kRangeHeaderSize, _range_grid_rect.top - kRangeHeaderSize,
		_range_grid_rect.right, _range_grid_rect.bottom);
	dc->FillSolidRect(selector_rect, RGB(28, 28, 28));

	for (int col = 0; col < 13; ++col) {
		int x = _range_grid_rect.left + col * kRangeCellSize;
		CString rank;
		rank.Format("%c", ranks[col]);
		dc->SetTextColor(RGB(220, 220, 220));
		dc->DrawText(rank, CRect(x, _range_grid_rect.top - kRangeHeaderSize + 1, x + kRangeCellSize, _range_grid_rect.top - 2),
			DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		CPoint triangle[3] = {
			CPoint(x + kRangeCellSize / 2, _range_grid_rect.top - 2),
			CPoint(x + 5, _range_grid_rect.top - 12),
			CPoint(x + kRangeCellSize - 5, _range_grid_rect.top - 12)
		};
		dc->SelectObject(&triangle_brush);
		dc->Polygon(triangle, 3);
	}

	for (int row = 0; row < 13; ++row) {
		int y = _range_grid_rect.top + row * kRangeCellSize;
		CString rank;
		rank.Format("%c", ranks[row]);
		dc->SetTextColor(RGB(220, 220, 220));
		dc->DrawText(rank, CRect(_range_grid_rect.left - kRangeHeaderSize, y + 4, _range_grid_rect.left - 4, y + kRangeCellSize),
			DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		CPoint triangle[3] = {
			CPoint(_range_grid_rect.left - 2, y + kRangeCellSize / 2),
			CPoint(_range_grid_rect.left - 12, y + 5),
			CPoint(_range_grid_rect.left - 12, y + kRangeCellSize - 5)
		};
		dc->SelectObject(&triangle_brush);
		dc->Polygon(triangle, 3);
	}

	for (int row = 0; row < 13; ++row) {
		for (int col = 0; col < 13; ++col) {
			int index = row * 13 + col;
			CRect cell(_range_grid_rect.left + col * kRangeCellSize,
				_range_grid_rect.top + row * kRangeCellSize,
				_range_grid_rect.left + (col + 1) * kRangeCellSize,
				_range_grid_rect.top + (row + 1) * kRangeCellSize);
			bool enabled = _terminal->IsRangeCellEnabled(index);
			dc->SelectObject(enabled ? &enabled_brush : &disabled_brush);
			dc->Rectangle(cell);
			dc->SetTextColor(enabled ? RGB(255, 255, 255) : RGB(160, 160, 160));
			dc->DrawText(_terminal->RangeLabel(row, col), cell, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		}
	}

	dc->SelectObject(&border_pen);
	dc->SelectStockObject(NULL_BRUSH);
	dc->Rectangle(_range_grid_rect);
	dc->SetTextColor(old_text);
	dc->SetBkMode(old_mode);
	dc->SelectObject(old_pen);
	dc->SelectObject(old_brush);
	if (old_font != NULL) {
		dc->SelectObject(old_font);
	}
}

int COpponentRangeWindow::RangeCellFromPoint(CPoint point) const
{
	if (_range_grid_rect.IsRectEmpty() || !_range_grid_rect.PtInRect(point)) {
		return -1;
	}
	int col = (point.x - _range_grid_rect.left) / kRangeCellSize;
	int row = (point.y - _range_grid_rect.top) / kRangeCellSize;
	if (row < 0 || row >= 13 || col < 0 || col >= 13) {
		return -1;
	}
	return row * 13 + col;
}

int COpponentRangeWindow::RangeRowTriangleFromPoint(CPoint point) const
{
	CRect row_header(_range_grid_rect.left - kRangeHeaderSize, _range_grid_rect.top,
		_range_grid_rect.left, _range_grid_rect.bottom);
	if (!row_header.PtInRect(point)) {
		return -1;
	}
	int row = (point.y - _range_grid_rect.top) / kRangeCellSize;
	return row >= 0 && row < 13 ? row : -1;
}

int COpponentRangeWindow::RangeColumnTriangleFromPoint(CPoint point) const
{
	CRect column_header(_range_grid_rect.left, _range_grid_rect.top - kRangeHeaderSize,
		_range_grid_rect.right, _range_grid_rect.top);
	if (!column_header.PtInRect(point)) {
		return -1;
	}
	int col = (point.x - _range_grid_rect.left) / kRangeCellSize;
	return col >= 0 && col < 13 ? col : -1;
}

void COpponentRangeWindow::OnLButtonDown(UINT nFlags, CPoint point)
{
	if (_terminal != NULL) {
		int row = RangeRowTriangleFromPoint(point);
		if (row >= 0) {
			bool enable = false;
			for (int col = 0; col < 13; ++col) {
				if (!_terminal->IsRangeCellEnabled(row * 13 + col)) {
					enable = true;
					break;
				}
			}
			_terminal->SetRangeRow(row, enable);
			Invalidate(FALSE);
			return;
		}
		int col = RangeColumnTriangleFromPoint(point);
		if (col >= 0) {
			bool enable = false;
			for (int test_row = 0; test_row < 13; ++test_row) {
				if (!_terminal->IsRangeCellEnabled(test_row * 13 + col)) {
					enable = true;
					break;
				}
			}
			_terminal->SetRangeColumn(col, enable);
			Invalidate(FALSE);
			return;
		}
		int index = RangeCellFromPoint(point);
		if (index >= 0) {
			_range_dragging = true;
			_range_drag_value = !_terminal->IsRangeCellEnabled(index);
			_last_drag_range_index = -1;
			SetCapture();
			_terminal->SetRangeCell(index, _range_drag_value);
			_last_drag_range_index = index;
			Invalidate(FALSE);
			return;
		}
	}
	CWnd::OnLButtonDown(nFlags, point);
}

void COpponentRangeWindow::OnLButtonUp(UINT nFlags, CPoint point)
{
	if (_range_dragging) {
		_range_dragging = false;
		_last_drag_range_index = -1;
		if (GetCapture() == this) {
			ReleaseCapture();
		}
		return;
	}
	CWnd::OnLButtonUp(nFlags, point);
}

void COpponentRangeWindow::OnMouseMove(UINT nFlags, CPoint point)
{
	if (_range_dragging && _terminal != NULL) {
		int index = RangeCellFromPoint(point);
		if (index >= 0 && index != _last_drag_range_index) {
			_terminal->SetRangeCell(index, _range_drag_value);
			_last_drag_range_index = index;
			Invalidate(FALSE);
		}
		return;
	}
	CWnd::OnMouseMove(nFlags, point);
}

void COpponentRangeWindow::OnVpipChanged()
{
	if (_terminal == NULL) {
		return;
	}
	CString vpip_text;
	_vpip_input.GetWindowText(vpip_text);
	_terminal->ApplyVpipRange(vpip_text);
	_terminal->RefreshRangeOdds();
	Invalidate(FALSE);
}

IMPLEMENT_DYNAMIC(CChatTerminalWindow, CWnd)

BEGIN_MESSAGE_MAP(CChatTerminalWindow, CWnd)
	ON_WM_CREATE()
	ON_WM_SIZE()
	ON_WM_MOVING()
	ON_BN_CLICKED(IDC_TERMINAL_CLEAR, &CChatTerminalWindow::OnClearClicked)
	ON_BN_CLICKED(IDC_TERMINAL_SEND, &CChatTerminalWindow::OnSendClicked)
	ON_CBN_SELCHANGE(IDC_TERMINAL_SCREEN, &CChatTerminalWindow::OnScreenChanged)
	ON_COMMAND(ID_TERMINAL_FEATURE_POT_ODDS, &CChatTerminalWindow::OnFeaturePotOdds)
	ON_UPDATE_COMMAND_UI(ID_TERMINAL_FEATURE_POT_ODDS, &CChatTerminalWindow::OnUpdateFeaturePotOdds)
	ON_COMMAND(ID_TERMINAL_FEATURE_IMPLIED_POT_ODDS, &CChatTerminalWindow::OnFeatureImpliedPotOdds)
	ON_UPDATE_COMMAND_UI(ID_TERMINAL_FEATURE_IMPLIED_POT_ODDS, &CChatTerminalWindow::OnUpdateFeatureImpliedPotOdds)
	ON_COMMAND(ID_TERMINAL_FEATURE_REVERSE_IMPLIED_ODDS, &CChatTerminalWindow::OnFeatureReverseImpliedOdds)
	ON_UPDATE_COMMAND_UI(ID_TERMINAL_FEATURE_REVERSE_IMPLIED_ODDS, &CChatTerminalWindow::OnUpdateFeatureReverseImpliedOdds)
	ON_COMMAND(ID_TERMINAL_FEATURE_LOAD_HUD_PROFILE, &CChatTerminalWindow::OnFeatureLoadHudProfile)
	ON_COMMAND(ID_TERMINAL_EXTEND_BROWSER, &CChatTerminalWindow::OnExtendBrowser)
	ON_COMMAND(ID_TERMINAL_FEATURE_OPPONENT_RANGE, &CChatTerminalWindow::OnFeatureOpponentRange)
	ON_UPDATE_COMMAND_UI(ID_TERMINAL_FEATURE_OPPONENT_RANGE, &CChatTerminalWindow::OnUpdateFeatureOpponentRange)
	ON_EN_KILLFOCUS(IDC_TERMINAL_HOLE_CARDS, &CChatTerminalWindow::OnHoleCardsChanged)
	ON_MESSAGE(WM_CHAT_TERMINAL_APPEND, &CChatTerminalWindow::OnAppendMessage)
	ON_MESSAGE(WM_CHAT_TERMINAL_CLEAR, &CChatTerminalWindow::OnClearTerminal)
	ON_WM_CTLCOLOR()
	ON_WM_ERASEBKGND()
END_MESSAGE_MAP()

// Terminal palette.
static const COLORREF kTermBg    = RGB(0x0A, 0x0E, 0x12);  // near-black
static const COLORREF kTermText  = RGB(0x3D, 0xF5, 0x7A);  // phosphor green
static const COLORREF kTermLabel = RGB(0x7F, 0xD0, 0xFF);  // cyan-ish labels
static const COLORREF kTermInput = RGB(0xEA, 0xEA, 0xEA);  // near-white for input

HBRUSH CChatTerminalWindow::OnCtlColor(CDC *pDC, CWnd *pWnd, UINT nCtlColor) {
	if (_term_bg_brush.GetSafeHandle() == NULL) {
		_term_bg_brush.CreateSolidBrush(kTermBg);
	}
	// The 4 section displays are CVTermPane (self-drawn) -- no WM_CTLCOLOR. Only the
	// labels need dark styling here.
	for (int i = 0; i < kChatTerminalSectionCount; ++i) {
		if (pWnd == &_section_labels[i]) {
			pDC->SetTextColor(kTermLabel);
			pDC->SetBkColor(kTermBg);
			return (HBRUSH)_term_bg_brush.GetSafeHandle();
		}
	}
	// The chat / hole-cards input boxes: dark with light text.
	if (pWnd == &_chat_input || pWnd == &_hole_cards_input) {
		pDC->SetTextColor(kTermInput);
		pDC->SetBkColor(kTermBg);
		return (HBRUSH)_term_bg_brush.GetSafeHandle();
	}
	return CWnd::OnCtlColor(pDC, pWnd, nCtlColor);
}

BOOL CChatTerminalWindow::PreTranslateMessage(MSG *pMsg) {
	// Make the chat box behave like a command prompt: Enter sends, Up/Down recall.
	if (pMsg->message == WM_KEYDOWN && pMsg->hwnd == _chat_input.GetSafeHwnd()) {
		if (pMsg->wParam == VK_RETURN) {
			SendChatText();
			return TRUE;
		}
		if ((pMsg->wParam == VK_UP || pMsg->wParam == VK_DOWN) && !_input_history.empty()) {
			if (pMsg->wParam == VK_UP) {
				if (_history_index > 0) --_history_index;
			} else {
				if (_history_index < (int)_input_history.size()) ++_history_index;
			}
			if (_history_index >= (int)_input_history.size()) {
				_chat_input.SetWindowText("");
			} else {
				_chat_input.SetWindowText(_input_history[_history_index]);
			}
			int len = _chat_input.GetWindowTextLength();
			_chat_input.SetSel(len, len);
			return TRUE;
		}
	}
	return CWnd::PreTranslateMessage(pMsg);
}

BOOL CChatTerminalWindow::OnEraseBkgnd(CDC *pDC) {
	CRect rc;
	GetClientRect(&rc);
	CBrush bg(kTermBg);
	pDC->FillRect(&rc, &bg);
	return TRUE;
}


CChatTerminalWindow::CChatTerminalWindow()
{
	_owner = NULL;
	_attach_left = false;
	_active_screen = 0;
	_history_index = 0;
	_layout_ready = false;
	_pot_odds_enabled = false;
	_implied_pot_odds_enabled = false;
	_reverse_implied_odds_enabled = false;
	for (int i = 0; i < 169; ++i) {
		_range_enabled[i] = true;
	}
}

CChatTerminalWindow::~CChatTerminalWindow()
{
}

BOOL CChatTerminalWindow::Create(CWnd *owner)
{
	_owner = owner;
	CString class_name = AfxRegisterWndClass(
		CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW,
		AfxGetApp()->LoadStandardCursor(IDC_ARROW),
		(HBRUSH)(COLOR_WINDOW + 1),
		::LoadIcon(NULL, IDI_APPLICATION));

	DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
	DWORD ex_style = WS_EX_TOOLWINDOW;
	BOOL created = CWnd::CreateEx(
		ex_style,
		class_name,
		"Terminal",
		style,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		kTerminalWidth,
		kTerminalHeight,
		owner == NULL ? NULL : owner->GetSafeHwnd(),
		NULL);
	if (created) {
		AttachToOwner(true);
	}
	return created;
}

int CChatTerminalWindow::OnCreate(LPCREATESTRUCT lpCreateStruct)
{
	if (CWnd::OnCreate(lpCreateStruct) == -1) {
		return -1;
	}

	_title.Create("Terminal", WS_CHILD | WS_VISIBLE | SS_LEFT, CRect(0, 0, 0, 0), this);
	_clear_button.Create("Clear", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, CRect(0, 0, 0, 0), this, IDC_TERMINAL_CLEAR);
	_send_button.Create("Send", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, CRect(0, 0, 0, 0), this, IDC_TERMINAL_SEND);
	_screen_combo.Create(WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_TERMINAL_SCREEN);
	_hole_cards_label.Create("Hole cards", WS_CHILD | WS_VISIBLE | SS_LEFT, CRect(0, 0, 0, 0), this);
	_hole_cards_input.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, CRect(0, 0, 0, 0), this, IDC_TERMINAL_HOLE_CARDS);
	_chat_input.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, CRect(0, 0, 0, 0), this, IDC_TERMINAL_CHAT);

	_menu.CreateMenu();
	CMenu features_menu;
	features_menu.CreatePopupMenu();
	features_menu.AppendMenu(MF_STRING, ID_TERMINAL_FEATURE_POT_ODDS, "Enable Pot Odds Calculation");
	features_menu.AppendMenu(MF_STRING, ID_TERMINAL_FEATURE_IMPLIED_POT_ODDS, "Enable Implied Pot Odds");
	features_menu.AppendMenu(MF_STRING, ID_TERMINAL_FEATURE_REVERSE_IMPLIED_ODDS, "Enable Reverse Implied Odds");
	features_menu.AppendMenu(MF_STRING, ID_TERMINAL_FEATURE_OPPONENT_RANGE, "Show Opponent Range");
	features_menu.AppendMenu(MF_SEPARATOR);
	features_menu.AppendMenu(MF_STRING, ID_TERMINAL_FEATURE_LOAD_HUD_PROFILE, "Load HUD Profile...");
	features_menu.AppendMenu(MF_SEPARATOR);
	features_menu.AppendMenu(MF_STRING, ID_TERMINAL_EXTEND_BROWSER, "Extend this to your browser");
	_menu.AppendMenu(MF_POPUP, (UINT_PTR)features_menu.Detach(), "Features");
	SetMenu(&_menu);

	BuildRangeSelector();

	const char *labels[kChatTerminalSectionCount] = {
		"Context",
		"State",
		"Decisions",
		"Chat"
	};
	// Monospace font so the section displays read like a terminal.
	_terminal_font.CreateFont(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
	for (int i = 0; i < kChatTerminalSectionCount; ++i) {
		_section_labels[i].Create(labels[i], WS_CHILD | WS_VISIBLE | SS_LEFT, CRect(0, 0, 0, 0), this);
		// libvterm-backed terminal pane (double-buffered, flicker-free, ANSI/scrollback).
		_sections[i].Create(this, 25000 + i, CRect(0, 0, 0, 0));
		_sections[i].SetColors(RGB(0x0A, 0x0E, 0x12), RGB(0x3D, 0xF5, 0x7A));
		_section_labels[i].SetFont(&_terminal_font);
	}
	_chat_input.SetFont(&_terminal_font);
	_hole_cards_input.SetFont(&_terminal_font);

	_layout_ready = true;
	EnsureScreen("main");
	RefreshScreenList();
	LayoutControls(lpCreateStruct->cx, lpCreateStruct->cy);
	AppendToSection("main", kChatTerminalContext, "Terminal API ready.\r\n", true);
	AppendToSection("main", kChatTerminalChat, "Type steering notes here. The send hook is intentionally local for now.\r\n", true);
	return 0;
}

void CChatTerminalWindow::OnSize(UINT nType, int cx, int cy)
{
	CWnd::OnSize(nType, cx, cy);
	LayoutControls(cx, cy);
}

void CChatTerminalWindow::OnMoving(UINT fwSide, LPRECT pRect)
{
	CWnd::OnMoving(fwSide, pRect);
	if (_owner == NULL || !::IsWindow(_owner->GetSafeHwnd())) {
		return;
	}

	CRect owner_rect;
	_owner->GetWindowRect(&owner_rect);
	int terminal_center = (pRect->left + pRect->right) / 2;
	int owner_center = (owner_rect.left + owner_rect.right) / 2;
	_attach_left = terminal_center < owner_center;

	int width = pRect->right - pRect->left;
	int height = pRect->bottom - pRect->top;
	if (_attach_left) {
		pRect->right = owner_rect.left - kTerminalGap;
		pRect->left = pRect->right - width;
	}
	else {
		pRect->left = owner_rect.right + kTerminalGap;
		pRect->right = pRect->left + width;
	}
	pRect->top = owner_rect.top;
	pRect->bottom = pRect->top + height;
}

void CChatTerminalWindow::AttachToOwner(bool force)
{
	if (_owner == NULL || !::IsWindow(_owner->GetSafeHwnd()) || !::IsWindow(GetSafeHwnd())) {
		return;
	}

	CRect owner_rect, rect;
	_owner->GetWindowRect(&owner_rect);
	GetWindowRect(&rect);
	if (!force) {
		int terminal_center = rect.CenterPoint().x;
		int owner_center = owner_rect.CenterPoint().x;
		_attach_left = terminal_center < owner_center;
	}

	int x = _attach_left ? owner_rect.left - rect.Width() - kTerminalGap : owner_rect.right + kTerminalGap;
	SetWindowPos(NULL, x, owner_rect.top, rect.Width(), rect.Height(), SWP_NOZORDER | SWP_NOACTIVATE);
	if (::IsWindow(_opponent_range_window.GetSafeHwnd())) {
		_opponent_range_window.AttachToOwner();
	}
}

void CChatTerminalWindow::LayoutControls(int cx, int cy)
{
	if (!_layout_ready || cx <= 0 || cy <= 0) {
		return;
	}

	int left = kControlMargin;
	int right = cx - kControlMargin;
	int top = kControlMargin;
	_title.MoveWindow(left, top + 5, 145, 18);
	_screen_combo.MoveWindow(left + 150, top, 120, 120);
	_hole_cards_label.MoveWindow(left + 278, top + 5, 58, 18);
	_hole_cards_input.MoveWindow(left + 338, top, 48, 22);
	_clear_button.MoveWindow(right - 64, top, 64, 22);

	int grid_top = top + kTopHeight;
	int chat_top = cy - kControlMargin - kChatHeight;
	int grid_bottom = chat_top - kControlMargin;
	int grid_height = max(120, grid_bottom - grid_top);
	int col_gap = 6;
	int row_gap = 18;
	int col_width = (right - left - col_gap) / 2;
	int row_height = (grid_height - row_gap) / 2;

	for (int i = 0; i < kChatTerminalSectionCount; ++i) {
		int col = i % 2;
		int row = i / 2;
		int section_left = left + col * (col_width + col_gap);
		int section_top = grid_top + row * (row_height + row_gap);
		_section_labels[i].MoveWindow(section_left, section_top, col_width, 14);
		_sections[i].MoveWindow(section_left, section_top + 14, col_width, row_height - 14);
	}

	_chat_input.MoveWindow(left, chat_top, right - left - 72, 22);
	_send_button.MoveWindow(right - 64, chat_top, 64, 22);
}

void CChatTerminalWindow::AppendMessage(int section, CString text, bool stream)
{
	AppendMessage("main", section, text, stream);
}

void CChatTerminalWindow::AppendMessage(CString screen, int section, CString text, bool stream)
{
	if (!::IsWindow(GetSafeHwnd())) {
		return;
	}
	SChatTerminalMessage *message = new SChatTerminalMessage;
	message->screen = screen;
	message->section = section;
	message->text = text;
	message->stream = stream;
	message->clear_screen = false;
	message->set_pinned = false;
	PostMessage(WM_CHAT_TERMINAL_APPEND, 0, (LPARAM)message);
}

// Thread-safe: replace a screen's fixed (pinned) State block in place. Posts to
// the UI thread, so it's safe to call from the heartbeat/symbol-engine threads.
void CChatTerminalWindow::SetPinnedStateAsync(CString screen, CString text)
{
	if (!::IsWindow(GetSafeHwnd())) {
		return;
	}
	SChatTerminalMessage *message = new SChatTerminalMessage;
	message->screen = screen;
	message->section = kChatTerminalState;
	message->text = text;
	message->stream = false;
	message->clear_screen = false;
	message->set_pinned = true;
	PostMessage(WM_CHAT_TERMINAL_APPEND, 0, (LPARAM)message);
}

void CChatTerminalWindow::ClearTerminal(void)
{
	if (::IsWindow(GetSafeHwnd())) {
		PostMessage(WM_CHAT_TERMINAL_CLEAR, 0, 0);
	}
}

void CChatTerminalWindow::ClearScreen(CString screen)
{
	if (!::IsWindow(GetSafeHwnd())) {
		return;
	}
	SChatTerminalMessage *message = new SChatTerminalMessage;
	message->screen = screen;
	message->section = 0;
	message->stream = false;
	message->clear_screen = true;
	message->set_pinned = false;
	PostMessage(WM_CHAT_TERMINAL_APPEND, 0, (LPARAM)message);
}

int CChatTerminalWindow::EnsureScreen(CString screen)
{
	screen.Trim();
	if (screen.IsEmpty()) {
		screen = "main";
	}
	for (size_t i = 0; i < _screens.size(); ++i) {
		if (_screens[i].name.CompareNoCase(screen) == 0) {
			return (int)i;
		}
	}
	SChatTerminalScreen new_screen;
	new_screen.name = screen;
	_screens.push_back(new_screen);
	return (int)_screens.size() - 1;
}

void CChatTerminalWindow::RefreshScreenList(void)
{
	CString active_name;
	if (_active_screen >= 0 && _active_screen < (int)_screens.size()) {
		active_name = _screens[_active_screen].name;
	}
	_screen_combo.ResetContent();
	for (size_t i = 0; i < _screens.size(); ++i) {
		int item = _screen_combo.AddString(_screens[i].name);
		_screen_combo.SetItemData(item, (DWORD_PTR)i);
	}
	int selection = active_name.IsEmpty() ? 0 : _screen_combo.FindStringExact(-1, active_name);
	if (selection == CB_ERR) {
		selection = 0;
	}
	_screen_combo.SetCurSel(selection);
	if (selection != CB_ERR) {
		_active_screen = (int)_screen_combo.GetItemData(selection);
	}
}

void CChatTerminalWindow::RefreshSection(int i)
{
	if (_active_screen < 0 || _active_screen >= (int)_screens.size()) return;
	if (i < 0 || i >= kChatTerminalSectionCount) return;
	// Full re-render (used on screen switch): reset the terminal and replay.
	_sections[i].ResetTerminal();
	if (i == kChatTerminalState && !_screens[_active_screen].pinned_state.IsEmpty()) {
		_sections[i].SetScreenAnsi(_screens[_active_screen].pinned_state);  // in-place block
	} else {
		_sections[i].FeedAnsi(_screens[_active_screen].sections[i]);
	}
}

void CChatTerminalWindow::RefreshVisibleSections(void)
{
	for (int i = 0; i < kChatTerminalSectionCount; ++i) RefreshSection(i);
}

void CChatTerminalWindow::AppendToSection(CString screen, int section, CString text, bool stream)
{
	int screen_index = EnsureScreen(screen);
	if (section < 0 || section >= kChatTerminalSectionCount) {
		section = kChatTerminalContext;
	}
	if (!stream && text.Right(2) != "\r\n") {
		text += "\r\n";
	}
	_screens[screen_index].sections[section] += text;
	if (screen_index != _active_screen) {
		RefreshScreenList();
		return;
	}
	_sections[section].FeedAnsi(text);   // append to the terminal (scrolls)
	UpdateBrowserSnapshot();
}

void CChatTerminalWindow::SetPinnedState(CString screen, CString text)
{
	int screen_index = EnsureScreen(screen);
	if (!text.IsEmpty() && text.Right(2) != "\r\n") {
		text += "\r\n";
	}
	_screens[screen_index].pinned_state = text;
	if (screen_index == _active_screen) {
		// In-place "progress-bar" block: overwrite the State terminal screen.
		_sections[kChatTerminalState].SetScreenAnsi(text);
	}
	UpdateBrowserSnapshot();
}

void CChatTerminalWindow::UpdateBrowserSnapshot(void)
{
	if (_active_screen < 0 || _active_screen >= (int)_screens.size()) return;
	CSLock lock(g_browser_snap_cs);
	for (int i = 0; i < kChatTerminalSectionCount; ++i) {
		g_browser_sec[i] = _screens[_active_screen].sections[i];
	}
	g_browser_pinned = _screens[_active_screen].pinned_state;
}

void CChatTerminalWindow::OnExtendBrowser()
{
	unsigned short port = (p_chat_terminal_server != NULL) ? p_chat_terminal_server->port() : 27654;
	CString url;
	url.Format("http://127.0.0.1:%u/terminal/", (unsigned)port);
	::ShellExecute(GetSafeHwnd(), "open", url, NULL, NULL, SW_SHOWNORMAL);
}

// Called from the HTTP server thread when a command is typed in the browser
// prompt. Echo it into the Chat pane (thread-safe; AppendMessage posts to the UI).
void TerminalBrowserInject(const CString &cmd)
{
	if (p_chat_terminal == NULL || cmd.IsEmpty()) return;
	CString line;
	line.Format("\x1b[35m[web]\x1b[0m %s", cmd.GetString());
	p_chat_terminal->AppendMessage("main", kChatTerminalChat, line, false);
}

void CChatTerminalWindow::OnClearClicked()
{
	if (_active_screen >= 0 && _active_screen < (int)_screens.size()) {
		ClearScreen(_screens[_active_screen].name);
	}
}

void CChatTerminalWindow::OnSendClicked()
{
	SendChatText();
}

void CChatTerminalWindow::OnScreenChanged()
{
	int sel = _screen_combo.GetCurSel();
	if (sel != CB_ERR) {
		_active_screen = (int)_screen_combo.GetItemData(sel);
		RefreshVisibleSections();
		UpdateBrowserSnapshot();
	}
}

void CChatTerminalWindow::OnFeaturePotOdds()
{
	_pot_odds_enabled = !_pot_odds_enabled;
	_menu.ModifyMenu(ID_TERMINAL_FEATURE_POT_ODDS, MF_BYCOMMAND | MF_STRING,
		ID_TERMINAL_FEATURE_POT_ODDS,
		_pot_odds_enabled ? "Disable Pot Odds Calculation" : "Enable Pot Odds Calculation");
	if (!_pot_odds_enabled) {
		_last_pot_odds_board = "";
	}
	UpdatePotOddsForCurrentBoard(true);
}

void CChatTerminalWindow::OnUpdateFeaturePotOdds(CCmdUI *pCmdUI)
{
	pCmdUI->SetCheck(_pot_odds_enabled);
	pCmdUI->SetText(_pot_odds_enabled ? "Disable Pot Odds Calculation" : "Enable Pot Odds Calculation");
}

void CChatTerminalWindow::OnFeatureImpliedPotOdds()
{
	_implied_pot_odds_enabled = !_implied_pot_odds_enabled;
	_menu.ModifyMenu(ID_TERMINAL_FEATURE_IMPLIED_POT_ODDS, MF_BYCOMMAND | MF_STRING,
		ID_TERMINAL_FEATURE_IMPLIED_POT_ODDS,
		_implied_pot_odds_enabled ? "Disable Implied Pot Odds" : "Enable Implied Pot Odds");
	if (_implied_pot_odds_enabled) {
		ShowOpponentRangeWindow(true);
		UpdatePotOddsForCurrentBoard(true);
	}
	else {
		UpdatePotOddsForCurrentBoard(true);
	}
}

void CChatTerminalWindow::OnUpdateFeatureImpliedPotOdds(CCmdUI *pCmdUI)
{
	pCmdUI->SetCheck(_implied_pot_odds_enabled);
	pCmdUI->SetText(_implied_pot_odds_enabled ? "Disable Implied Pot Odds" : "Enable Implied Pot Odds");
}

void CChatTerminalWindow::OnFeatureReverseImpliedOdds()
{
	_reverse_implied_odds_enabled = !_reverse_implied_odds_enabled;
	_menu.ModifyMenu(ID_TERMINAL_FEATURE_REVERSE_IMPLIED_ODDS, MF_BYCOMMAND | MF_STRING,
		ID_TERMINAL_FEATURE_REVERSE_IMPLIED_ODDS,
		_reverse_implied_odds_enabled ? "Disable Reverse Implied Odds" : "Enable Reverse Implied Odds");
	if (_reverse_implied_odds_enabled) {
		ShowOpponentRangeWindow(true);
	}
	UpdatePotOddsForCurrentBoard(true);
}

void CChatTerminalWindow::OnUpdateFeatureReverseImpliedOdds(CCmdUI *pCmdUI)
{
	pCmdUI->SetCheck(_reverse_implied_odds_enabled);
	pCmdUI->SetText(_reverse_implied_odds_enabled ? "Disable Reverse Implied Odds" : "Enable Reverse Implied Odds");
}

void CChatTerminalWindow::OnFeatureLoadHudProfile()
{
	CFileDialog dialog(
		TRUE,
		NULL,
		NULL,
		OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
		"HUD Profiles (*.pt4hud;*.hud;*.txt;*.ini;*.csv)|*.pt4hud;*.hud;*.txt;*.ini;*.csv|All Files (*.*)|*.*||",
		this);
	if (dialog.DoModal() != IDOK) {
		return;
	}
	CString message;
	if (p_hud_manager != NULL && p_hud_manager->LoadProfile(dialog.GetPathName(), &message)) {
		AppendToSection("main", kChatTerminalContext, message, false);
		Invalidate();
	}
	else {
		if (message.IsEmpty()) {
			message = "Could not load HUD profile.";
		}
		AppendToSection("main", kChatTerminalContext, message, false);
	}
}

void CChatTerminalWindow::OnFeatureOpponentRange()
{
	ShowOpponentRangeWindow(!IsOpponentRangeWindowVisible());
}

void CChatTerminalWindow::OnUpdateFeatureOpponentRange(CCmdUI *pCmdUI)
{
	bool visible = IsOpponentRangeWindowVisible();
	pCmdUI->SetCheck(visible);
	pCmdUI->SetText(visible ? "Hide Opponent Range" : "Show Opponent Range");
}

void CChatTerminalWindow::OnHoleCardsChanged()
{
	if (_pot_odds_enabled || _implied_pot_odds_enabled || _reverse_implied_odds_enabled) {
		UpdatePotOddsForCurrentBoard(true);
	}
}

void CChatTerminalWindow::SendChatText(void)
{
	CString text;
	_chat_input.GetWindowText(text);
	text.Trim();
	if (text.IsEmpty()) {
		return;
	}
	_chat_input.SetWindowText("");
	// Remember the command for Up/Down recall (terminal-style history).
	_input_history.push_back(text);
	_history_index = (int)_input_history.size();
	CString line;
	line.Format("You: %s", text.GetString());
	CString screen = "main";
	if (_active_screen >= 0 && _active_screen < (int)_screens.size()) {
		screen = _screens[_active_screen].name;
	}
	AppendToSection(screen, kChatTerminalChat, line, false);
}

LRESULT CChatTerminalWindow::OnAppendMessage(WPARAM wParam, LPARAM lParam)
{
	SChatTerminalMessage *message = (SChatTerminalMessage *)lParam;
	if (message != NULL) {
		if (message->set_pinned) {
			// Fixed, in-place "progress-bar" block at the top of its section
			// (replaces, never accumulates).
			SetPinnedState(message->screen, message->text);
		}
		else if (message->clear_screen) {
			int screen_index = EnsureScreen(message->screen);
			for (int i = 0; i < kChatTerminalSectionCount; ++i) {
				_screens[screen_index].sections[i] = "";
			}
			if (screen_index == _active_screen) {
				RefreshVisibleSections();
			}
		}
		else {
			AppendToSection(message->screen, message->section, message->text, message->stream);
		}
		delete message;
	}
	return 0;
}

LRESULT CChatTerminalWindow::OnClearTerminal(WPARAM wParam, LPARAM lParam)
{
	for (int i = 0; i < kChatTerminalSectionCount; ++i) {
		_sections[i].ResetTerminal();
	}
	for (size_t s = 0; s < _screens.size(); ++s) {
		for (int i = 0; i < kChatTerminalSectionCount; ++i) {
			_screens[s].sections[i] = "";
			_screens[s].pinned_state = "";
		}
	}
	RefreshVisibleSections();
	return 0;
}

CString CChatTerminalWindow::CurrentCommunityCardsText(void)
{
	CString board;
	if (p_table_state == NULL) {
		return board;
	}
	for (int i = 0; i < kNumberOfCommunityCards; ++i) {
		Card *card = p_table_state->CommonCards(i);
		if (card != NULL && card->IsKnownCard()) {
			board += card->ToString();
		}
	}
	return board;
}

static int TerminalRankFromChar(char rank)
{
	rank = (char)toupper(rank);
	if (rank >= '2' && rank <= '9') return rank - '2';
	if (rank == 'T') return Rank_TEN;
	if (rank == 'J') return Rank_JACK;
	if (rank == 'Q') return Rank_QUEEN;
	if (rank == 'K') return Rank_KING;
	if (rank == 'A') return Rank_ACE;
	return -1;
}

static int TerminalSuitFromChar(char suit)
{
	suit = (char)tolower(suit);
	if (suit == 'h') return Suit_HEARTS;
	if (suit == 'd') return Suit_DIAMONDS;
	if (suit == 'c') return Suit_CLUBS;
	if (suit == 's') return Suit_SPADES;
	return -1;
}

static bool TerminalParseCard(CString text, int offset, int *card)
{
	if (card == NULL || offset + 1 >= text.GetLength()) {
		return false;
	}
	int rank = TerminalRankFromChar((char)text[offset]);
	int suit = TerminalSuitFromChar((char)text[offset + 1]);
	if (rank < 0 || suit < 0) {
		return false;
	}
	*card = StdDeck_MAKE_CARD(rank, suit);
	return true;
}

CString CChatTerminalWindow::CalculatePotOddsText(CString hole_cards, CString board_cards, bool use_range, bool reverse, CString label)
{
	hole_cards.Trim();
	hole_cards.Remove(' ');
	board_cards.Trim();
	board_cards.Remove(' ');
	if (hole_cards.GetLength() != 4) {
		return label + ": enter hole cards like AsTc";
	}

	int hero0 = -1, hero1 = -1;
	if (!TerminalParseCard(hole_cards, 0, &hero0) || !TerminalParseCard(hole_cards, 2, &hero1) || hero0 == hero1) {
		return label + ": invalid hole cards";
	}

	std::vector<int> board;
	for (int i = 0; i + 1 < board_cards.GetLength(); i += 2) {
		int card = -1;
		if (TerminalParseCard(board_cards, i, &card)) {
			board.push_back(card);
		}
	}
	if (board.size() < 3) {
		return label + ": waiting for community cards";
	}
	if (board.size() > 5) {
		return label + ": invalid board";
	}

	bool used[StdDeck_N_CARDS] = { false };
	used[hero0] = true;
	used[hero1] = true;
	for (size_t i = 0; i < board.size(); ++i) {
		if (used[board[i]]) {
			return label + ": duplicate card detected";
		}
		used[board[i]] = true;
	}

	CardMask hero_cards, board_mask;
	CardMask_RESET(hero_cards);
	CardMask_RESET(board_mask);
	CardMask_SET(hero_cards, hero0);
	CardMask_SET(hero_cards, hero1);
	for (size_t i = 0; i < board.size(); ++i) {
		CardMask_SET(board_mask, board[i]);
	}

	long wins = 0, ties = 0, losses = 0;
	long range_hands = 0;
	int missing_board = 5 - (int)board.size();
	for (int opp0 = 0; opp0 < StdDeck_N_CARDS; ++opp0) {
		if (used[opp0]) continue;
		used[opp0] = true;
		for (int opp1 = opp0 + 1; opp1 < StdDeck_N_CARDS; ++opp1) {
			if (used[opp1]) continue;
			if (use_range && !RangeAllowsOpponentHand(opp0, opp1)) continue;
			++range_hands;
			used[opp1] = true;

			CardMask opp_cards;
			CardMask_RESET(opp_cards);
			CardMask_SET(opp_cards, opp0);
			CardMask_SET(opp_cards, opp1);

			if (missing_board == 0) {
				CardMask hero_eval, opp_eval;
				CardMask_OR(hero_eval, hero_cards, board_mask);
				CardMask_OR(opp_eval, opp_cards, board_mask);
				HandVal hero_value = Hand_EVAL_N(hero_eval, 7);
				HandVal opp_value = Hand_EVAL_N(opp_eval, 7);
				if (hero_value > opp_value) ++wins;
				else if (hero_value == opp_value) ++ties;
				else ++losses;
			}
			else if (missing_board == 1) {
				for (int b0 = 0; b0 < StdDeck_N_CARDS; ++b0) {
					if (used[b0]) continue;
					CardMask runout, full_board, hero_eval, opp_eval;
					CardMask_RESET(runout);
					CardMask_SET(runout, b0);
					CardMask_OR(full_board, board_mask, runout);
					CardMask_OR(hero_eval, hero_cards, full_board);
					CardMask_OR(opp_eval, opp_cards, full_board);
					HandVal hero_value = Hand_EVAL_N(hero_eval, 7);
					HandVal opp_value = Hand_EVAL_N(opp_eval, 7);
					if (hero_value > opp_value) ++wins;
					else if (hero_value == opp_value) ++ties;
					else ++losses;
				}
			}
			else {
				for (int b0 = 0; b0 < StdDeck_N_CARDS; ++b0) {
					if (used[b0]) continue;
					used[b0] = true;
					for (int b1 = b0 + 1; b1 < StdDeck_N_CARDS; ++b1) {
						if (used[b1]) continue;
						CardMask runout, full_board, hero_eval, opp_eval;
						CardMask_RESET(runout);
						CardMask_SET(runout, b0);
						CardMask_SET(runout, b1);
						CardMask_OR(full_board, board_mask, runout);
						CardMask_OR(hero_eval, hero_cards, full_board);
						CardMask_OR(opp_eval, opp_cards, full_board);
						HandVal hero_value = Hand_EVAL_N(hero_eval, 7);
						HandVal opp_value = Hand_EVAL_N(opp_eval, 7);
						if (hero_value > opp_value) ++wins;
						else if (hero_value == opp_value) ++ties;
						else ++losses;
					}
					used[b0] = false;
				}
			}
			used[opp1] = false;
		}
		used[opp0] = false;
	}

	double total = (double)wins + (double)ties + (double)losses;
	if (total <= 0.0) {
		return label + ": no valid outcomes";
	}
	double equity = ((double)wins + (double)ties * 0.5) / total;
	double opponent_equity = ((double)losses + (double)ties * 0.5) / total;
	double displayed_equity = reverse ? opponent_equity : equity;
	double pot = p_engine_container == NULL ? 0.0 : p_engine_container->symbol_engine_chip_amounts()->pot();
	double call = p_engine_container == NULL ? 0.0 : p_engine_container->symbol_engine_chip_amounts()->call();
	CString text;
	if (call > 0.0) {
		double required = call / (pot + call);
		text.Format("%s: %s / %s | %s %.1f%% | required %.1f%% | call %.2f into %.2f",
			label.GetString(), hole_cards.GetString(), board_cards.GetString(),
			reverse ? "opponent equity" : "equity", displayed_equity * 100.0, required * 100.0, call, pot);
	}
	else {
		text.Format("%s: %s / %s | %s %.1f%% | no call due",
			label.GetString(), hole_cards.GetString(), board_cards.GetString(),
			reverse ? "opponent equity" : "equity", displayed_equity * 100.0);
	}
	if (use_range) {
		CString suffix;
		suffix.Format(" | range hands %ld", range_hands);
		text += suffix;
	}
	return text;
}

void CChatTerminalWindow::UpdatePotOddsForCurrentBoard(bool force)
{
	if (!_pot_odds_enabled && !_implied_pot_odds_enabled && !_reverse_implied_odds_enabled) {
		SetPinnedState("main", "");
		return;
	}
	CString board = CurrentCommunityCardsText();
	if (!force && board == _last_pot_odds_board) {
		return;
	}
	_last_pot_odds_board = board;
	CString hole_cards;
	_hole_cards_input.GetWindowText(hole_cards);
	CString state_text;
	if (_pot_odds_enabled) {
		state_text += CalculatePotOddsText(hole_cards, board, false, false, "Pot odds");
		state_text += "\r\n";
	}
	if (_implied_pot_odds_enabled) {
		state_text += CalculatePotOddsText(hole_cards, board, true, false, "Implied pot odds");
		state_text += "\r\n";
	}
	if (_reverse_implied_odds_enabled) {
		state_text += CalculatePotOddsText(hole_cards, board, true, true, "Reverse implied odds");
		state_text += "\r\n";
	}
	SetPinnedState("main", state_text);
}

void CChatTerminalWindow::MaybeUpdatePotOddsFromTableState(void)
{
	UpdatePotOddsForCurrentBoard(false);
}

void CChatTerminalWindow::BuildRangeSelector(void)
{
	for (int i = 0; i < 169; ++i) {
		_range_enabled[i] = true;
	}
}

void CChatTerminalWindow::ShowOpponentRangeWindow(bool visible)
{
	if (visible) {
		if (!::IsWindow(_opponent_range_window.GetSafeHwnd())) {
			_opponent_range_window.Create(_owner, this);
		}
		else {
			_opponent_range_window.AttachToOwner();
			_opponent_range_window.ShowWindow(SW_SHOW);
		}
	}
	else if (::IsWindow(_opponent_range_window.GetSafeHwnd())) {
		_opponent_range_window.ShowWindow(SW_HIDE);
	}
	_menu.ModifyMenu(ID_TERMINAL_FEATURE_OPPONENT_RANGE, MF_BYCOMMAND | MF_STRING,
		ID_TERMINAL_FEATURE_OPPONENT_RANGE,
		IsOpponentRangeWindowVisible() ? "Hide Opponent Range" : "Show Opponent Range");
}

bool CChatTerminalWindow::IsOpponentRangeWindowVisible(void) const
{
	return ::IsWindow(_opponent_range_window.GetSafeHwnd()) && _opponent_range_window.IsWindowVisible();
}

bool CChatTerminalWindow::IsRangeCellEnabled(int index) const
{
	return index >= 0 && index < 169 && _range_enabled[index];
}

void CChatTerminalWindow::SetRangeCell(int index, bool enabled)
{
	if (index < 0 || index >= 169) {
		return;
	}
	if (_range_enabled[index] == enabled) {
		return;
	}
	_range_enabled[index] = enabled;
	RefreshRangeOdds();
}

void CChatTerminalWindow::SetRangeRow(int row, bool enabled)
{
	if (row < 0 || row >= 13) {
		return;
	}
	for (int col = 0; col < 13; ++col) {
		_range_enabled[row * 13 + col] = enabled;
	}
	RefreshRangeOdds();
}

void CChatTerminalWindow::SetRangeColumn(int col, bool enabled)
{
	if (col < 0 || col >= 13) {
		return;
	}
	for (int row = 0; row < 13; ++row) {
		_range_enabled[row * 13 + col] = enabled;
	}
	RefreshRangeOdds();
}

void CChatTerminalWindow::RefreshRangeOdds(void)
{
	if (_implied_pot_odds_enabled || _reverse_implied_odds_enabled) {
		UpdatePotOddsForCurrentBoard(true);
	}
}

void CChatTerminalWindow::ApplyVpipRange(CString vpip_text)
{
	vpip_text.Trim();
	if (vpip_text.IsEmpty()) {
		return;
	}
	int vpip = atoi(vpip_text);
	if (vpip < 0) vpip = 0;
	if (vpip > 100) vpip = 100;
	int target_combos = (int)(1326.0 * (double)vpip / 100.0 + 0.5);

	struct SRangeChoice {
		int index;
		int score;
		int combos;
	};
	std::vector<SRangeChoice> choices;
	choices.reserve(169);
	for (int row = 0; row < 13; ++row) {
		for (int col = 0; col < 13; ++col) {
			SRangeChoice choice;
			choice.index = row * 13 + col;
			choice.score = RangeScore(row, col);
			choice.combos = RangeComboCount(row, col);
			choices.push_back(choice);
		}
	}
	std::sort(choices.begin(), choices.end(), [](const SRangeChoice &a, const SRangeChoice &b) {
		if (a.score != b.score) return a.score > b.score;
		return a.index < b.index;
	});

	for (int i = 0; i < 169; ++i) {
		_range_enabled[i] = false;
	}
	int selected_combos = 0;
	for (size_t i = 0; i < choices.size() && selected_combos < target_combos; ++i) {
		_range_enabled[choices[i].index] = true;
		selected_combos += choices[i].combos;
	}
}

int CChatTerminalWindow::RangeComboCount(int row, int col) const
{
	if (row == col) return 6;
	if (row < col) return 4;
	return 12;
}

int CChatTerminalWindow::RangeScore(int row, int col) const
{
	int high = 12 - min(row, col);
	int low = 12 - max(row, col);
	int gap = abs(row - col) - 1;
	if (gap < 0) gap = 0;
	int connected_bonus = max(0, 4 - gap) * 18;
	int broadway_bonus = (high >= 8 && low >= 8) ? 90 : 0;
	if (row == col) {
		return 10000 + high * 120;
	}
	if (row < col) {
		return 6000 + high * 120 + low * 34 + connected_bonus + broadway_bonus - gap * 22;
	}
	return 3000 + high * 115 + low * 28 + connected_bonus + broadway_bonus - gap * 28;
}

CString CChatTerminalWindow::RangeLabel(int row, int col)
{
	const char *ranks = "AKQJT98765432";
	CString label;
	if (row == col) {
		label.Format("%c%c", ranks[row], ranks[col]);
	}
	else if (row < col) {
		label.Format("%c%cs", ranks[row], ranks[col]);
	}
	else {
		label.Format("%c%co", ranks[col], ranks[row]);
	}
	return label;
}

bool CChatTerminalWindow::RangeAllowsOpponentHand(int first_card, int second_card)
{
	const int ace_high_row_from_rank[] = { 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
	int rank0 = StdDeck_RANK(first_card);
	int rank1 = StdDeck_RANK(second_card);
	int suit0 = StdDeck_SUIT(first_card);
	int suit1 = StdDeck_SUIT(second_card);
	if (rank0 == rank1) {
		int row = ace_high_row_from_rank[rank0];
		return _range_enabled[row * 13 + row];
	}

	int high_rank = rank0 > rank1 ? rank0 : rank1;
	int low_rank = rank0 > rank1 ? rank1 : rank0;
	int high_row = ace_high_row_from_rank[high_rank];
	int low_row = ace_high_row_from_rank[low_rank];
	int index = suit0 == suit1
		? high_row * 13 + low_row
		: low_row * 13 + high_row;
	return index >= 0 && index < 169 && _range_enabled[index];
}

void ChatTerminalAppend(int section, CString text)
{
	if (p_chat_terminal != NULL) {
		p_chat_terminal->AppendMessage(section, text, false);
	}
}

void ChatTerminalStream(int section, CString text)
{
	if (p_chat_terminal != NULL) {
		p_chat_terminal->AppendMessage(section, text, true);
	}
}

void ChatTerminalAppendToScreen(CString screen, int section, CString text)
{
	if (p_chat_terminal != NULL) {
		p_chat_terminal->AppendMessage(screen, section, text, false);
	}
}

void ChatTerminalStreamToScreen(CString screen, int section, CString text)
{
	if (p_chat_terminal != NULL) {
		p_chat_terminal->AppendMessage(screen, section, text, true);
	}
}

void ChatTerminalClear(void)
{
	if (p_chat_terminal != NULL) {
		p_chat_terminal->ClearTerminal();
	}
}

void ChatTerminalClearScreen(CString screen)
{
	if (p_chat_terminal != NULL) {
		p_chat_terminal->ClearScreen(screen);
	}
}
