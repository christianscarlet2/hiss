//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************

#include "stdafx.h"
#include "CVTermPane.h"

extern "C" {
#include "vterm.h"
}

BEGIN_MESSAGE_MAP(CVTermPane, CWnd)
	ON_WM_CREATE()
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
	ON_WM_SIZE()
	ON_WM_MOUSEWHEEL()
	ON_WM_VSCROLL()
	ON_WM_DESTROY()
END_MESSAGE_MAP()

// ---- C callback thunks -> instance --------------------------------------------
static int cb_damage(VTermRect, void *user) { ((CVTermPane *)user)->OnDamage(); return 1; }
static int cb_moverect(VTermRect, VTermRect, void *user) { ((CVTermPane *)user)->OnDamage(); return 1; }
static int cb_movecursor(VTermPos, VTermPos, int, void *) { return 1; }
static int cb_settermprop(VTermProp, VTermValue *, void *) { return 1; }
static int cb_bell(void *) { return 1; }
static int cb_resize(int, int, void *) { return 1; }
static int cb_sb_pushline(int cols, const VTermScreenCell *cells, void *user) { return ((CVTermPane *)user)->OnPushline(cols, cells); }
static int cb_sb_popline(int cols, VTermScreenCell *cells, void *user) { return ((CVTermPane *)user)->OnPopline(cols, cells); }
static int cb_sb_clear(void *) { return 1; }

static VTermScreenCallbacks g_screen_cbs = {
	cb_damage, cb_moverect, cb_movecursor, cb_settermprop,
	cb_bell, cb_resize, cb_sb_pushline, cb_sb_popline, cb_sb_clear, NULL
};

CVTermPane::CVTermPane()
	: _vt(NULL), _screen(NULL), _char_w(8), _line_h(15), _rows(24), _cols(80),
	  _bg(RGB(0x0A, 0x0E, 0x12)), _fg(RGB(0x3D, 0xF5, 0x7A)),
	  _max_sb(8000), _scroll_off(0), _stick(true), _is_screen_mode(false) {
}

CVTermPane::~CVTermPane() {
	if (_vt) { vterm_free(_vt); _vt = NULL; }
}

BOOL CVTermPane::Create(CWnd *parent, UINT id, const CRect &rc) {
	CString cls = AfxRegisterWndClass(CS_HREDRAW | CS_VREDRAW,
		::LoadCursor(NULL, IDC_IBEAM), NULL, NULL);
	return CWnd::CreateEx(0, cls, "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL, rc, parent, id);
}

void CVTermPane::SetColors(COLORREF bg, COLORREF fg) {
	_bg = bg; _fg = fg;
	if (_vt) ApplyDefaultColors();
	if (::IsWindow(GetSafeHwnd())) Invalidate(FALSE);
}

void CVTermPane::EnsureFonts() {
	if (_font.GetSafeHandle() != NULL) return;
	_font.CreateFont(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
	_font_bold.CreateFont(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
	CDC *dc = GetDC();
	if (dc) {
		CFont *old = dc->SelectObject(&_font);
		TEXTMETRIC tm; dc->GetTextMetrics(&tm);
		_line_h = tm.tmHeight + tm.tmExternalLeading + 1;
		_char_w = tm.tmAveCharWidth;
		dc->SelectObject(old);
		ReleaseDC(dc);
	}
	if (_char_w < 1) _char_w = 8;
	if (_line_h < 1) _line_h = 15;
}

void CVTermPane::ApplyDefaultColors() {
	if (_vt == NULL) return;
	VTermState *state = vterm_obtain_state(_vt);
	VTermColor fg, bg;
	vterm_color_rgb(&fg, GetRValue(_fg), GetGValue(_fg), GetBValue(_fg));
	vterm_color_rgb(&bg, GetRValue(_bg), GetGValue(_bg), GetBValue(_bg));
	vterm_state_set_default_colors(state, &fg, &bg);
}

void CVTermPane::RebuildVTerm() {
	CRect rc; GetClientRect(&rc);
	int cols = (rc.Width() - 4) / _char_w;   if (cols < 1) cols = 1;
	int rows = (rc.Height() - 2) / _line_h;  if (rows < 1) rows = 1;
	_cols = cols; _rows = rows;
	if (_vt) { vterm_free(_vt); _vt = NULL; }
	_vt = vterm_new(rows, cols);
	vterm_set_utf8(_vt, 1);
	_screen = vterm_obtain_screen(_vt);
	vterm_screen_set_callbacks(_screen, &g_screen_cbs, this);
	vterm_screen_enable_altscreen(_screen, 0);
	ApplyDefaultColors();
	vterm_screen_reset(_screen, 1);
}

int CVTermPane::OnCreate(LPCREATESTRUCT lpCreateStruct) {
	if (CWnd::OnCreate(lpCreateStruct) == -1) return -1;
	EnsureFonts();
	RebuildVTerm();
	return 0;
}

void CVTermPane::OnDestroy() {
	if (_vt) { vterm_free(_vt); _vt = NULL; _screen = NULL; }
	CWnd::OnDestroy();
}

void CVTermPane::FeedAnsi(const CString &text) {
	if (_vt == NULL || text.IsEmpty()) return;
	// Switching to append/scroll mode: a pinned screen block no longer applies.
	_is_screen_mode = false;
	_screen_text.Empty();
	CStringA a(text);
	vterm_input_write(_vt, a.GetString(), a.GetLength());
	if (_stick) _scroll_off = 0;
	UpdateScrollBar();
	if (::IsWindow(GetSafeHwnd())) Invalidate(FALSE);
}

// Home + clear screen + clear scrollback, then write _screen_text -> overwrites in
// place. Used both by SetScreenAnsi() and after any reflow (resize/rebuild) so the
// fixed block can never be left blank by a layout pass.
void CVTermPane::WriteScreenText() {
	if (_vt == NULL) return;
	static const char kHomeClear[] = "\x1b[H\x1b[2J";
	vterm_input_write(_vt, kHomeClear, (size_t)(sizeof(kHomeClear) - 1));
	CStringA a(_screen_text);
	if (a.GetLength() > 0) vterm_input_write(_vt, a.GetString(), a.GetLength());
	_scrollback.clear();
	_scroll_off = 0;
	_stick = true;
}

void CVTermPane::SetScreenAnsi(const CString &text) {
	if (_vt == NULL) return;
	_is_screen_mode = true;
	_screen_text = text;
	WriteScreenText();
	UpdateScrollBar();
	if (::IsWindow(GetSafeHwnd())) Invalidate(FALSE);
}

void CVTermPane::ResetTerminal() {
	_is_screen_mode = false;
	_screen_text.Empty();
	_scrollback.clear();
	_scroll_off = 0;
	_stick = true;
	if (_screen) vterm_screen_reset(_screen, 1);
	if (::IsWindow(GetSafeHwnd())) { UpdateScrollBar(); Invalidate(FALSE); }
}

void CVTermPane::ResolveCell(const void *vcell, SbCell *out) const {
	const VTermScreenCell *c = (const VTermScreenCell *)vcell;
	out->ch = (c->chars[0] == 0) ? ' ' : c->chars[0];
	out->bold = c->attrs.bold != 0;
	out->reverse = c->attrs.reverse != 0;
	VTermColor fg = c->fg, bg = c->bg;
	if (VTERM_COLOR_IS_DEFAULT_FG(&fg)) out->fg = _fg;
	else { vterm_screen_convert_color_to_rgb(_screen, &fg); out->fg = RGB(fg.rgb.red, fg.rgb.green, fg.rgb.blue); }
	if (VTERM_COLOR_IS_DEFAULT_BG(&bg)) out->bg = _bg;
	else { vterm_screen_convert_color_to_rgb(_screen, &bg); out->bg = RGB(bg.rgb.red, bg.rgb.green, bg.rgb.blue); }
}

int CVTermPane::OnPushline(int cols, const void *cells) {
	const VTermScreenCell *arr = (const VTermScreenCell *)cells;
	SbLine line;
	line.reserve(cols);
	for (int i = 0; i < cols; ++i) {
		SbCell sc; ResolveCell(&arr[i], &sc);
		line.push_back(sc);
	}
	_scrollback.push_back(line);
	while (_scrollback.size() > _max_sb) _scrollback.pop_front();
	return 1;
}

int CVTermPane::OnPopline(int cols, void *cells) {
	// Restore a line back onto the screen when it scrolls down. Rarely needed for
	// our append-only logs; return 0 so libvterm fills with blanks.
	(void)cols; (void)cells;
	return 0;
}

void CVTermPane::OnDamage() {
	if (_stick) _scroll_off = 0;
	if (::IsWindow(GetSafeHwnd())) Invalidate(FALSE);
}

int CVTermPane::VisibleRows() const {
	CRect rc; GetClientRect(&rc);
	int v = (rc.Height() - 2) / (_line_h > 0 ? _line_h : 15);
	return v < 1 ? 1 : v;
}

int CVTermPane::MaxScroll() const {
	return TotalHistory();   // can scroll up through all the history
}

void CVTermPane::UpdateScrollBar() {
	SCROLLINFO si; ZeroMemory(&si, sizeof(si));
	si.cbSize = sizeof(si);
	si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
	int total = TotalHistory() + _rows;
	si.nMin = 0;
	si.nMax = total > 0 ? total - 1 : 0;
	si.nPage = VisibleRows();
	// scrollbar pos counts from the top; _scroll_off counts up from bottom.
	int pos = TotalHistory() - _scroll_off;
	if (pos < 0) pos = 0;
	si.nPos = pos;
	if (::IsWindow(GetSafeHwnd())) SetScrollInfo(SB_VERT, &si, TRUE);
}

BOOL CVTermPane::OnEraseBkgnd(CDC *) { return TRUE; }

void CVTermPane::OnPaint() {
	CPaintDC paint(this);
	CRect rc; GetClientRect(&rc);
	if (rc.Width() <= 0 || rc.Height() <= 0 || _vt == NULL) return;
	EnsureFonts();

	CDC mem; mem.CreateCompatibleDC(&paint);
	CBitmap bmp; bmp.CreateCompatibleBitmap(&paint, rc.Width(), rc.Height());
	CBitmap *oldbmp = mem.SelectObject(&bmp);
	mem.FillSolidRect(rc, _bg);
	mem.SetBkMode(TRANSPARENT);

	int visible = VisibleRows();
	int total_hist = TotalHistory();
	for (int screen_row = 0; screen_row < visible; ++screen_row) {
		int line_index = (total_hist - _scroll_off) + screen_row;   // index into [history|live]
		if (line_index < 0) continue;
		int y = 1 + screen_row * _line_h;
		int col = 0;
		while (col < _cols) {
			SbCell cell;
			int span_w = 1;
			if (line_index < total_hist) {
				const SbLine &sl = _scrollback[line_index];
				if (col >= (int)sl.size()) break;
				cell = sl[col];
			} else {
				int live_row = line_index - total_hist;
				if (live_row >= _rows) break;
				VTermPos pos; pos.row = live_row; pos.col = col;
				VTermScreenCell vc;
				if (!vterm_screen_get_cell(_screen, pos, &vc)) { ++col; continue; }
				span_w = (vc.width > 0) ? vc.width : 1;
				ResolveCell(&vc, &cell);
			}
			COLORREF fg = cell.reverse ? cell.bg : cell.fg;
			COLORREF bg = cell.reverse ? cell.fg : cell.bg;
			int x = 3 + col * _char_w;
			if (bg != _bg) mem.FillSolidRect(x, y, _char_w * span_w, _line_h, bg);
			if (cell.ch != ' ' && cell.ch != 0) {
				CFont *old = mem.SelectObject(cell.bold ? &_font_bold : &_font);
				mem.SetTextColor(fg);
				WCHAR wch[2] = { (WCHAR)cell.ch, 0 };
				::TextOutW(mem.GetSafeHdc(), x, y, wch, 1);
				mem.SelectObject(old);
			}
			col += span_w;
		}
	}

	paint.BitBlt(0, 0, rc.Width(), rc.Height(), &mem, 0, 0, SRCCOPY);
	mem.SelectObject(oldbmp);
}

void CVTermPane::OnSize(UINT nType, int cx, int cy) {
	CWnd::OnSize(nType, cx, cy);
	if (_vt == NULL || _char_w < 1 || _line_h < 1) return;
	int cols = (cx - 4) / _char_w;   if (cols < 1) cols = 1;
	int rows = (cy - 2) / _line_h;   if (rows < 1) rows = 1;
	if (cols != _cols || rows != _rows) {
		_cols = cols; _rows = rows;
		vterm_set_size(_vt, rows, cols);
		// A reflow can scroll/blank the live screen. If this pane is showing a
		// pinned fixed block, re-write it so the resize can't leave it blank.
		if (_is_screen_mode) WriteScreenText();
	}
	if (_stick) _scroll_off = 0;
	UpdateScrollBar();
	Invalidate(FALSE);
}

BOOL CVTermPane::OnMouseWheel(UINT, short zDelta, CPoint) {
	int lines = (zDelta / WHEEL_DELTA) * 3;
	_scroll_off += lines;                 // wheel up -> scroll back into history
	if (_scroll_off < 0) _scroll_off = 0;
	if (_scroll_off > MaxScroll()) _scroll_off = MaxScroll();
	_stick = (_scroll_off == 0);
	UpdateScrollBar();
	Invalidate(FALSE);
	return TRUE;
}

void CVTermPane::OnVScroll(UINT nSBCode, UINT nPos, CScrollBar *pScrollBar) {
	int page = VisibleRows();
	int pos = TotalHistory() - _scroll_off;   // current top position
	switch (nSBCode) {
		case SB_LINEUP:   pos -= 1; break;
		case SB_LINEDOWN: pos += 1; break;
		case SB_PAGEUP:   pos -= page; break;
		case SB_PAGEDOWN: pos += page; break;
		case SB_THUMBTRACK:
		case SB_THUMBPOSITION: pos = (int)nPos; break;
		case SB_TOP:    pos = 0; break;
		case SB_BOTTOM: pos = TotalHistory(); break;
		default: break;
	}
	_scroll_off = TotalHistory() - pos;
	if (_scroll_off < 0) _scroll_off = 0;
	if (_scroll_off > MaxScroll()) _scroll_off = MaxScroll();
	_stick = (_scroll_off == 0);
	UpdateScrollBar();
	Invalidate(FALSE);
	CWnd::OnVScroll(nSBCode, nPos, pScrollBar);
}
