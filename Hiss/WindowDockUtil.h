//******************************************************************************
//
// WindowDockUtil.h -- small shared helpers for the Hiss tool windows:
//   * an "Always on Top" item on the window's system menu (right-click the title
//     bar / Alt+Space), with a checkmark and HWND_TOPMOST toggling.
//   * edge-snapping during a move (WM_MOVING): the window detaches freely but
//     "sticks" to its owner's left/right side or a screen edge when dragged near,
//     so it can be re-attached to the side.
//
//******************************************************************************
#ifndef INC_WINDOWDOCKUTIL_H
#define INC_WINDOWDOCKUTIL_H

#include <afxwin.h>

// Custom system-command id (must be < 0xF000 and a multiple of 16).
#define SC_HISS_ALWAYS_ON_TOP 0x9010

// Append "Always on Top" to a window's system menu (idempotent-ish: call once in OnCreate).
static inline void Hiss_AppendAlwaysOnTopMenu(CWnd *w) {
  if (w == NULL || !::IsWindow(w->GetSafeHwnd())) return;
  CMenu *sys = w->GetSystemMenu(FALSE);
  if (sys == NULL) return;
  sys->AppendMenu(MF_SEPARATOR);
  sys->AppendMenu(MF_STRING, SC_HISS_ALWAYS_ON_TOP, _T("Always on Top"));
}

// Handle a WM_SYSCOMMAND; returns true if it was our Always-on-Top toggle.
static inline bool Hiss_HandleAlwaysOnTopSysCommand(CWnd *w, UINT nID, bool *flag) {
  if ((nID & 0xFFF0) != SC_HISS_ALWAYS_ON_TOP) return false;
  *flag = !*flag;
  w->SetWindowPos(*flag ? &CWnd::wndTopMost : &CWnd::wndNoTopMost,
                  0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  CMenu *sys = w->GetSystemMenu(FALSE);
  if (sys != NULL) {
    sys->CheckMenuItem(SC_HISS_ALWAYS_ON_TOP,
                       MF_BYCOMMAND | (*flag ? MF_CHECKED : MF_UNCHECKED));
  }
  return true;
}

// Snap the proposed move-rect to the owner's outer left/right side and to screen
// work-area edges (within SNAP px), keeping the window size fixed.
// Returns: 0 = not docked to owner, 1 = docked to owner's LEFT, 2 = docked to RIGHT.
static inline int Hiss_SnapMovingRect(LPRECT r, CWnd *owner, int gap) {
  const int SNAP = 18;
  int wdt = r->right - r->left;
  int hgt = r->bottom - r->top;
  int docked = 0;
  if (owner != NULL && ::IsWindow(owner->GetSafeHwnd())) {
    CRect o; owner->GetWindowRect(&o);
    if (abs((int)(r->right - (o.left - gap))) <= SNAP) {        // stick to owner's left
      r->right = o.left - gap; r->left = r->right - wdt; docked = 1;
    } else if (abs((int)(r->left - (o.right + gap))) <= SNAP) { // stick to owner's right
      r->left = o.right + gap; r->right = r->left + wdt; docked = 2;
    }
    if (docked != 0 && abs((int)(r->top - o.top)) <= SNAP * 3) { // align tops when docked
      r->top = o.top; r->bottom = r->top + hgt;
    }
  }
  RECT wa; SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
  if (abs((int)(r->left - wa.left)) <= SNAP)     { r->left = wa.left;   r->right = r->left + wdt; }
  if (abs((int)(r->right - wa.right)) <= SNAP)   { r->right = wa.right; r->left = r->right - wdt; }
  if (abs((int)(r->top - wa.top)) <= SNAP)       { r->top = wa.top;     r->bottom = r->top + hgt; }
  if (abs((int)(r->bottom - wa.bottom)) <= SNAP) { r->bottom = wa.bottom; r->top = r->bottom - hgt; }
  return docked;
}

#endif  // INC_WINDOWDOCKUTIL_H
