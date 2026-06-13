//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Human-readable, variable-filled decision explanations.
//
//   Lets an .ohf file annotate a decision branch so that WHEN that branch is
//   reached, a templated explanation (with live values substituted in) is printed
//   to the Decisions pane of the OpenHoldem Terminal window.
//
//   Usage in the .ohf:
//     1. Define the explanation TEMPLATE as a string-literal function, naming it
//        f$expl_<tag>. Use {symbol} placeholders for live context:
//
//          ##f$expl_fold_tank##
//          "Fold: one pair vs a {lastraiseractiontime}s tank by a {f$Opp_VPIP}%
//           VPIP raiser; pot {PotSize}bb, call {AmountToCall}bb -> likely beat."
//
//     2. Trigger it by adding the matching explain_<tag> symbol to the TAIL of the
//        branch's WHEN condition (AND short-circuits, so it fires only when the
//        branch is actually selected and returns true so it is transparent):
//
//          WHEN f$HaveOnePair AND f$TimingSaysStrong AND explain_fold_tank Fold FORCE
//
//   The placeholders may reference any symbol or f$function; each is evaluated
//   live and substituted. Emission is suppressed during parsing and de-duplicated
//   so an unchanged explanation is not repeated every heartbeat.
//
//******************************************************************************

#ifndef INC_CSYMBOLENGINEEXPLAIN_H
#define INC_CSYMBOLENGINEEXPLAIN_H

#include "CVirtualSymbolEngine.h"

class CSymbolEngineExplain : public CVirtualSymbolEngine {
 public:
  CSymbolEngineExplain();
  ~CSymbolEngineExplain();
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
  CString RenderTemplate(const CString tag);
  CString Interpolate(const CString tmpl);
  void    Emit(const CString tag);
 private:
  CString _last_tag;   // de-dupe: emit a given tag at most once per street/turn
};

#endif  // INC_CSYMBOLENGINEEXPLAIN_H
