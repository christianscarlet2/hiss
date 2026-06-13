//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: A real terminal widget backed by libvterm (a full VT100/xterm engine).
//
//   libvterm parses the byte stream and maintains the screen + scrollback; this
//   CWnd renders the screen cells with a SINGLE double-buffered blit per paint
//   (flicker-free, never blank), in a monospace font, honouring per-cell colour
//   and bold. Feed it bytes with FeedAnsi(); for an in-place "screen" update
//   (e.g. the timing-tells progress block) use SetScreenAnsi(), which homes and
//   clears first so the same region is overwritten rather than scrolled.
//
//******************************************************************************

#ifndef INC_CVTERMPANE_H
#define INC_CVTERMPANE_H

#include <vector>
#include <deque>

struct VTerm;          // opaque (libvterm)
struct VTermScreen;

class CVTermPane : public CWnd {
 public:
  CVTermPane();
  virtual ~CVTermPane();

  BOOL Create(CWnd *parent, UINT id, const CRect &rc);
  void SetColors(COLORREF bg, COLORREF fg);

  void FeedAnsi(const CString &text);        // append bytes (scrolls)
  void SetScreenAnsi(const CString &text);   // clear+home, then write (in place)
  void ResetTerminal();                       // wipe screen + scrollback

  // libvterm scrollback callbacks (public so the C thunks can reach them).
  int OnPushline(int cols, const void *cells);
  int OnPopline(int cols, void *cells);
  void OnDamage();

 protected:
  afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
  afx_msg void OnPaint();
  afx_msg BOOL OnEraseBkgnd(CDC *pDC);
  afx_msg void OnSize(UINT nType, int cx, int cy);
  afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
  afx_msg void OnVScroll(UINT nSBCode, UINT nPos, CScrollBar *pScrollBar);
  afx_msg void OnDestroy();
  DECLARE_MESSAGE_MAP()

 private:
  struct SbCell { unsigned int ch; COLORREF fg; COLORREF bg; bool bold; bool reverse; };
  typedef std::vector<SbCell> SbLine;

  void EnsureFonts();
  void RebuildVTerm();           // (re)create the VTerm sized to the client
  void WriteScreenText();        // home+clear, then (re)write _screen_text in place
  void ApplyDefaultColors();
  int  VisibleRows() const;
  int  TotalHistory() const { return (int)_scrollback.size(); }
  int  MaxScroll() const;
  void UpdateScrollBar();
  void ResolveCell(const void *vcell, SbCell *out) const;

  VTerm       *_vt;
  VTermScreen *_screen;
  CFont   _font, _font_bold;
  int     _char_w, _line_h;
  int     _rows, _cols;
  COLORREF _bg, _fg;
  std::deque<SbLine> _scrollback;   // lines scrolled off the top
  size_t  _max_sb;
  int     _scroll_off;              // lines scrolled up from the bottom (0 = live)
  bool    _stick;
  // "Screen mode": SetScreenAnsi() pins a fixed block that must survive vterm
  // reflows (resize/rebuild). We remember the text and re-apply it ourselves so a
  // layout pass can never blank the pane (the owner dedups updates and won't re-pin).
  bool    _is_screen_mode;
  CString _screen_text;
};

#endif  // INC_CVTERMPANE_H
