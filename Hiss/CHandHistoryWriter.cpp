//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Central, self-contained hand-history generator.
//
//   This is the single source of truth for hand histories. The sibling
//   CHandHistory* engines (DealPhase / Action / Uncontested / Showdown) are now
//   neutered no-ops; everything is observed and written here.
//
//   Every heartbeat we look at the scraped table-state and incrementally
//   reconstruct the hand: blinds, per-street betting actions (from bet/stack
//   changes), the board as it appears, and the result. At the start of the next
//   hand (handreset) the finished hand is appended to
//     <OpenHoldemDirectory>\handhistory\hh_session_<id>.txt
//
//   The tablemap is frequently incomplete, so for any field TYPE that is not in
//   the tablemap we substitute a placeholder rather than guess:
//     "Seat N" = player name unknown      ?  = card unknown
//     ??       = amount / board unknown
//   Action reconstruction from screen-scraping is inherently approximate; the
//   file header says so.
//
//******************************************************************************

#include "stdafx.h"
#include "CHandHistoryWriter.h"

#include "CBetroundCalculator.h"
#include "CEngineContainer.h"
#include "CHandresetDetector.h"
#include "CScraper.h"
#include "CSessionCounter.h"
#include "CSymbolEngineActiveDealtPlaying.h"
#include "CSymbolEngineChipAmounts.h"
#include "CSymbolEngineDealerchair.h"
#include "CSymbolEngineTableLimits.h"
#include "CSymbolEngineUserchair.h"
#include "CTableState.h"
#include "..\CTablemap\CTablemap.h"
#include "..\DLLs\Files_DLL\Files.h"

const double kEpsilon = 0.0001;

CHandHistoryWriter *p_handhistory_writer = NULL;

CHandHistoryWriter::CHandHistoryWriter() {
  // This engine collects data from the table-state and the other engines
  // and therefore must be executed after all the rest (it is registered last).
  _output_file = "";
  ResetHand();
}

CHandHistoryWriter::~CHandHistoryWriter() {
  // Best-effort: flush a hand that was still in progress at shutdown.
  Flush();
}

void CHandHistoryWriter::InitOnStartup() {
}

void CHandHistoryWriter::UpdateOnConnection() {
}

void CHandHistoryWriter::UpdateOnHandreset() {
  // A new hand has started: write out the one that just finished, then reset.
  Flush();
  ResetHand();
}

void CHandHistoryWriter::UpdateOnNewRound() {
}

void CHandHistoryWriter::UpdateOnMyTurn() {
}

void CHandHistoryWriter::UpdateOnHeartbeat() {
  if (!_meta_captured) {
    // Wait until at least two players are dealt (blinds posted) before we
    // open a hand. If we joined mid-hand we still open it, with placeholders.
    int ndealt = p_engine_container->symbol_engine_active_dealt_playing()->nplayersdealt();
    if ((ndealt >= 2) || (BETROUND > kBetroundPreflop)) {
      CaptureMetadata();
    } else {
      return;
    }
  }
  ObserveStreetTransition();
  ObserveActions();
  ObserveResult();
}

// ---------------------------------------------------------------------------
// recorder lifecycle
// ---------------------------------------------------------------------------

void CHandHistoryWriter::ResetHand() {
  _meta_captured = false;
  _hand_dirty    = false;
  _hand_number   = "";
  _nchairs       = 0;
  _button        = kUndefined;
  _hero          = kUndefined;
  _sb = _bb = _ante = 0.0;
  _have_names = _have_balance = _have_bet = false;
  _have_cards = _have_board = _have_dealer = false;
  _cur_street    = kBetroundPreflop;
  _street_max    = 0.0;
  _blinds_done   = false;
  _body          = "";
  _board_flop = _board_turn = _board_river = "";
  _flop_logged = _turn_logged = _river_logged = false;
  _winner_uncontested = kUndefined;
  _showdown_logged = false;
  _final_pot = 0.0;
  for (int i = 0; i < kMaxNumberOfPlayers; ++i) {
    _seat_name[i]    = "";
    _seat_stack[i]   = 0.0;
    _seat_in_hand[i] = false;
    _hole[i]         = "";
    _street_bet[i]   = 0.0;
    _folded[i]       = false;
  }
}

void CHandHistoryWriter::CaptureMetadata() {
  // Detect which field-types this tablemap actually provides.
  _have_names   = AnySeatRegion("name");
  _have_balance = AnySeatRegion("balance");
  _have_bet     = AnySeatRegion("bet");
  _have_cards   = AnySeatRegion("cardface0") || AnySeatRegion("cardrank0") || AnySeatRegion("card0");
  _have_dealer  = AnySeatRegion("dealer");
  _have_board   = RegionExists("c0cardface0") || RegionExists("c0card0") || RegionExists("c0cardrank0");

  _nchairs = p_tablemap->nchairs();
  if (_nchairs <= 0 || _nchairs > kMaxNumberOfPlayers) {
    _nchairs = kMaxNumberOfPlayers;
  }
  _hand_number = p_handreset_detector->GetHandNumber();
  _button = _have_dealer ? p_engine_container->symbol_engine_dealerchair()->dealerchair() : kUndefined;
  _hero   = p_engine_container->symbol_engine_userchair()->userchair();
  _sb     = p_engine_container->symbol_engine_tablelimits()->sblind();
  _bb     = p_engine_container->symbol_engine_tablelimits()->bblind();
  _ante   = p_engine_container->symbol_engine_tablelimits()->ante();

  int dealtbits = p_engine_container->symbol_engine_active_dealt_playing()->playersdealtbits();
  for (int i = 0; i < _nchairs; ++i) {
    bool in_hand = ((dealtbits & (1 << i)) != 0) || p_table_state->Player(i)->active();
    _seat_in_hand[i] = in_hand;
    _seat_name[i]    = FmtName(i);
    _seat_stack[i]   = p_table_state->Player(i)->stack();
    _street_bet[i]   = 0.0;
    _folded[i]       = false;
  }
  _cur_street = BETROUND;
  _street_max = 0.0;
  _meta_captured = true;
  _hand_dirty    = true;

  // Blinds / antes (only reconstructable if we can read bets and started preflop).
  if (_have_bet && (BETROUND == kBetroundPreflop)) {
    int last_chair  = (_button >= 0 && _button < _nchairs) ? _button : (_nchairs - 1);
    int first_chair = (last_chair + 1) % _nchairs;
    bool sb_seen = false;
    bool bb_seen = false;
    for (int k = 0; k < _nchairs; ++k) {
      int i = (first_chair + k) % _nchairs;
      double cb = p_table_state->Player(i)->_bet.GetValue();
      if (cb <= 0) continue;
      if (sb_seen && bb_seen) {
        if (cb < _sb - kEpsilon) {
          _body += FmtName(i) + " posts ante " + FmtMoney(cb) + "\n";
        }
        // Additional big-blind-sized bets can't be told apart from callers; skip.
        continue;
      }
      if (sb_seen) {
        _body += FmtName(i) + " posts big blind " + FmtMoney(cb) + "\n";
        bb_seen = true;
        _street_bet[i] = cb;
        if (cb > _street_max) _street_max = cb;
        continue;
      }
      // No blind seen yet: usually the small blind, possibly a lone big blind.
      if (cb <= _sb + kEpsilon) {
        _body += FmtName(i) + " posts small blind " + FmtMoney(cb) + "\n";
        sb_seen = true;
      } else {
        // Big blind with a missing / dead small blind.
        _body += FmtName(i) + " posts big blind " + FmtMoney(cb) + "\n";
        sb_seen = true;
        bb_seen = true;
      }
      _street_bet[i] = cb;
      if (cb > _street_max) _street_max = cb;
    }
    _blinds_done = true;
  } else if (BETROUND > kBetroundPreflop) {
    _body += "(joined table mid-hand; blinds not observed)\n";
  } else if (!_have_bet) {
    _body += "(bet regions missing from tablemap; blinds not observed)\n";
  }

  // Hero hole cards.
  _body += "*** HOLE CARDS ***\n";
  if (_hero >= 0 && _hero < _nchairs) {
    _body += "Dealt to " + FmtName(_hero) + " [" + FmtHoleCards(_hero) + "]\n";
  } else {
    _body += "Dealt to hero [??] (hero seat unknown)\n";
  }
}

void CHandHistoryWriter::ObserveStreetTransition() {
  int br = BETROUND;
  if (br <= _cur_street) return;
  // Log every street we have advanced into (handles fast multi-street jumps).
  if (br >= kBetroundFlop && !_flop_logged) {
    _body += "*** FLOP *** [" + BoardTokens(3) + "]\n";
    _flop_logged = true;
  }
  if (br >= kBetroundTurn && !_turn_logged) {
    _body += "*** TURN *** [" + BoardTokens(4) + "]\n";
    _turn_logged = true;
  }
  if (br >= kBetroundRiver && !_river_logged) {
    _body += "*** RIVER *** [" + BoardTokens(5) + "]\n";
    _river_logged = true;
  }
  // New street: bets are pushed to the pot, so the per-street baseline resets.
  for (int i = 0; i < _nchairs; ++i) {
    _street_bet[i] = 0.0;
  }
  _street_max = 0.0;
  _cur_street = br;
}

void CHandHistoryWriter::ObserveActions() {
  if (BETROUND < kBetroundPreflop) return;
  int activebits = p_engine_container->symbol_engine_active_dealt_playing()->playersactivebits();
  for (int i = 0; i < _nchairs; ++i) {
    if (!_seat_in_hand[i]) continue;
    if (_folded[i]) continue;
    bool active = ((activebits & (1 << i)) != 0);
    if (!active && !p_table_state->Player(i)->IsAllin()) {
      _folded[i] = true;
      _body += FmtName(i) + " folds\n";
      continue;
    }
    if (!_have_bet) continue;
    double cur = p_table_state->Player(i)->_bet.GetValue();
    if (cur > _street_bet[i] + kEpsilon) {
      if (cur > _street_max + kEpsilon) {
        const char *verb = (_street_max <= kEpsilon) ? "bets " : "raises to ";
        _body += FmtName(i) + " " + verb + FmtMoney(cur) + "\n";
        _street_max = cur;
      } else {
        _body += FmtName(i) + " calls " + FmtMoney(cur) + "\n";
      }
      _street_bet[i] = cur;
    }
  }
}

void CHandHistoryWriter::ObserveResult() {
  // Track the largest pot we ever saw this hand.
  double pot = p_engine_container->symbol_engine_chip_amounts()->pot();
  if (pot > _final_pot) _final_pot = pot;

  // Capture any cards that become visible (hero + shown hands at showdown).
  for (int i = 0; i < _nchairs; ++i) {
    if (p_table_state->Player(i)->HasKnownCards()) {
      _hole[i] = FmtHoleCards(i);
    }
  }

  int ndealt  = p_engine_container->symbol_engine_active_dealt_playing()->nplayersdealt();
  int nactive = p_engine_container->symbol_engine_active_dealt_playing()->nplayersactive();

  // Uncontested win: everybody but one folded.
  if (_winner_uncontested < 0 && ndealt >= 2 && nactive == 1) {
    int activebits = p_engine_container->symbol_engine_active_dealt_playing()->playersactivebits();
    for (int i = 0; i < _nchairs; ++i) {
      if (activebits & (1 << i)) {
        _winner_uncontested = i;
        _body += FmtName(i) + " wins the pot (" + FmtMoney(_final_pot) + ") uncontested\n";
        break;
      }
    }
  }

  // Showdown: at the river with opponent cards visible.
  if (!_showdown_logged && BETROUND == kBetroundRiver) {
    bool any_shown = false;
    for (int i = 0; i < _nchairs; ++i) {
      if (i == _hero) continue;
      if (p_table_state->Player(i)->HasKnownCards()) { any_shown = true; break; }
    }
    if (any_shown) {
      _showdown_logged = true;
      _body += "*** SHOW DOWN ***\n";
      for (int i = 0; i < _nchairs; ++i) {
        if (p_table_state->Player(i)->HasKnownCards()) {
          _body += FmtName(i) + " shows [" + _hole[i] + "]\n";
        }
      }
    }
  }
}

void CHandHistoryWriter::Flush() {
  if (!_meta_captured || !_hand_dirty) return;
  EnsureOutputPath();
  if (_output_file.IsEmpty()) return;

  CString out;
  out += "================================================================\n";
  CString head;
  head.Format("Hand #%s   (approximate, reconstructed from screen-scraping)\n",
              _hand_number.IsEmpty() ? "?" : _hand_number.GetString());
  out += head;
  out += "Placeholders: \"Seat N\"=name unknown, ?=card unknown, ??=amount/board unknown\n";

  CString table_line;
  CString button_text;
  if (_have_dealer && _button >= 0 && _button < _nchairs) {
    button_text.Format("Seat %d", _button);
  } else {
    button_text = "unknown";
  }
  table_line.Format("Table %d-max | Blinds %s/%s",
                    _nchairs, FmtMoney(_sb).GetString(), FmtMoney(_bb).GetString());
  out += table_line;
  if (_ante > kEpsilon) {
    out += " | Ante " + FmtMoney(_ante);
  }
  out += " | Button: " + button_text + "\n";

  out += "Seats:\n";
  for (int i = 0; i < _nchairs; ++i) {
    if (!_seat_in_hand[i]) continue;
    CString seat;
    seat.Format("  Seat %d: %s (%s in chips)%s\n",
                i, FmtName(i).GetString(), FmtStack(i).GetString(),
                (i == _hero) ? " -- HERO" : "");
    out += seat;
  }

  out += _body;

  out += "*** SUMMARY ***\n";
  out += "Total pot " + FmtMoney(_final_pot) + " | Board [" + BoardTokens(5) + "]\n";
  out += "================================================================\n\n";

  FILE *fp = NULL;
  if (fopen_s(&fp, _output_file.GetString(), "a") == 0 && fp != NULL) {
    fwrite(out.GetString(), 1, out.GetLength(), fp);
    fclose(fp);
    write_log(k_always_log_basic_information,
              "[CHandHistoryWriter] Wrote hand #%s to %s\n",
              _hand_number.IsEmpty() ? "?" : _hand_number.GetString(),
              _output_file.GetString());
  }
  _hand_dirty = false;
}

// ---------------------------------------------------------------------------
// formatting helpers (placeholder-aware)
// ---------------------------------------------------------------------------

CString CHandHistoryWriter::FmtName(int chair) {
  if (chair < 0 || chair >= kMaxNumberOfPlayers) return "Seat ?";
  if (_have_names) {
    CString n = p_table_state->Player(chair)->name();
    n.Trim();
    if (!n.IsEmpty()) return n;
  }
  CString s;
  s.Format("Seat %d", chair);
  return s;
}

CString CHandHistoryWriter::FmtStack(int chair) {
  if (!_have_balance) return "?";
  return FmtMoney(_seat_stack[chair]);
}

CString CHandHistoryWriter::FmtMoney(double v) {
  CString s;
  s.Format("%.2f", v);
  return s;
}

CString CHandHistoryWriter::FmtHoleCards(int chair) {
  if (!_have_cards) return "??";
  CString result;
  for (int j = 0; j < kMaxNumberOfCardsPerPlayer; ++j) {
    Card *c = p_table_state->Player(chair)->hole_cards(j);
    if (c == NULL) continue;
    if (c->IsKnownCard()) {
      if (!result.IsEmpty()) result += " ";
      result += c->ToString();
    }
  }
  if (result.IsEmpty()) return "? ?";
  return result;
}

CString CHandHistoryWriter::BoardTokens(int upto_count) {
  if (!_have_board) return "??";
  CString result;
  int limit = (upto_count < kNumberOfCommunityCards) ? upto_count : kNumberOfCommunityCards;
  for (int j = 0; j < limit; ++j) {
    Card *c = p_table_state->CommonCards(j);
    if (c == NULL) continue;
    if (c->IsKnownCard()) {
      if (!result.IsEmpty()) result += " ";
      result += c->ToString();
    }
  }
  return result;
}

bool CHandHistoryWriter::AnySeatRegion(const char *suffix) {
  for (int i = 0; i < kMaxNumberOfPlayers; ++i) {
    CString name;
    name.Format("p%d%s", i, suffix);
    if (RegionExists(name)) return true;
  }
  return false;
}

bool CHandHistoryWriter::RegionExists(const CString &name) {
  if (p_tablemap == NULL) return false;
  return p_tablemap->ItemExists(name);
}

void CHandHistoryWriter::EnsureOutputPath() {
  if (!_output_file.IsEmpty()) return;
  CString folder = OpenHoldemDirectory() + "\\handhistory";
  CreateDirectory(folder, NULL);
  int sid = (p_sessioncounter != NULL) ? p_sessioncounter->session_id() : 0;
  _output_file.Format("%s\\hh_session_%d.txt", folder.GetString(), sid);
}

// ---------------------------------------------------------------------------
// Legacy public API, kept so the (now neutered) sibling engines still compile.
// These are intentionally no-ops: all recording happens in this engine.
// ---------------------------------------------------------------------------

void CHandHistoryWriter::AddMessage(CString message)    {}
void CHandHistoryWriter::PostsSmallBlind(int chair)     {}
void CHandHistoryWriter::PostsBigBlind(int chair)       {}
void CHandHistoryWriter::PostsAnte(int chair)           {}
void CHandHistoryWriter::Checks(int chair)              {}
void CHandHistoryWriter::Folds(int chair)               {}
void CHandHistoryWriter::Calls(int chair)               {}
void CHandHistoryWriter::Raises(int chair)              {}
void CHandHistoryWriter::WinsUncontested(int chair)     {}

bool CHandHistoryWriter::EvaluateSymbol(const CString name, double *result, bool log /* = false */) {
  // No symbols provided
  return false;
}

CString CHandHistoryWriter::SymbolsProvided() {
  // No symbols provided
  return "";
}
