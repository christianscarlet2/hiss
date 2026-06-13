//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: "Two successive clicks" auto-action.
//
//   Clicks the centre of the first rectangle, then (after a delay in ms) the
//   centre of the second rectangle of the tablemap region "two_successive_clicks"
//   -- but ONLY while the OCR of the region "two_successive_clicks_label" equals
//   the user-configured text "two_successive_clicks_text". Edge-triggered: it
//   fires once each time the label starts matching, and re-arms when it stops.
//
//   The match-text and delay are edited on the main-window toolbar and persisted
//   per-tablemap in the shared postgres DB (settings table).
//
//******************************************************************************

#ifndef INC_CTWOSUCCESSIVECLICKS_H
#define INC_CTWOSUCCESSIVECLICKS_H

#include "..\Shared\CCritSec\CCritSec.h"

class CTwoSuccessiveClicks {
 public:
  CTwoSuccessiveClicks();
  ~CTwoSuccessiveClicks();
 public:
  // UI thread: set (and optionally persist to the current tablemap) the config.
  // Two independent match texts, each with its own enable flag; the clicks fire
  // when EITHER enabled (and non-empty) text equals the label OCR.
  void SetConfig(CString text1, bool enable1, CString text2, bool enable2,
                 int delay_ms, bool persist_to_tablemap);
  // UI thread: (re)load the values saved for the currently-loaded tablemap.
  void LoadForCurrentTablemap();
  CString Text1();
  CString Text2();
  bool    Enable1();
  bool    Enable2();
  int     DelayMs();
 public:
  // Autoplayer thread, once per cadence. Fires only when decision_is_raise (the
  // .ohf wants to raise) AND the label matches an enabled box. Returns true if it
  // performed the clicks.
  bool HandleCycle(bool decision_is_raise);
 private:
  CString TablemapField();
 private:
  CCritSec m_critsec;
  CString  _text1, _text2;
  bool     _enable1, _enable2;
  int      _delay_ms;
  bool     _was_matching;
};

extern CTwoSuccessiveClicks *p_two_successive_clicks;

#endif // INC_CTWOSUCCESSIVECLICKS_H
