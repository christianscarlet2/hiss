//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Scrape + game-state validation heuristics.
//
//   Runs sanity heuristics over the scraped table state every heartbeat and
//   exposes the verdict as symbols the .ohf (or any consumer) can gate on, and
//   as a JSON report served at /api/validate. The goal is to catch mis-scrapes
//   (a bad pot/stack/bet OCR, duplicate or impossible cards, chip totals that
//   don't conserve) BEFORE a strategy or a manual bet acts on a garbage value.
//
//   Symbols provided (all 0/1 unless noted):
//     validator_ok            no hard errors in the current state
//     validator_nerrors       count of hard errors (numeric)
//     validator_nwarnings     count of soft warnings (numeric)
//     validator_confidence    0..1 heuristic confidence in the scrape
//     validator_cards_ok      no duplicate / impossible / count-mismatched cards
//     validator_pot_ok        pot/potcommon/potplayer + chip conservation sane
//     validator_stacks_ok     stacks non-negative, not absurd, no impossible jumps
//     validator_bets_ok       bets/round/seat counts internally consistent
//
//   This is a READ-ONLY layer: it never changes the table state or a decision.
//   It only reports. The .ohf may choose to fold/skip when validator_ok = 0.
//
//******************************************************************************

#ifndef INC_CSYMBOLENGINEVALIDATOR_H
#define INC_CSYMBOLENGINEVALIDATOR_H

#include "CVirtualSymbolEngine.h"
#include "..\Shared\MagicNumbers\MagicNumbers.h"

class CSymbolEngineValidator : public CVirtualSymbolEngine {
 public:
  CSymbolEngineValidator();
  ~CSymbolEngineValidator();
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
 public:
  // Re-run all heuristics now and refresh the cached verdict.
  void Validate();
  // Accessors for the cached verdict (used by EvaluateSymbol + /api/validate).
  bool    Ok()          { return _ok; }
  int     Nerrors()     { return _nerrors; }
  int     Nwarnings()   { return _nwarnings; }
  double  Confidence()  { return _confidence; }
  bool    CardsOk()     { return _cards_ok; }
  bool    PotOk()       { return _pot_ok; }
  bool    StacksOk()    { return _stacks_ok; }
  bool    BetsOk()      { return _bets_ok; }
  CString Report()      { return _report; }   // newline-separated "ERROR:"/"WARN:" lines
 private:
  void Reset();
  void AddError(const CString msg);
  void AddWarn(const CString msg);
  void CheckCards();
  void CheckPotsAndConservation();
  void CheckStacks();
  void CheckBetsAndSituation();
  int  ExpectedCommunityCards(int betround);
 private:
  bool    _ok;
  bool    _cards_ok, _pot_ok, _stacks_ok, _bets_ok;
  int     _nerrors, _nwarnings;
  double  _confidence;
  CString _report;
  CString _last_terminal_report;   // dedupe: last pot/stacks/bets alert pushed to the terminal
};

extern CSymbolEngineValidator *p_symbol_engine_validator;

#endif  // INC_CSYMBOLENGINEVALIDATOR_H
