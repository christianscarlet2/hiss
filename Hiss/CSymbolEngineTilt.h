//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Real-time tilt signals for the OHF strategy.
//
//   Tracks each chair's stack across recent hands (sampled at hand-reset) and
//   exposes drawdown-based tilt symbols the .ohf can react to in-hand:
//
//     hero_drawdown          fraction of the hero's recent peak stack lost (0..1)
//     hero_tilting           hero_drawdown >= the tilt threshold (bool)
//     raiser_drawdown        the current raiser's recent stack drawdown (0..1)
//     raiser_recent_bigloss  raiser_drawdown >= threshold (bool)
//     raiser_maybe_tilting   raiser took a big recent loss and is firing now (bool)
//
//   This is the FAST, in-hand layer. The deeper behavioural/bad-beat analysis
//   (VPIP spikes, disliked-decision clusters) lives in the tilt-detector skill,
//   which reads game_state_log / learner_decisions over the analysis-loop cadence.
//
//******************************************************************************

#ifndef INC_CSYMBOLENGINETILT_H
#define INC_CSYMBOLENGINETILT_H

#include "CVirtualSymbolEngine.h"
#include "..\Shared\MagicNumbers\MagicNumbers.h"

class CSymbolEngineTilt : public CVirtualSymbolEngine {
 public:
  CSymbolEngineTilt();
  ~CSymbolEngineTilt();
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
  double Drawdown(int chair);   // (recent peak - current) / recent peak, 0 if n/a
 private:
  static const int kRing = 15;  // hands of stack history kept per chair
  double _stack_hist[kMaxNumberOfPlayers][kRing];
  int    _count[kMaxNumberOfPlayers];   // how many samples recorded
  int    _idx[kMaxNumberOfPlayers];     // next write slot (ring)
  double _tilt_threshold;               // drawdown that counts as tilt (e.g. 0.33)
};

extern CSymbolEngineTilt *p_symbol_engine_tilt;

#endif  // INC_CSYMBOLENGINETILT_H
