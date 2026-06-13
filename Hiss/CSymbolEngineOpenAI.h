//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: OpenPPL symbols that expose the OpenAI advisor / steering state.
//
//   openai_paused        1 while the operator typed /stop (else 0). Gate your
//                        top-level f$ functions on it to hold the autoplayer.
//   openai_steer_anchor  the anchor id the operator steered to via /goto or
//                        /strategy (0 = none). Dispatch on it (see 16_steering.ohf).
//   openai_action        the action code OpenAI last recommended, or the
//                        kOpenAiNoOpinion sentinel (so a disabled advisor never
//                        changes play). RETURN it only when > -1000.
//   openai_autohijack    1 if auto-hijack on no-fit spots is enabled.
//   openai_consult       TRIGGER (returns true): at a no-good-fit branch, asks
//                        the advisor to consult OpenAI (only acts if enabled).
//
//******************************************************************************

#ifndef INC_CSYMBOLENGINEOPENAI_H
#define INC_CSYMBOLENGINEOPENAI_H

#include "CVirtualSymbolEngine.h"

class CSymbolEngineOpenAI : public CVirtualSymbolEngine {
 public:
  CSymbolEngineOpenAI();
  ~CSymbolEngineOpenAI();
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
};

#endif  // INC_CSYMBOLENGINEOPENAI_H
