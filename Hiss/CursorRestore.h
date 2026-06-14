//******************************************************************************
//
// CursorRestore.h -- save the mouse cursor position on construction and restore
// it on destruction. Used to wrap a whole autoplayer/manual click SEQUENCE so the
// cursor returns to where the user left it -- only ONCE, at the end of the
// sequence (after the two-successive-clicks, the numpad, nOkay and nConfirm), not
// between the individual clicks.
//
//******************************************************************************
#ifndef INC_CURSORRESTORE_H
#define INC_CURSORRESTORE_H

#include <windows.h>

struct CCursorRestorer {
  POINT pt;
  bool  ok;
  CCursorRestorer()  { ok = (GetCursorPos(&pt) != FALSE); }
  ~CCursorRestorer() { if (ok) SetCursorPos(pt.x, pt.y); }
};

#endif  // INC_CURSORRESTORE_H
