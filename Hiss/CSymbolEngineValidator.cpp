//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************

#include "stdafx.h"
#include "CSymbolEngineValidator.h"

#include <math.h>

#include "CBetroundCalculator.h"
#include "CEngineContainer.h"
#include "CSymbolEngineActiveDealtPlaying.h"
#include "CSymbolEngineChipAmounts.h"
#include "CSymbolEngineTableLimits.h"
#include "CTableState.h"
#include "Card.h"
#include "CPlayer.h"
#include "ChatTerminalWindow.h"   // ChatTerminalAppend (thread-safe, PostMessage)
#include "..\CTablemap\CTablemap.h"

CSymbolEngineValidator *p_symbol_engine_validator = NULL;

// Treat very large multiples of the big blind as physically implausible scrapes.
static const double kMaxPlausibleBBsInStack   = 200000.0;  // 200k bb stack -> suspect
static const double kMaxPlausibleBBsInPot     = 500000.0;
static const double kChipEpsilon              = 0.01;

CSymbolEngineValidator::CSymbolEngineValidator() {
  UpdateOnConnection();
}

CSymbolEngineValidator::~CSymbolEngineValidator() {}

void CSymbolEngineValidator::InitOnStartup()    { UpdateOnConnection(); }
void CSymbolEngineValidator::UpdateOnConnection() { Reset(); }
void CSymbolEngineValidator::UpdateOnHandreset() {}
void CSymbolEngineValidator::UpdateOnNewRound()  { Validate(); }
void CSymbolEngineValidator::UpdateOnMyTurn()    { Validate(); }
void CSymbolEngineValidator::UpdateOnHeartbeat() { Validate(); }

void CSymbolEngineValidator::Reset() {
  _ok = true;
  _cards_ok = _pot_ok = _stacks_ok = _bets_ok = true;
  _nerrors = 0;
  _nwarnings = 0;
  _confidence = 1.0;
  _report = "";
}

void CSymbolEngineValidator::AddError(const CString msg) {
  ++_nerrors;
  _ok = false;
  _report += "ERROR: " + msg + "\n";
}

void CSymbolEngineValidator::AddWarn(const CString msg) {
  ++_nwarnings;
  _report += "WARN: " + msg + "\n";
}

int CSymbolEngineValidator::ExpectedCommunityCards(int betround) {
  switch (betround) {
    case kBetroundPreflop: return 0;
    case kBetroundFlop:    return 3;
    case kBetroundTurn:    return 4;
    case kBetroundRiver:   return 5;
    default:               return -1;  // unknown -> don't enforce
  }
}

void CSymbolEngineValidator::CheckCards() {
  if (p_table_state == NULL || p_tablemap == NULL) return;
  int nchairs = p_tablemap->nchairs();
  if (nchairs <= 0 || nchairs > kMaxNumberOfPlayers) nchairs = kMaxNumberOfPlayers;

  // Collect every KNOWN card on the table (hole cards of any player whose cards
  // are visible + the community cards) and look for duplicates / impossible values.
  int seen[52];
  for (int i = 0; i < 52; ++i) seen[i] = 0;

  // Community cards
  int n_community_known = 0;
  for (int i = 0; i < kNumberOfCommunityCards; ++i) {
    Card *c = p_table_state->CommonCards(i);
    if (c == NULL || !c->IsKnownCard()) continue;
    ++n_community_known;
    int v = c->GetValue();
    if (v < 0 || v > 51) { AddError("community card has an impossible value"); _cards_ok = false; continue; }
    if (++seen[v] > 1) { AddError("duplicate card on the board/among hands"); _cards_ok = false; }
  }

  // Players' hole cards
  for (int chair = 0; chair < nchairs; ++chair) {
    CPlayer *p = p_table_state->Player(chair);
    if (p == NULL) continue;
    int known_here = 0;
    for (int j = 0; j < kMaxNumberOfCardsPerPlayer; ++j) {
      Card *c = p->hole_cards(j);
      if (c == NULL || !c->IsKnownCard()) continue;
      ++known_here;
      int v = c->GetValue();
      if (v < 0 || v > 51) { AddError("a hole card has an impossible value"); _cards_ok = false; continue; }
      if (++seen[v] > 1) { AddError("duplicate card among hands/board"); _cards_ok = false; }
    }
    // A player should show 0 or a full holding, never a lone single hole card.
    if (known_here == 1) {
      AddWarn("a seat shows exactly one known hole card (partial scrape)");
      _cards_ok = false;
    }
  }

  // Community-card count must match the betting round.
  int br = (p_betround_calculator != NULL) ? p_betround_calculator->betround() : kUndefined;
  int expected = ExpectedCommunityCards(br);
  if (expected >= 0 && n_community_known != expected) {
    CString m;
    m.Format("board shows %d cards but betround %d expects %d (scrape lag or mis-scrape)",
             n_community_known, br, expected);
    AddWarn(m);
    _cards_ok = false;
  }
}

void CSymbolEngineValidator::CheckPotsAndConservation() {
  if (p_engine_container == NULL || p_table_state == NULL || p_tablemap == NULL) return;
  CSymbolEngineChipAmounts *chips = p_engine_container->symbol_engine_chip_amounts();
  CSymbolEngineTableLimits *lim = p_engine_container->symbol_engine_tablelimits();
  if (chips == NULL) return;

  double pot       = chips->pot();
  double potcommon = chips->potcommon();
  double potplayer = chips->potplayer();
  double bb = (lim != NULL) ? lim->bblind() : 0.0;
  double sb = (lim != NULL) ? lim->sblind() : 0.0;

  if (pot < -kChipEpsilon)       { AddError("pot is negative"); _pot_ok = false; }
  if (potcommon < -kChipEpsilon) { AddError("potcommon is negative (mis-scraped pot or wrong potmethod)"); _pot_ok = false; }
  if (potplayer < -kChipEpsilon) { AddError("potplayer (round bets) is negative"); _pot_ok = false; }

  // Independently recompute the round bets from each player's scraped bet and
  // cross-check against the engine's potplayer (catches a single bad bet OCR).
  int nchairs = p_tablemap->nchairs();
  if (nchairs <= 0 || nchairs > kMaxNumberOfPlayers) nchairs = kMaxNumberOfPlayers;
  double sum_bets = 0.0;
  for (int i = 0; i < nchairs; ++i) {
    CPlayer *p = p_table_state->Player(i);
    if (p == NULL) continue;
    double b = p->_bet.GetValue();
    if (b < -kChipEpsilon) { AddError("a seat has a negative current bet"); _pot_ok = false; }
    sum_bets += (b > 0 ? b : 0);
  }
  if (potplayer > kChipEpsilon && fabs(sum_bets - potplayer) > max(kChipEpsilon, 0.02 * potplayer)) {
    CString m;
    m.Format("round-bets mismatch: sum of seats = %.2f but potplayer = %.2f", sum_bets, potplayer);
    AddWarn(m);
    _pot_ok = false;
  }

  // With this table the scraped pot is the running total and should include the
  // current bets, so pot must be >= the round bets. A pot SMALLER than the bets
  // means a mis-scraped pot (the classic negative-potcommon case).
  if (pot > kChipEpsilon && potplayer > pot + max(kChipEpsilon, 0.05 * pot)) {
    CString m;
    m.Format("round bets (%.2f) exceed the total pot (%.2f) -> pot mis-scrape", potplayer, pot);
    AddError(m);
    _pot_ok = false;
  }

  // Absurd magnitudes relative to the blind.
  if (bb > 0 && pot > 0 && (pot / bb) > kMaxPlausibleBBsInPot) {
    AddError("pot is implausibly large vs the big blind"); _pot_ok = false;
  }

  // Preflop the round bets should at least cover the posted blinds.
  int br = (p_betround_calculator != NULL) ? p_betround_calculator->betround() : kUndefined;
  if (br == kBetroundPreflop && bb > 0 && (sb + bb) > 0
      && potplayer > kChipEpsilon && potplayer + kChipEpsilon < (sb + bb) * 0.5) {
    AddWarn("preflop round bets are below the posted blinds (a blind may be un-scraped)");
    _pot_ok = false;
  }
}

void CSymbolEngineValidator::CheckStacks() {
  if (p_engine_container == NULL || p_table_state == NULL || p_tablemap == NULL) return;
  CSymbolEngineTableLimits *lim = p_engine_container->symbol_engine_tablelimits();
  CSymbolEngineChipAmounts *chips = p_engine_container->symbol_engine_chip_amounts();
  double bb = (lim != NULL) ? lim->bblind() : 0.0;

  int nchairs = p_tablemap->nchairs();
  if (nchairs <= 0 || nchairs > kMaxNumberOfPlayers) nchairs = kMaxNumberOfPlayers;
  for (int i = 0; i < nchairs; ++i) {
    CPlayer *p = p_table_state->Player(i);
    if (p == NULL || !p->seated()) continue;
    double behind = p->_bet.GetValue() >= 0 ? (p->stack() - p->_bet.GetValue()) : p->stack();
    if (behind < -kChipEpsilon) { AddError("a seat has a negative stack-behind"); _stacks_ok = false; }
    double total = p->stack();
    if (bb > 0 && total > 0 && (total / bb) > kMaxPlausibleBBsInStack) {
      AddWarn("a seat's stack is implausibly large vs the big blind"); _stacks_ok = false;
    }
    // A stack should not GROW mid-hand (chips only leave your stack until you win
    // at showdown). Compare against the start-of-hand snapshot the engine keeps.
    if (chips != NULL) {
      double start = chips->stacks_at_hand_start(i);
      if (start > 0 && total > start + max(kChipEpsilon, 0.05 * start)) {
        // Allowed at hand end (won the pot); flag only mid-hand as a likely mis-scrape.
        int br = (p_betround_calculator != NULL) ? p_betround_calculator->betround() : kUndefined;
        if (br == kBetroundPreflop || br == kBetroundFlop || br == kBetroundTurn) {
          AddWarn("a seat's stack grew mid-hand (likely a stack/bet mis-scrape)");
          _stacks_ok = false;
        }
      }
    }
  }
}

void CSymbolEngineValidator::CheckBetsAndSituation() {
  if (p_engine_container == NULL) return;
  CSymbolEngineChipAmounts *chips = p_engine_container->symbol_engine_chip_amounts();
  CSymbolEngineActiveDealtPlaying *adp = p_engine_container->symbol_engine_active_dealt_playing();

  int br = (p_betround_calculator != NULL) ? p_betround_calculator->betround() : kUndefined;
  if (br != kUndefined && (br < kBetroundPreflop || br > kBetroundRiver)) {
    CString m; m.Format("betround out of range: %d", br); AddError(m); _bets_ok = false;
  }
  if (chips != NULL && chips->ncurrentbets() < 0) {
    AddError("ncurrentbets is negative"); _bets_ok = false;
  }
  if (adp != NULL) {
    double ndealt   = adp->nplayersdealt();
    double nplaying = adp->nopponentsplaying();
    if (ndealt >= 0 && nplaying > ndealt) {
      AddWarn("more opponents playing than players dealt (seat/scrape inconsistency)");
      _bets_ok = false;
    }
    // Postflop there must be community cards.
    if (p_table_state != NULL && br > kBetroundPreflop) {
      int n_known = 0;
      for (int i = 0; i < kNumberOfCommunityCards; ++i) {
        Card *c = p_table_state->CommonCards(i);
        if (c != NULL && c->IsKnownCard()) ++n_known;
      }
      if (n_known == 0) { AddWarn("postflop but no community cards scraped"); _bets_ok = false; }
    }
  }
}

void CSymbolEngineValidator::Validate() {
  Reset();
  // Guard: nothing to validate until we have a table state.
  if (p_table_state == NULL || p_engine_container == NULL) {
    _confidence = 0.0;
    return;
  }
  CheckCards();
  CheckPotsAndConservation();
  CheckStacks();
  CheckBetsAndSituation();

  // Confidence: start at 1, each error costs a lot, each warning a little.
  double conf = 1.0 - (0.34 * _nerrors) - (0.08 * _nwarnings);
  if (conf < 0.0) conf = 0.0;
  if (conf > 1.0) conf = 1.0;
  _confidence = conf;

  if (_nerrors > 0) {
    write_log(k_always_log_errors, "[CSymbolEngineValidator] %d error(s), %d warning(s):\n%s",
              _nerrors, _nwarnings, _report.GetString());
  }

  // Tell the user via the Hiss chat-terminal window when POT/STACKS/BETS are invalid
  // (cards are excluded -- those go down the self-heal path). Debounced: only push when
  // the issue first appears or its text changes, not every heartbeat. Reset when it clears.
  bool psb_bad = (!_pot_ok || !_stacks_ok || !_bets_ok);
  if (psb_bad) {
    if (_report != _last_terminal_report) {
      CString msg;
      msg.Format("[VALIDATOR] scrape issue (pot/stacks/bets) - confidence %.2f:\n%s",
                 _confidence, _report.GetString());
      ChatTerminalAppend(kChatTerminalChat, msg);
      _last_terminal_report = _report;
    }
  } else {
    _last_terminal_report = "";   // resolved -> re-notify if it recurs
  }
}

bool CSymbolEngineValidator::EvaluateSymbol(const CString name, double *result, bool log) {
  if (name.Left(10) != "validator_") return false;
  if (name == "validator_ok")          { if (result) *result = _ok ? 1.0 : 0.0; return true; }
  if (name == "validator_nerrors")     { if (result) *result = _nerrors;        return true; }
  if (name == "validator_nwarnings")   { if (result) *result = _nwarnings;      return true; }
  if (name == "validator_confidence")  { if (result) *result = _confidence;     return true; }
  if (name == "validator_cards_ok")    { if (result) *result = _cards_ok ? 1.0 : 0.0;  return true; }
  if (name == "validator_pot_ok")      { if (result) *result = _pot_ok ? 1.0 : 0.0;    return true; }
  if (name == "validator_stacks_ok")   { if (result) *result = _stacks_ok ? 1.0 : 0.0; return true; }
  if (name == "validator_bets_ok")     { if (result) *result = _bets_ok ? 1.0 : 0.0;   return true; }
  return false;
}

CString CSymbolEngineValidator::SymbolsProvided() {
  return "validator_ok validator_nerrors validator_nwarnings validator_confidence "
         "validator_cards_ok validator_pot_ok validator_stacks_ok validator_bets_ok ";
}
