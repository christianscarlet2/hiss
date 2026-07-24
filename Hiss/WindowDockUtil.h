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

// ---- persistence (per-window, in the app's registry profile) -----------------
static inline void Hiss_ApplyTopMost(CWnd *w, bool on) {
  if (w != NULL && ::IsWindow(w->GetSafeHwnd())) {
    w->SetWindowPos(on ? &CWnd::wndTopMost : &CWnd::wndNoTopMost,
                    0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
}
static inline void Hiss_SaveAlwaysOnTop(LPCTSTR key, bool on) {
  if (AfxGetApp() != NULL) AfxGetApp()->WriteProfileInt(_T("AlwaysOnTop"), key, on ? 1 : 0);
}
static inline bool Hiss_LoadAlwaysOnTop(LPCTSTR key) {
  return (AfxGetApp() != NULL) && (AfxGetApp()->GetProfileInt(_T("AlwaysOnTop"), key, 0) != 0);
}

// ---- window placement (position + size), same per-window profile storage --------------------
//
// Stored as four ints under "WindowPlacement\<key>". Saved on destroy, restored on create, so the
// React table view reopens exactly where it was left instead of at CW_USEDEFAULT. [Emrald]
static inline void Hiss_SaveWindowPlacement(CWnd *w, LPCTSTR key) {
  if (w == NULL || !::IsWindow(w->GetSafeHwnd()) || AfxGetApp() == NULL) return;
  WINDOWPLACEMENT wp; wp.length = sizeof(wp);
  if (!w->GetWindowPlacement(&wp)) return;
  // Use the RESTORED rect, never the current one: saving while maximized or minimized would
  // persist a full-screen or off-screen box and the window would reopen wrong.
  const RECT &r = wp.rcNormalPosition;
  int cx = r.right - r.left, cy = r.bottom - r.top;
  if (cx <= 0 || cy <= 0) return;                       // never persist a degenerate size
  CWinApp *app = AfxGetApp();
  CString section; section.Format(_T("WindowPlacement\\%s"), key);
  app->WriteProfileInt(section, _T("x"),  r.left);
  app->WriteProfileInt(section, _T("y"),  r.top);
  app->WriteProfileInt(section, _T("cx"), cx);
  app->WriteProfileInt(section, _T("cy"), cy);
  app->WriteProfileInt(section, _T("max"), (wp.showCmd == SW_SHOWMAXIMIZED) ? 1 : 0);
}

// Restore a saved placement. Returns false when nothing was saved (caller keeps its defaults).
static inline bool Hiss_RestoreWindowPlacement(CWnd *w, LPCTSTR key) {
  if (w == NULL || !::IsWindow(w->GetSafeHwnd()) || AfxGetApp() == NULL) return false;
  CWinApp *app = AfxGetApp();
  CString section; section.Format(_T("WindowPlacement\\%s"), key);
  int cx = app->GetProfileInt(section, _T("cx"), 0);
  int cy = app->GetProfileInt(section, _T("cy"), 0);
  if (cx <= 0 || cy <= 0) return false;                 // nothing saved yet
  int x = app->GetProfileInt(section, _T("x"), 0);
  int y = app->GetProfileInt(section, _T("y"), 0);
  // A monitor may have been unplugged or the layout changed since we saved. If the saved box no
  // longer intersects any visible monitor the window would open somewhere unreachable, so fall
  // back to the nearest monitor's work area rather than restoring a box the user cannot grab.
  CRect want(x, y, x + cx, y + cy);
  HMONITOR mon = ::MonitorFromRect(&want, MONITOR_DEFAULTTONULL);
  if (mon == NULL) {
    mon = ::MonitorFromRect(&want, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    if (mon != NULL && ::GetMonitorInfo(mon, &mi)) {
      x = mi.rcWork.left + 40;
      y = mi.rcWork.top + 40;
      if (cx > mi.rcWork.right - mi.rcWork.left) cx = mi.rcWork.right - mi.rcWork.left;
      if (cy > mi.rcWork.bottom - mi.rcWork.top) cy = mi.rcWork.bottom - mi.rcWork.top;
    }
  }
  w->SetWindowPos(NULL, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
  if (app->GetProfileInt(section, _T("max"), 0) != 0) w->ShowWindow(SW_MAXIMIZE);
  return true;
}

// Append "Always on Top" to a window's system menu (idempotent-ish: call once in OnCreate).
static inline void Hiss_AppendAlwaysOnTopMenu(CWnd *w) {
  if (w == NULL || !::IsWindow(w->GetSafeHwnd())) return;
  CMenu *sys = w->GetSystemMenu(FALSE);
  if (sys == NULL) return;
  sys->AppendMenu(MF_SEPARATOR);
  sys->AppendMenu(MF_STRING, SC_HISS_ALWAYS_ON_TOP, _T("Always on Top"));
}

// Restore the persisted Always-on-Top state on window create: loads the saved flag,
// applies topmost, and checks the system-menu item. Call in OnCreate AFTER the menus exist.
static inline void Hiss_RestoreAlwaysOnTop(CWnd *w, bool *flag, LPCTSTR key) {
  if (w == NULL || flag == NULL) return;
  *flag = Hiss_LoadAlwaysOnTop(key);
  if (*flag) Hiss_ApplyTopMost(w, true);
  CMenu *sys = w->GetSystemMenu(FALSE);
  if (sys != NULL) {
    sys->CheckMenuItem(SC_HISS_ALWAYS_ON_TOP, MF_BYCOMMAND | (*flag ? MF_CHECKED : MF_UNCHECKED));
  }
}

// Handle a WM_SYSCOMMAND; returns true if it was our Always-on-Top toggle. Persists.
static inline bool Hiss_HandleAlwaysOnTopSysCommand(CWnd *w, UINT nID, bool *flag, LPCTSTR key) {
  if ((nID & 0xFFF0) != SC_HISS_ALWAYS_ON_TOP) return false;
  *flag = !*flag;
  Hiss_ApplyTopMost(w, *flag);
  Hiss_SaveAlwaysOnTop(key, *flag);
  CMenu *sys = w->GetSystemMenu(FALSE);
  if (sys != NULL) {
    sys->CheckMenuItem(SC_HISS_ALWAYS_ON_TOP,
                       MF_BYCOMMAND | (*flag ? MF_CHECKED : MF_UNCHECKED));
  }
  return true;
}

// Toggle from a VISIBLE menu-bar item: flips, applies, persists, and re-checks both the
// given menu item (menu_bar may be NULL for custom-drawn menus) and the system-menu item.
static inline void Hiss_ToggleAlwaysOnTopFromMenu(CWnd *w, bool *flag, LPCTSTR key,
                                                  CMenu *menu_bar, UINT cmdid) {
  if (w == NULL || flag == NULL) return;
  *flag = !*flag;
  Hiss_ApplyTopMost(w, *flag);
  Hiss_SaveAlwaysOnTop(key, *flag);
  if (menu_bar != NULL) {
    menu_bar->CheckMenuItem(cmdid, MF_BYCOMMAND | (*flag ? MF_CHECKED : MF_UNCHECKED));
  }
  CMenu *sys = w->GetSystemMenu(FALSE);
  if (sys != NULL) {
    sys->CheckMenuItem(SC_HISS_ALWAYS_ON_TOP, MF_BYCOMMAND | (*flag ? MF_CHECKED : MF_UNCHECKED));
  }
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
