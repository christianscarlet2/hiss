//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Timing tells from the pNactive regions.
//
//   Measures how long each chair stays "active" (the to-act highlight on its
//   pNactive region, RECT 1 only -- rect2 is ignored) before the NEXT chair
//   becomes active. That dwell time is that player's action time = a timing tell.
//
//   Exposes OpenPPL symbols  p0timing .. p9timing  (the last recorded action
//   time of that chair, in SECONDS, as a double) for use in IF conditions, e.g.
//       WHEN p3timing > 3 RaiseBy 3 FORCE
//   and pins a live summary into the State box of the OpenHoldem Terminal window.
//
//******************************************************************************

#ifndef INC_CSYMBOLENGINETIMINGTELLS_H
#define INC_CSYMBOLENGINETIMINGTELLS_H

#include "CVirtualSymbolEngine.h"
#include "..\Shared\MagicNumbers\MagicNumbers.h"

class CSymbolEngineTimingTells : public CVirtualSymbolEngine {
 public:
  CSymbolEngineTimingTells();
  ~CSymbolEngineTimingTells();
 public:
  void InitOnStartup();
  void UpdateOnConnection();
  void UpdateOnHandreset();
  void UpdateOnNewRound();
  void UpdateOnMyTurn();
  void UpdateOnHeartbeat();
 public:
  bool EvaluateSymbol(const CString name, double *result, bool log = false);
  CString SymbolsProvided();
 private:
  // Evaluate the pNactive region using ONLY rectangle 1 (temporarily disabling
  // its optional rect2 OR-match), returning the "active" highlight state.
  bool Rect1Active(int chair);
  void PublishStateBox();
 private:
  double        _timing_seconds[kMaxNumberOfPlayers];  // last recorded dwell per chair
  bool          _was_active[kMaxNumberOfPlayers];       // rect1-active state last frame
  int           _last_chair;        // chair that most recently became active (-1 = none)
  unsigned long _last_tick;         // GetTickCount() when _last_chair became active
  CString       _last_published;    // last text pinned to the State box (avoid redraw spam)
};

#endif  // INC_CSYMBOLENGINETIMINGTELLS_H
