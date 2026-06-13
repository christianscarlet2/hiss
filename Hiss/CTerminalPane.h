//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: A real terminal widget (custom-drawn), to replace the rich-edit
//   "console" boxes that flickered and blacked out under rapid updates.
//
//   Why custom: we own the rendering, so painting is a SINGLE double-buffered
//   blit (flicker-free), the control is never cleared to an empty/black state,
//   and we get exactly the terminal behaviours we want:
//     * monospace, dark background, per-run ANSI SGR colour + bold
//     * a scrollback log you can scroll/wheel through
//     * a FIXED "pinned" header block that updates in place (progress-bar style)
//     * auto-scroll that sticks to the bottom, pauses when you scroll up, and
//       resumes the moment you scroll back to the bottom
//
//******************************************************************************

#ifndef INC_CTERMINALPANE_H
#define INC_CTERMINALPANE_H

#include <vector>
#include <deque>

class CTerminalPane : public CWnd {
 public:
  CTerminalPane();
  virtual ~CTerminalPane();

  BOOL Create(CWnd *parent, UINT id, const CRect &rc);
  void SetColors(COLORREF bg, COLORREF fg);

  // Append text (may contain ANSI SGR escapes) to the scrolling log.
  void AppendAnsi(const CString &text);
  // Replace the fixed pinned header block (rendered above the log, in place).
  void SetPinnedAnsi(const CString &text);
  // Replace the whole scrolling log with this text (used on screen switch).
  void SetLogAnsi(const CString &text);
  void ClearLog();

 protected:
  afx_msg void OnPaint();
  afx_msg BOOL OnEraseBkgnd(CDC *pDC);
  afx_msg void OnSize(UINT nType, int cx, int cy);
  afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
  afx_msg void OnVScroll(UINT nSBCode, UINT nPos, CScrollBar *pScrollBar);
  afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
  DECLARE_MESSAGE_MAP()

 private:
  struct Run { CString text; COLORREF color; bool bold; };
  typedef std::vector<Run> Line;

  void EnsureFonts();
  void ParseAnsiToLines(const CString &text, std::vector<Line> &out);
  int  VisibleLogLines() const;
  int  MaxScroll() const;
  void UpdateScrollBar();
  void DrawLine(CDC &dc, const Line &line, int x, int y);

  std::deque<Line>  _log;       // scrollback (oldest .. newest)
  std::vector<Line> _pinned;    // fixed header lines (replaced in place)
  int      _scroll;             // index of the first visible log line
  bool     _stick;             // auto-scroll: keep the newest line in view
  COLORREF _bg, _fg;
  CFont    _font, _font_bold;
  int      _line_h;
  size_t   _max_lines;
};

#endif  // INC_CTERMINALPANE_H
