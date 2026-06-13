//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************

#include "stdafx.h"
#include "CTerminalPane.h"

BEGIN_MESSAGE_MAP(CTerminalPane, CWnd)
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
	ON_WM_SIZE()
	ON_WM_MOUSEWHEEL()
	ON_WM_VSCROLL()
	ON_WM_CREATE()
END_MESSAGE_MAP()

CTerminalPane::CTerminalPane()
	: _scroll(0), _stick(true),
	  _bg(RGB(0x0A, 0x0E, 0x12)), _fg(RGB(0x3D, 0xF5, 0x7A)),
	  _line_h(15), _max_lines(8000) {
}

CTerminalPane::~CTerminalPane() {}

BOOL CTerminalPane::Create(CWnd *parent, UINT id, const CRect &rc) {
	CString cls = AfxRegisterWndClass(CS_HREDRAW | CS_VREDRAW,
		::LoadCursor(NULL, IDC_ARROW), NULL, NULL);
	return CWnd::CreateEx(0, cls, "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL,
		rc, parent, id);
}

int CTerminalPane::OnCreate(LPCREATESTRUCT lpCreateStruct) {
	if (CWnd::OnCreate(lpCreateStruct) == -1) return -1;
	EnsureFonts();
	return 0;
}

void CTerminalPane::SetColors(COLORREF bg, COLORREF fg) {
	_bg = bg; _fg = fg;
	if (::IsWindow(GetSafeHwnd())) Invalidate(FALSE);
}

void CTerminalPane::EnsureFonts() {
	if (_font.GetSafeHandle() != NULL) return;
	_font.CreateFont(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
	_font_bold.CreateFont(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
	// Measure line height.
	CDC *dc = GetDC();
	if (dc != NULL) {
		CFont *old = dc->SelectObject(&_font);
		TEXTMETRIC tm; dc->GetTextMetrics(&tm);
		_line_h = tm.tmHeight + tm.tmExternalLeading + 1;
		dc->SelectObject(old);
		ReleaseDC(dc);
	}
}

// Parse ANSI SGR escapes (\x1b[...m) into coloured/bold runs, splitting on \n.
void CTerminalPane::ParseAnsiToLines(const CString &text, std::vector<Line> &out) {
	out.clear();
	Line cur_line;
	COLORREF color = _fg;
	bool bold = false;
	CString run;
	int i = 0, n = text.GetLength();
	while (i < n) {
		char c = (char)text[i];
		if (c == 27 && (i + 1) < n && text[i + 1] == '[') {           // ESC '['
			if (!run.IsEmpty()) { Run r; r.text = run; r.color = color; r.bold = bold; cur_line.push_back(r); run.Empty(); }
			int j = i + 2;
			CString params;
			while (j < n && (text[j] == ';' || (text[j] >= '0' && text[j] <= '9'))) { params += text[j]; ++j; }
			if (j < n && text[j] == 'm') {
				int start = 0;
				while (start <= params.GetLength()) {
					int semi = params.Find(';', start);
					CString tok = (semi < 0) ? params.Mid(start) : params.Mid(start, semi - start);
					int code = atoi(tok.GetString());
					switch (code) {
						case 0:  color = _fg; bold = false; break;
						case 1:  bold = true; break;
						case 22: bold = false; break;
						case 39: color = _fg; break;
						case 30: color = RGB(0x6A,0x70,0x7A); break;
						case 31: case 91: color = RGB(0xFF,0x55,0x55); break;
						case 32: case 92: color = RGB(0x50,0xFA,0x7B); break;
						case 33: case 93: color = RGB(0xF1,0xFA,0x8C); break;
						case 34: case 94: color = RGB(0x6C,0xB6,0xFF); break;
						case 35: case 95: color = RGB(0xFF,0x79,0xC6); break;
						case 36: case 96: color = RGB(0x8B,0xE9,0xFD); break;
						case 37: case 97: color = RGB(0xF8,0xF8,0xF2); break;
						default: break;
					}
					if (semi < 0) break;
					start = semi + 1;
				}
				i = j + 1;
			} else {
				i = (j < n) ? (j + 1) : n;
			}
			continue;
		}
		if (c == '\r') { ++i; continue; }
		if (c == '\n') {
			if (!run.IsEmpty()) { Run r; r.text = run; r.color = color; r.bold = bold; cur_line.push_back(r); run.Empty(); }
			out.push_back(cur_line);
			cur_line.clear();
			++i;
			continue;
		}
		run += c;
		++i;
	}
	if (!run.IsEmpty()) { Run r; r.text = run; r.color = color; r.bold = bold; cur_line.push_back(r); }
	if (!cur_line.empty() || out.empty()) out.push_back(cur_line);
}

int CTerminalPane::VisibleLogLines() const {
	CRect rc; GetClientRect(&rc);
	int avail = rc.Height() - 2 - (int)_pinned.size() * _line_h;
	if (avail < 0) avail = 0;
	int v = avail / (_line_h > 0 ? _line_h : 15);
	return v < 0 ? 0 : v;
}

int CTerminalPane::MaxScroll() const {
	int over = (int)_log.size() - VisibleLogLines();
	return over > 0 ? over : 0;
}

void CTerminalPane::UpdateScrollBar() {
	SCROLLINFO si; ZeroMemory(&si, sizeof(si));
	si.cbSize = sizeof(si);
	si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
	si.nMin = 0;
	si.nMax = (int)_log.size() > 0 ? (int)_log.size() - 1 : 0;
	si.nPage = VisibleLogLines();
	si.nPos = _scroll;
	SetScrollInfo(SB_VERT, &si, TRUE);
}

void CTerminalPane::ClearLog() {
	_log.clear();
	_scroll = 0;
	_stick = true;
	if (::IsWindow(GetSafeHwnd())) { UpdateScrollBar(); Invalidate(FALSE); }
}

void CTerminalPane::SetPinnedAnsi(const CString &text) {
	ParseAnsiToLines(text, _pinned);
	if (text.IsEmpty()) _pinned.clear();
	if (::IsWindow(GetSafeHwnd())) { UpdateScrollBar(); Invalidate(FALSE); }
}

void CTerminalPane::SetLogAnsi(const CString &text) {
	_log.clear();
	if (!text.IsEmpty()) {
		std::vector<Line> lines;
		ParseAnsiToLines(text, lines);
		for (size_t k = 0; k < lines.size(); ++k) _log.push_back(lines[k]);
		while (_log.size() > _max_lines) _log.pop_front();
	}
	_stick = true;
	_scroll = MaxScroll();
	if (::IsWindow(GetSafeHwnd())) { UpdateScrollBar(); Invalidate(FALSE); }
}

void CTerminalPane::AppendAnsi(const CString &text) {
	if (text.IsEmpty()) return;
	std::vector<Line> lines;
	ParseAnsiToLines(text, lines);
	for (size_t k = 0; k < lines.size(); ++k) _log.push_back(lines[k]);
	while (_log.size() > _max_lines) _log.pop_front();
	if (_stick) _scroll = MaxScroll();
	if (::IsWindow(GetSafeHwnd())) { UpdateScrollBar(); Invalidate(FALSE); }
}

void CTerminalPane::DrawLine(CDC &dc, const Line &line, int x, int y) {
	for (size_t r = 0; r < line.size(); ++r) {
		const Run &run = line[r];
		if (run.text.IsEmpty()) continue;
		CFont *old = dc.SelectObject(run.bold ? &_font_bold : &_font);
		dc.SetTextColor(run.color);
		dc.TextOut(x, y, run.text);
		CSize sz = dc.GetTextExtent(run.text);
		x += sz.cx;
		dc.SelectObject(old);
	}
}

BOOL CTerminalPane::OnEraseBkgnd(CDC *) {
	return TRUE;   // all painting happens in OnPaint (double-buffered) -> no flicker
}

void CTerminalPane::OnPaint() {
	CPaintDC paint(this);
	CRect rc; GetClientRect(&rc);
	if (rc.Width() <= 0 || rc.Height() <= 0) return;
	EnsureFonts();

	// Double buffer.
	CDC mem; mem.CreateCompatibleDC(&paint);
	CBitmap bmp; bmp.CreateCompatibleBitmap(&paint, rc.Width(), rc.Height());
	CBitmap *oldbmp = mem.SelectObject(&bmp);
	mem.FillSolidRect(rc, _bg);
	mem.SetBkMode(TRANSPARENT);

	int y = 1;
	const int x = 3;
	// Pinned header (fixed, always shown at top).
	for (size_t i = 0; i < _pinned.size(); ++i) { DrawLine(mem, _pinned[i], x, y); y += _line_h; }
	// Scrolling log below the pinned block.
	int visible = VisibleLogLines();
	int start = _scroll;
	if (start < 0) start = 0;
	for (int k = 0; k < visible && (start + k) < (int)_log.size(); ++k) {
		DrawLine(mem, _log[start + k], x, y);
		y += _line_h;
	}

	paint.BitBlt(0, 0, rc.Width(), rc.Height(), &mem, 0, 0, SRCCOPY);
	mem.SelectObject(oldbmp);
}

void CTerminalPane::OnSize(UINT nType, int cx, int cy) {
	CWnd::OnSize(nType, cx, cy);
	if (_stick) _scroll = MaxScroll();
	UpdateScrollBar();
	Invalidate(FALSE);
}

BOOL CTerminalPane::OnMouseWheel(UINT, short zDelta, CPoint) {
	int lines = zDelta / WHEEL_DELTA;        // +up / -down
	_scroll -= lines * 3;
	if (_scroll < 0) _scroll = 0;
	if (_scroll > MaxScroll()) _scroll = MaxScroll();
	_stick = (_scroll >= MaxScroll());        // resume follow when back at bottom
	UpdateScrollBar();
	Invalidate(FALSE);
	return TRUE;
}

void CTerminalPane::OnVScroll(UINT nSBCode, UINT nPos, CScrollBar *pScrollBar) {
	int s = _scroll;
	int page = VisibleLogLines();
	switch (nSBCode) {
		case SB_LINEUP:   s -= 1; break;
		case SB_LINEDOWN: s += 1; break;
		case SB_PAGEUP:   s -= page; break;
		case SB_PAGEDOWN: s += page; break;
		case SB_THUMBTRACK:
		case SB_THUMBPOSITION: s = (int)nPos; break;
		case SB_TOP:      s = 0; break;
		case SB_BOTTOM:   s = MaxScroll(); break;
		default: break;
	}
	if (s < 0) s = 0;
	if (s > MaxScroll()) s = MaxScroll();
	_scroll = s;
	_stick = (_scroll >= MaxScroll());        // at bottom -> resume auto-scroll
	UpdateScrollBar();
	Invalidate(FALSE);
	CWnd::OnVScroll(nSBCode, nPos, pScrollBar);
}
