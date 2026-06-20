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
#include "CLogWriter.h"
#include "CSessionCounter.h"
#include "CSymbolEngineActiveDealtPlaying.h"
#include "CSymbolEngineTimingTells.h"
#include "inlines/eval.h"   // pokereval: Hand_EVAL_N + HandVal_* decode for showdown text
#include "CSymbolEngineChipAmounts.h"
#include "CSymbolEngineDealerchair.h"
#include "CSymbolEngineTableLimits.h"
#include "CSymbolEngineUserchair.h"
#include "CTableState.h"
#include "..\CTablemap\CTablemap.h"
#include "..\DLLs\Files_DLL\Files.h"

#include <set>
#include <string>
#include <map>

const double kEpsilon = 0.0001;

CHandHistoryWriter *p_handhistory_writer = NULL;

CHandHistoryWriter::CHandHistoryWriter() {
  // This engine collects data from the table-state and the other engines
  // and therefore must be executed after all the rest (it is registered last).
  _output_complete = "";
  _output_incomplete = "";
  _tourney_title = "";
  _tourney_id = "";
  _table_name = "";
  for (int i = 0; i < kMaxNumberOfPlayers; ++i) _known_name[i] = "";
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
  // Cache real names every heartbeat (when no status overlay is showing) so the
  // hand history never names a player "ANTE" / "PostSB" / "PostBB".
  ObserveNames();
  // Scrape the tournament name/id once (cheap: only until both are found).
  ScrapeTourneyInfo();
  // Publish a stable per-table identity for /api/table-state consumers. The synapse phantom guard
  // uses it to reject a hand-result stack-delta that spans a table SWITCH (same-config switches are
  // invisible from stacks/blinds/seat-count alone). tourney_id|table_name changes on any switch.
  g_table_identity = _tourney_id + "|" + _table_name;
  // Detect Omaha/PLO/Hi-Lo PER TABLE so the loader can auto-switch to the "<name>_omaha" 4-card map.
  // SETUP (Emrald): possibly several Hiss instances (one per scrcpy/phone), and EACH instance tabs
  // between MULTIPLE tables. This static map is per-PROCESS (=> per instance); keying it by the
  // table identity makes it per-TABLE within the instance. So tabbing PLO8 <-> NLH on one phone never
  // bleeds one table's game type onto the other, and each instance keeps its own tables' state.
  {
    // Strong per-FELT proof from the hero's hole cards (always the CURRENT foreground table):
    //   4 DISTINCT valid hole cards => Omaha (distinctness rejects phantom duplicate reads on a NLH
    //   felt under the always-loaded 4-card map); exactly 2 known + slots 2,3 NoCard => Hold'em.
    bool four_distinct = false, two_only = false;
    if (p_tablemap != NULL && p_tablemap->SupportsOmaha() && p_table_state != NULL) {
      Card *h0 = p_table_state->User()->hole_cards(0);
      Card *h1 = p_table_state->User()->hole_cards(1);
      Card *h2 = p_table_state->User()->hole_cards(2);
      Card *h3 = p_table_state->User()->hole_cards(3);
      bool k0 = h0 && h0->IsKnownCard(), k1 = h1 && h1->IsKnownCard();
      bool k2 = h2 && h2->IsKnownCard(), k3 = h3 && h3->IsKnownCard();
      if (k0 && k1 && k2 && k3) {
        CString s0 = h0->ToString(), s1 = h1->ToString(), s2 = h2->ToString(), s3 = h3->ToString();
        four_distinct = (s0 != s1 && s0 != s2 && s0 != s3 && s1 != s2 && s1 != s3 && s2 != s3);
      }
      two_only = (k0 && k1 && !k2 && !k3);
    }
    // Name signals (used ONLY to seed a table before card proof exists). The Claude/lobby-posted
    // g_tgi_gametype is folded in but is NOT authoritative over card proof -- that is the bug we are
    // fixing: a STALE posted "holdem" must never beat the real 4 cards on a PLO felt. [Emrald]
    CString live_name, live_title, live_id;
    if (p_scraper != NULL) {
      p_scraper->EvaluateRegion("c0table_name", &live_name);
      p_scraper->EvaluateRegion("c0tourney_title", &live_title);
      p_scraper->EvaluateRegion("c0tourney_id", &live_id);
    }
    CString live = live_name + " " + live_title + " " + live_id + " " + _table_name + " "
                 + _tourney_title + " " + _tourney_id + " " + g_tgi_gametype;
    live.MakeLower();
    bool name_omaha  = (live.Find("omaha") >= 0 || live.Find("plo") >= 0 || live.Find("hi-lo") >= 0
                        || live.Find("hi/lo") >= 0 || live.Find("8 or better") >= 0 || live.Find("8orb") >= 0);
    bool name_holdem = (!name_omaha && (live.Find("hold") >= 0 || live.Find("no limit") >= 0
                        || live.Find("nolimit") >= 0 || live.Find("no-limit") >= 0 || live.Find("nlh") >= 0));

    // PER-TABLE game-type cache keyed by the (refreshed each heartbeat) table identity.
    // [Emrald: "create an entire symbol and game state and validation state per game"]
    struct TableGameState { bool is_omaha; bool omaha_confirmed; bool holdem_confirmed; int four_streak; DWORD last_tick; };
    static std::map<std::string, TableGameState> s_table_state;
    std::string key((LPCSTR)CStringA(g_table_identity));
    TableGameState &st = s_table_state[key];   // value-initialised (all false / 0) on first use
    st.last_tick = GetTickCount();
    // Card proof is AUTHORITATIVE and sticky per table. Require 4-distinct on 2 heartbeats to latch
    // Omaha (kills a one-frame phantom); a clean 2-card read confirms Hold'em unless Omaha is proven.
    if (four_distinct) {
      if (++st.four_streak >= 2) { st.is_omaha = true; st.omaha_confirmed = true; }
    } else {
      st.four_streak = 0;
    }
    if (two_only && !st.omaha_confirmed) { st.is_omaha = false; st.holdem_confirmed = true; }
    // Before any card proof for THIS table, seed from the name (posted gametype folded into `live`).
    if (!st.omaha_confirmed && !st.holdem_confirmed) {
      if (name_omaha) st.is_omaha = true;
      else if (name_holdem) st.is_omaha = false;
    }
    g_table_is_omaha = st.is_omaha;
  }
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
  _last_active_chair = kUndefined;
  _sb_chair = _bb_chair = kUndefined;
  _street_max    = 0.0;
  _blinds_done   = false;
  _body          = "";
  _board_flop = _board_turn = _board_river = "";
  _flop_logged = _turn_logged = _river_logged = false;
  _winner_uncontested = kUndefined;
  _showdown_logged = false;
  _uncalled_emitted = false;
  _showdown_winner = kUndefined;
  _joined_midhand = false;
  _final_pot = 0.0;
  _hand_decided = false;
  for (int i = 0; i < kMaxNumberOfPlayers; ++i) {
    _seat_name[i]    = "";
    _seat_stack[i]   = 0.0;
    _seat_in_hand[i] = false;
    _hole[i]         = "";
    _hand_desc[i]    = "";
    _hand_bracket[i] = "";
    _street_bet[i]   = 0.0;
    _prev_balance[i] = -1.0;
    _folded[i]       = false;
    _fold_street[i]  = kUndefined;
    _voluntary_bet[i] = false;
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

  // Tournament level changes as blinds rise, so re-scrape it each hand (unlike the
  // session-constant title/id). Keep only the digits (e.g. "Level 3" -> "3").
  _tourney_level = "";
  if (p_scraper != NULL && RegionExists("c0tourney_level")) {
    CString lv;
    if (p_scraper->EvaluateRegion("c0tourney_level", &lv)) {
      CString digits;
      for (int i = 0; i < lv.GetLength(); ++i) if (lv[i] >= '0' && lv[i] <= '9') digits += lv[i];
      if (!digits.IsEmpty()) _tourney_level = digits;
    }
  }

  int dealtbits = p_engine_container->symbol_engine_active_dealt_playing()->playersdealtbits();
  for (int i = 0; i < _nchairs; ++i) {
    bool in_hand = ((dealtbits & (1 << i)) != 0) || p_table_state->Player(i)->active();
    _seat_in_hand[i] = in_hand;
    _seat_name[i]    = FmtName(i);
    _seat_stack[i]   = p_table_state->Player(i)->stack();
    _street_bet[i]   = 0.0;
    _prev_balance[i] = -1.0;
    _folded[i]       = false;
  }
  _cur_street = BETROUND;
  _street_max = 0.0;
  _meta_captured = true;
  _hand_dirty    = true;
  _joined_midhand = (BETROUND > kBetroundPreflop);

  // Blinds / antes (only reconstructable if we can read bets and started preflop).
  if (_have_bet && (BETROUND == kBetroundPreflop)) {
    int last_chair  = (_button >= 0 && _button < _nchairs) ? _button : (_nchairs - 1);
    int first_chair = (last_chair + 1) % _nchairs;
    // ACR posts one ante line per player before the blinds. Antes are dead money
    // (they don't count toward the per-street bet to call).
    if (_ante > kEpsilon) {
      for (int k = 0; k < _nchairs; ++k) {
        int i = (first_chair + k) % _nchairs;
        if (_seat_in_hand[i] && p_table_state->Player(i)->_bet.GetValue() >= _ante - kEpsilon) {
          _body += FmtName(i) + " posts ante " + FmtMoney(_ante) + "\n";
        }
      }
    }
    bool sb_seen = false;
    bool bb_seen = false;
    for (int k = 0; k < _nchairs; ++k) {
      int i = (first_chair + k) % _nchairs;
      double cb = p_table_state->Player(i)->_bet.GetValue();
      // The blind is the bet beyond the (already-accounted) dead ante.
      if (_ante > kEpsilon) cb -= _ante;
      if (cb <= kEpsilon) continue;       // ante-only player (no blind)
      if (sb_seen && bb_seen) {
        // Additional blind-sized bets can't be told apart from callers; skip.
        continue;
      }
      if (sb_seen) {
        _body += FmtName(i) + " posts the big blind " + FmtMoney(cb) + "\n";
        bb_seen = true;
        _bb_chair = i;
        _street_bet[i] = cb;
        if (cb > _street_max) _street_max = cb;
        continue;
      }
      // No blind seen yet: usually the small blind, possibly a lone big blind.
      if (cb <= _sb + kEpsilon) {
        _body += FmtName(i) + " posts the small blind " + FmtMoney(cb) + "\n";
        sb_seen = true;
        _sb_chair = i;
      } else {
        // Big blind with a missing / dead small blind.
        _body += FmtName(i) + " posts the big blind " + FmtMoney(cb) + "\n";
        sb_seen = true;
        bb_seen = true;
        _bb_chair = i;
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
    // Use a token for the hero cards; the deal may not be scraped yet at this first
    // heartbeat, so resolve it at Flush from the best cards seen during the hand.
    _body += "Dealt to " + FmtName(_hero) + " [\x01HOLE\x01]\n";
  } else {
    _body += "Dealt to hero [? ?] (hero seat unknown)\n";
  }
}

void CHandHistoryWriter::ObserveStreetTransition() {
  if (_hand_decided) return;   // hand already won -> no more streets (no phantom board)
  int br = BETROUND;
  if (br <= _cur_street) return;
  // The previous street's betting just closed: return any uncalled bet first.
  EmitUncalledReturn();
  // Log every street we have advanced into (handles fast multi-street jumps).
  // ACR uses split board brackets: TURN/RIVER repeat the prior board then the new card.
  CString potline = "Main pot " + FmtMoney(p_engine_container->symbol_engine_chip_amounts()->pot()) + "\n";
  if (br >= kBetroundFlop && !_flop_logged) {
    _body += "*** FLOP *** [" + BoardTokens(3) + "]\n" + potline;
    _flop_logged = true;
  }
  if (br >= kBetroundTurn && !_turn_logged) {
    _body += "*** TURN *** [" + BoardTokens(3) + "] [" + BoardCard(3) + "]\n" + potline;
    _turn_logged = true;
  }
  if (br >= kBetroundRiver && !_river_logged) {
    _body += "*** RIVER *** [" + BoardTokens(4) + "] [" + BoardCard(4) + "]\n" + potline;
    _river_logged = true;
  }
  // New street: bets are pushed to the pot, so the per-street baseline resets.
  for (int i = 0; i < _nchairs; ++i) {
    _street_bet[i] = 0.0;
  }
  _street_max = 0.0;
  _last_active_chair = kUndefined;   // start the new street's check detection clean
  _cur_street = br;
}

void CHandHistoryWriter::ObserveActions() {
  if (_hand_decided) return;   // hand already won -> no more action lines
  if (BETROUND < kBetroundPreflop) return;
  // Fold detection keys off PLAYING (card presence), NOT ACTIVE. On ACR phone
  // tablemaps the "active" region marks only the seat whose turn it is (nactive==1),
  // so an active-based test instantly mislabels every other seat as folded -- which
  // suppresses every call/raise line and leaves all HUD numerators at 0. A seat that
  // was in the hand and no longer holds cards has folded.
  int playingbits = p_engine_container->symbol_engine_active_dealt_playing()->playersplayingbits();
  for (int i = 0; i < _nchairs; ++i) {
    if (!_seat_in_hand[i]) continue;
    if (_folded[i]) continue;
    bool playing = ((playingbits & (1 << i)) != 0);
    if (!playing && !p_table_state->Player(i)->IsAllin()) {
      _folded[i] = true;
      _fold_street[i] = BETROUND;
      _body += FmtName(i) + " folds\n";
      continue;
    }
    // Action size from STACK DROP (reliable scrape) rather than the flaky bet-chip OCR:
    // a seat's total committed THIS STREET = chips put in since the street baseline. Track
    // the last observed stack and accumulate decreases onto _street_bet (blinds pre-seed it).
    // Falls back to the bet region only if balances aren't readable.
    double cur;
    double bal = _have_balance ? p_table_state->Player(i)->_balance.GetValue() : 0.0;
    if (_have_balance && bal > 0.0) {
      if (_prev_balance[i] < 0.0) _prev_balance[i] = bal;  // first sighting baseline
      double delta = _prev_balance[i] - bal;               // chips committed since last observation
      _prev_balance[i] = bal;
      if (delta < kEpsilon) delta = 0.0;
      cur = _street_bet[i] + delta;                        // total committed this street
    } else if (_have_bet) {
      // Stack not readable this tick (bal<=0: a momentary stack-OCR miss, or an all-in that zeroed
      // the stack). The OLD code did `continue` here -> it DROPPED the action entirely (and every
      // all-in). Fall back to the bet region, which reads chips committed THIS STREET directly, so
      // the bet/raise is still recorded. Re-baseline the stack tracker (-1) so the next good stack
      // read starts a fresh delta and doesn't double-count what we just booked off the bet region.
      double b = p_table_state->Player(i)->_bet.GetValue();
      _prev_balance[i] = -1.0;
      if (b <= _street_bet[i] + kEpsilon) continue;        // nothing new committed this street
      cur = b;
    } else {
      continue;
    }
    if (cur > _street_bet[i] + kEpsilon) {
      CString allin = p_table_state->Player(i)->IsAllin() ? " and is all-in" : "";
      if (cur > _street_max + kEpsilon) {
        if (_street_max <= kEpsilon) {
          // Opening bet on this street.
          _body += FmtName(i) + " bets " + FmtMoney(cur) + allin + "\n";
        } else {
          // ACR raise syntax: "raises <by> to <to>", where <by> is the chips THIS
          // player adds over their own prior bet this street (not over the current
          // level) -- e.g. an opening raise to 360 over the BB prints "raises 360 to 360".
          _body += FmtName(i) + " raises " + FmtMoney(cur - _street_bet[i])
                 + " to " + FmtMoney(cur) + allin + "\n";
        }
        _street_max = cur;
      } else {
        _body += FmtName(i) + " calls " + FmtMoney(cur - _street_bet[i]) + allin + "\n";
      }
      _voluntary_bet[i] = true;   // put chips in beyond a forced blind
      _street_bet[i] = cur;
    }
  }

  // --- check detection (postflop) -------------------------------------------
  // The to-act highlight (pNactive) moving off a player who is still in the hand,
  // owes nothing (no bet faced this street), and has not put chips in = a check.
  // Folds (activebits) and calls/raises (bet increases) are handled above; this
  // only fires while nobody has bet this street, so it never mis-labels a bet.
  if (BETROUND >= kBetroundFlop && _street_max <= kEpsilon
      && p_engine_container->symbol_engine_timing_tells() != NULL) {
    int act = p_engine_container->symbol_engine_timing_tells()->CurrentActiveChair();
    if (act != _last_active_chair) {
      int prev = _last_active_chair;
      _last_active_chair = act;
      if (prev >= 0 && prev < _nchairs && _seat_in_hand[prev] && !_folded[prev]) {
        double pbet = _have_bet ? p_table_state->Player(prev)->_bet.GetValue() : 0.0;
        if (pbet <= kEpsilon) {
          _body += FmtName(prev) + " checks\n";
        }
      }
    }
  }
}

void CHandHistoryWriter::ObserveResult() {
  if (_hand_decided) return;   // result already recorded -> freeze pot/cards/winner
  // Track the largest pot we ever saw this hand.
  double pot = p_engine_container->symbol_engine_chip_amounts()->pot();
  if (pot > _final_pot) _final_pot = pot;

  // Capture any cards that become visible (hero + shown hands at showdown).
  for (int i = 0; i < _nchairs; ++i) {
    if (p_table_state->Player(i)->HasKnownCards()) {
      _hole[i] = FmtHoleCards(i);
    }
  }

  int ndealt   = p_engine_container->symbol_engine_active_dealt_playing()->nplayersdealt();
  int nplaying = p_engine_container->symbol_engine_active_dealt_playing()->nplayersplaying();

  // Uncontested win: everybody but one folded -> exactly one seat still holds cards.
  // Use PLAYING, not ACTIVE: the "active" region marks only the to-act seat on phone
  // tablemaps, so an active-based test crowns the wrong player.
  if (_winner_uncontested < 0 && ndealt >= 2 && nplaying == 1) {
    int playingbits = p_engine_container->symbol_engine_active_dealt_playing()->playersplayingbits();
    for (int i = 0; i < _nchairs; ++i) {
      if (playingbits & (1 << i)) {
        EmitUncalledReturn();   // return any uncalled bet before awarding the pot
        _winner_uncontested = i;
        _body += FmtName(i) + " collected " + FmtMoney(_final_pot) + " from pot\n";
        _hand_decided = true;   // terminal result: stop appending streets/actions/pot
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
      _body += "Main pot " + FmtMoney(_final_pot) + "\n";
      // Evaluate every shown hand, emit the ACR "shows [..] (<desc> [5cards])" line,
      // and pick the winner (highest handval).
      int best_hv = -1;
      _showdown_winner = kUndefined;
      for (int i = 0; i < _nchairs; ++i) {
        if (!p_table_state->Player(i)->HasKnownCards()) continue;
        CString d, br; int hv = -1;
        if (DescribeShowdownHand(i, d, br, hv)) {
          _hand_desc[i] = d; _hand_bracket[i] = br;
          _body += FmtName(i) + " shows [" + _hole[i] + "] (" + d + " " + br + ")\n";
          if (hv > best_hv) { best_hv = hv; _showdown_winner = i; }
        } else {
          _body += FmtName(i) + " shows [" + _hole[i] + "]\n";
        }
      }
      if (_showdown_winner != kUndefined) {
        _body += FmtName(_showdown_winner) + " collected " + FmtMoney(_final_pot) + " from main pot\n";
      }
      _hand_decided = true;   // showdown recorded: terminal result, stop appending
    }
  }
}

void CHandHistoryWriter::Flush() {
  if (!_meta_captured || !_hand_dirty) return;
  EnsureOutputPath();

  // ---- ACR-format hand history (Americas Cardroom layout, for PokerTracker 4) --
  CString out;
  out += AcrHeader() + "\n";

  // Table + button line. ACR seats are 1-based; the button must be a seat number.
  int button_seat = (_have_dealer && _button >= 0 && _button < _nchairs)
                  ? AcrSeat(_button) : 1;
  // Use the scraped table name when available, else the session id.
  CString table_label = _table_name;
  table_label.Trim();
  if (table_label.IsEmpty()) {
    table_label.Format("%d", (p_sessioncounter != NULL) ? p_sessioncounter->session_id() : 0);
  }
  CString table_line;
  table_line.Format("Table '%s' %d-max Seat #%d is the button\n",
                    table_label.GetString(),
                    (_nchairs > 0 ? _nchairs : kMaxNumberOfPlayers), button_seat);
  out += table_line;

  // Seat list (only players that were in the hand).
  for (int i = 0; i < _nchairs; ++i) {
    if (!_seat_in_hand[i]) continue;
    CString seat;
    seat.Format("Seat %d: %s (%s)\n",
                AcrSeat(i), FmtName(i).GetString(), FmtStack(i).GetString());
    out += seat;
  }

  // Antes, blinds, hole cards, streets, actions, result (built in _body).
  out += _body;

  // ---- SUMMARY ----
  out += "*** SUMMARY ***\n";
  // ACR tournament summary: "Total pot X" then "Board [...]" on its own line
  // (no rake line in tournaments).
  out += "Total pot " + FmtMoney(_final_pot) + "\n";
  CString board = BoardTokens(5);
  if (!board.IsEmpty() && board != "??") out += "Board [" + board + "]\n";
  for (int i = 0; i < _nchairs; ++i) {
    if (!_seat_in_hand[i]) continue;
    CString seat;
    // Position label (ACR: button / small blind / big blind).
    CString tag;
    if (i == _button && _have_dealer) tag = " (button)";
    else if (i == _sb_chair)          tag = " (small blind)";
    else if (i == _bb_chair)          tag = " (big blind)";
    CString fate;
    if (i == _winner_uncontested) {
      fate.Format("collected (%s)", FmtMoney(_final_pot).GetString());
    } else if (_folded[i]) {
      // "folded on the <Street>", with "and did not bet" when the player put no
      // chips in voluntarily (forced blinds don't count).
      int fs = (_fold_street[i] == kUndefined) ? kBetroundPreflop : _fold_street[i];
      fate = "folded on the " + StreetName(fs);
      bool is_blind = (i == _sb_chair || i == _bb_chair);
      if (!_voluntary_bet[i] && !is_blind) fate += " and did not bet";
    } else if (i == _showdown_winner && !_hand_desc[i].IsEmpty()) {
      fate.Format("showed [%s] and won %s with %s %s", _hole[i].GetString(),
                  FmtMoney(_final_pot).GetString(), _hand_desc[i].GetString(),
                  _hand_bracket[i].GetString());
    } else if (!_hand_desc[i].IsEmpty()) {
      fate.Format("showed [%s] and lost with %s %s", _hole[i].GetString(),
                  _hand_desc[i].GetString(), _hand_bracket[i].GetString());
    } else if (!_hole[i].IsEmpty()) {
      fate.Format("showed [%s]", _hole[i].GetString());
    } else {
      fate = "mucked";
    }
    seat.Format("Seat %d: %s%s %s\n", AcrSeat(i), FmtName(i).GetString(),
                tag.GetString(), fate.GetString());
    out += seat;
  }
  out += "\n\n";

  // Resolve every per-chair name token to a single consistent real name.
  for (int i = 0; i < _nchairs; ++i) {
    CString token;
    token.Format("\x01P%d\x01", i);
    out.Replace(token, ResolveName(i));
  }
  // Resolve the hero hole-cards token to the best cards seen this hand.
  CString hero_cards = (_hero >= 0 && _hero < kMaxNumberOfPlayers && !_hole[_hero].IsEmpty())
                     ? _hole[_hero] : FmtHoleCards(_hero);
  out.Replace("\x01HOLE\x01", hero_cards);

  // ---- route to complete\ or incomplete\ depending on import quality ----
  bool complete = HandLooksComplete();
  // Replay logging: store the full ACR hand text as a hiss_log_hands row (background writer).
  if (p_log_writer != NULL && p_log_writer->Enabled()) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    long long ms = (long long)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
    p_log_writer->LogHand(ms, CStringA(_hand_number).GetString(), complete, CStringA(out).GetString());
  }
  CString path = complete ? _output_complete : _output_incomplete;
  if (path.IsEmpty()) { _hand_dirty = false; return; }

  FILE *fp = NULL;
  if (fopen_s(&fp, path.GetString(), "a") == 0 && fp != NULL) {
    fwrite(out.GetString(), 1, out.GetLength(), fp);
    fclose(fp);
    write_log(k_always_log_basic_information,
              "[CHandHistoryWriter] Wrote hand #%s (%s) to %s\n",
              _hand_number.IsEmpty() ? "?" : _hand_number.GetString(),
              complete ? "complete" : "incomplete", path.GetString());
  }
  _hand_dirty = false;
}

// A hand is import-quality ("complete") only if it has the structure PokerTracker
// needs: real names + stacks + bet amounts + a known button + hero, blinds seen
// from the start (not joined mid-hand), and a terminal result (someone won
// uncontested or a showdown was recorded). Anything else goes to incomplete\.
bool CHandHistoryWriter::HandLooksComplete() {
  if (_joined_midhand) return false;
  if (!_have_names || !_have_balance || !_have_bet || !_have_dealer) return false;
  // Observer hands have NO hero (userchair = -1) yet are fully import-quality for PT4
  // opponent stats -- PT4 does not need a hero to import a hand. Only reject a hero
  // index that is out of range (a corrupt scrape), not the legitimate "no hero" case.
  if (_hero >= _nchairs) return false;
  if (!_blinds_done) return false;
  if (_button < 0 || _button >= _nchairs) return false;
  if (_winner_uncontested < 0 && !_showdown_logged) return false;
  return true;
}

CString CHandHistoryWriter::AcrTimestampUtc() {
  SYSTEMTIME st;
  GetSystemTime(&st);   // UTC, matching ACR's "... UTC" stamp
  CString s;
  s.Format("%04d/%02d/%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay,
           st.wHour, st.wMinute, st.wSecond);
  return s;
}

CString CHandHistoryWriter::AcrHeader() {
  // Mirror ACR's tournament header; PokerTracker reads the blinds from the
  // parentheses. A stable per-session tournament id groups the session's hands.
  int sid = (p_sessioncounter != NULL) ? p_sessioncounter->session_id() : 0;
  CString hid = _hand_number;
  hid.Trim();
  if (hid.IsEmpty()) hid = "0";
  // Use the scraped tournament id when available, else the session id.
  CString tid = _tourney_id;
  tid.Trim();
  if (tid.IsEmpty()) tid.Format("%d", sid);
  // ACR prefixes the tournament id with the tournament name, e.g.
  // "$1,000 GTD Tournament #35045980". Use the scraped title when we have it.
  CString name = _tourney_title;
  name.Trim();
  CString tourney = name.IsEmpty() ? ("Tournament #" + tid)
                                   : (name + " Tournament #" + tid);
  CString level = _tourney_level;
  level.Trim();
  if (level.IsEmpty()) level = "1";   // fallback when no level region is scraped
  CString s;
  s.Format("Game Hand #%s - %s - Holdem (No Limit) - Level %s (%s/%s) - %s UTC",
           hid.GetString(), tourney.GetString(), level.GetString(),
           FmtMoney(_sb).GetString(), FmtMoney(_bb).GetString(),
           AcrTimestampUtc().GetString());
  return s;
}

// ---------------------------------------------------------------------------
// formatting helpers (placeholder-aware)
// ---------------------------------------------------------------------------

// During a hand we emit a STABLE per-chair token instead of the live name; Flush()
// replaces every token with ResolveName() once, so a player's name is identical on
// every line of the hand (PokerTracker requires that).
CString CHandHistoryWriter::FmtName(int chair) {
  CString s;
  s.Format("\x01P%d\x01", chair);
  return s;
}

// Is this scraped string an action/status overlay ("ANTE", "posts SB", "FOLD"...)
// rather than a real player name?
bool CHandHistoryWriter::LooksLikeStatus(CString n) {
  n.Trim();
  if (n.IsEmpty()) return true;
  CString u = n; u.MakeUpper();
  if (u.Left(4) == "POST") return true;        // "PostSB", "PostBB", "POSTS ANTE"
  if (u.Left(4) == "ANTE") return true;
  if (u.Find("BLIND") >= 0) return true;
  if (u.Find("SITTING") >= 0 || u.Find("SIT OUT") >= 0) return true;
  if (u.Find("ALL") >= 0 && u.Find("IN") >= 0) return true;  // "ALL IN" / "ALL-IN"
  const char *words[] = { "SB","BB","FOLD","FOLDS","CALL","CALLS","CHECK","CHECKS",
    "RAISE","RAISES","BET","BETS","WIN","WINS","WINNER","MUCK","MUCKS","DEALER",
    "EMPTY","SEAT","WAITING","JOIN","RESERVED","ANTE", NULL };
  for (int i = 0; words[i] != NULL; ++i) if (u == words[i]) return true;
  return false;
}

void CHandHistoryWriter::ScrapeTourneyInfo() {
  // Prefer Claude/MCP-parsed table_game_info (the c0tourney_* regions mis-scrape this
  // table). These win over the region scrapes for an accurate ACR header.
  if (!g_tgi_tourney_name.IsEmpty()) _tourney_title = g_tgi_tourney_name;
  if (!g_tgi_tourney_id.IsEmpty())   _tourney_id    = g_tgi_tourney_id;
  if (!g_tgi_table_number.IsEmpty()) _table_name    = g_tgi_table_number;
  if (g_tgi_level > 0) { CString l; l.Format("%.0f", g_tgi_level); _tourney_level = l; }
  if (p_scraper == NULL || p_tablemap == NULL) return;
  // When table_game_info / table_game_info_2 is in use, DO NOT fall back to the
  // c0tourney_* OCR regions -- they mis-scrape this table (truncated/garbled), so the
  // Claude/MCP-parsed values are authoritative even when some are still empty.
  if (g_tgi_set) return;
  // REFRESH the identity EVERY heartbeat. (Was: scrape-once-until-found, which left _tourney_id /
  // _table_name STUCK on the FIRST table for the whole session -> when Emrald tabs to another table on
  // the one phone, the new table's hands were mis-attributed to the first, and the per-table game-type
  // cache keyed off a stale identity.) One phone / tab-between-tables means the foreground table
  // changes, so we must re-read which table we are looking at each heartbeat.
  CString sc_title, sc_id, sc_name;
  if (RegionExists("c0tourney_title") && p_scraper->EvaluateRegion("c0tourney_title", &sc_title)) {
    sc_title.Remove(','); sc_title.Trim();
  }
  if (RegionExists("c0tourney_id") && p_scraper->EvaluateRegion("c0tourney_id", &sc_id)) {
    sc_id.Remove('('); sc_id.Remove(')'); sc_id.Trim();
  }
  if (RegionExists("c0table_name") && p_scraper->EvaluateRegion("c0table_name", &sc_name)) {
    sc_name.Remove('\''); sc_name.Trim();
  }
  // Title (HH header + the hi/lo title test): accept any non-trivial refresh.
  if (sc_title.GetLength() >= 3) _tourney_title = sc_title;
  // Identity (id|name): DEBOUNCE a change so a single garbage OCR frame cannot false-switch tables --
  // a new (id,name) must read the SAME on 2 consecutive heartbeats before we commit the switch. Empty/
  // short reads keep the previous identity (bridge an intermittent blank scrape).
  static CString s_pending_id, s_pending_name;
  static int s_pending_count = 0;
  CString new_id   = (sc_id.GetLength()   >= 3) ? sc_id   : _tourney_id;
  CString new_name = (sc_name.GetLength() >= 3) ? sc_name : _table_name;
  if (new_id != _tourney_id || new_name != _table_name) {
    if (new_id == s_pending_id && new_name == s_pending_name) {
      if (++s_pending_count >= 2) { _tourney_id = new_id; _table_name = new_name; s_pending_count = 0; }
    } else {
      s_pending_id = new_id; s_pending_name = new_name; s_pending_count = 1;
    }
  } else {
    s_pending_count = 0;
  }
}

void CHandHistoryWriter::ObserveNames() {
  if (p_tablemap == NULL || p_table_state == NULL) return;
  int n = p_tablemap->nchairs();
  if (n <= 0 || n > kMaxNumberOfPlayers) n = kMaxNumberOfPlayers;
  for (int i = 0; i < n; ++i) {
    CString nm = p_table_state->Player(i)->name();
    nm.Trim();
    if (!nm.IsEmpty() && !LooksLikeStatus(nm)) {
      _known_name[i] = nm;   // remember the latest real (non-overlay) name
    }
  }
}

CString CHandHistoryWriter::ResolveName(int chair) {
  if (chair >= 0 && chair < kMaxNumberOfPlayers && !_known_name[chair].IsEmpty()) {
    return _known_name[chair];
  }
  CString s;
  s.Format("Seat %d", AcrSeat(chair));   // ACR-style 1-based fallback
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
  // Read the ACTUAL scraped cards via IsKnownCard(), not the _have_cards region-name
  // heuristic (which can miss a tablemap's real card-region naming even though the
  // cards are scraped fine -- the hero's cards are always known, since the bot plays
  // with them, and shown opponent cards are known at showdown).
  if (chair < 0 || chair >= kMaxNumberOfPlayers || p_table_state == NULL) return "? ?";
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

CString CHandHistoryWriter::BoardCard(int idx) {
  if (!_have_board || idx < 0 || idx >= kNumberOfCommunityCards) return "?";
  Card *c = p_table_state->CommonCards(idx);
  if (c != NULL && c->IsKnownCard()) return c->ToString();
  return "?";
}

CString CHandHistoryWriter::StreetName(int betround) {
  switch (betround) {
    case kBetroundFlop:  return "Flop";
    case kBetroundTurn:  return "Turn";
    case kBetroundRiver: return "River";
    default:             return "Pre-Flop";
  }
}

// ACR rank names by StdDeck rank index (0=Two .. 12=Ace). Note ACR's quirky
// plurals: Two->"Deuces", Six->"Sixs".
static const char *kAcrSingular[13] = {
  "Deuce","Three","Four","Five","Six","Seven","Eight","Nine","Ten","Jack","Queen","King","Ace" };
static const char *kAcrPlural[13] = {
  "Deuces","Threes","Fours","Fives","Sixs","Sevens","Eights","Nines","Tens","Jacks","Queens","Kings","Aces" };

bool CHandHistoryWriter::DescribeShowdownHand(int chair, CString &desc, CString &bracket, int &handval) {
  if (chair < 0 || chair >= _nchairs || p_table_state == NULL) return false;
  if (!p_table_state->Player(chair)->HasKnownCards()) return false;
  int n = 0, rnk[7], sut[7];
  CString tok[7];
  CardMask mask; CardMask_RESET(mask);
  for (int j = 0; j < 2 && n < 7; ++j) {
    Card *c = p_table_state->Player(chair)->hole_cards(j);
    if (c != NULL && c->IsKnownCard()) {
      int v = c->GetValue();
      CardMask_SET(mask, v); rnk[n] = StdDeck_RANK(v); sut[n] = StdDeck_SUIT(v); tok[n] = c->ToString(); ++n;
    }
  }
  for (int j = 0; j < kNumberOfCommunityCards && n < 7; ++j) {
    Card *c = p_table_state->CommonCards(j);
    if (c != NULL && c->IsKnownCard()) {
      int v = c->GetValue();
      CardMask_SET(mask, v); rnk[n] = StdDeck_RANK(v); sut[n] = StdDeck_SUIT(v); tok[n] = c->ToString(); ++n;
    }
  }
  if (n < 5) return false;
  handval = Hand_EVAL_N(mask, n);
  int type = HandVal_HANDTYPE(handval);
  int t = HandVal_TOP_CARD(handval), s = HandVal_SECOND_CARD(handval);
  switch (type) {
    case HandType_NOPAIR:    desc.Format("a high card, %s high", kAcrSingular[t]); break;
    case HandType_ONEPAIR:   desc.Format("a pair of %s", kAcrPlural[t]); break;
    case HandType_TWOPAIR:   desc.Format("two pair, %s and %s", kAcrPlural[t], kAcrPlural[s]); break;
    case HandType_TRIPS:     desc.Format("three of a kind, Set of %s", kAcrPlural[t]); break;
    case HandType_STRAIGHT:  desc.Format("a straight, %s high", kAcrSingular[t]); break;
    case HandType_FLUSH:     desc.Format("a flush, %s high", kAcrSingular[t]); break;
    case HandType_FULLHOUSE: desc.Format("a full house, %s full of %s", kAcrPlural[t], kAcrPlural[s]); break;
    case HandType_QUADS:     desc.Format("four of a kind, Four %s", kAcrPlural[t]); break;
    default:                 desc.Format("a straight flush, %s high", kAcrSingular[t]); break;
  }
  // Build the 5-card bracket from the actual cards forming the hand.
  bool used[7] = { false, false, false, false, false, false, false };
  CString picks; int npick = 0;
  // pick `cnt` highest unused cards of rank r (any suit)
  // (small inline closures via index loops)
  #define HH_ADD(idx) do { if (npick) picks += " "; picks += tok[idx]; used[idx] = true; ++npick; } while (0)
  // pick up to cnt cards of a given rank
  for (int pass = 0; pass < 1; ++pass) {
    if (type == HandType_FLUSH || type == HandType_STFLUSH) {
      int fs = -1;
      for (int su = 0; su <= 3 && fs < 0; ++su) { int cc = 0; for (int k = 0; k < n; ++k) if (sut[k] == su) ++cc; if (cc >= 5) fs = su; }
      if (type == HandType_FLUSH) {
        for (int p = 0; p < 5; ++p) { int best = -1; for (int k = 0; k < n; ++k) if (!used[k] && sut[k] == fs && (best < 0 || rnk[k] > rnk[best])) best = k; if (best >= 0) HH_ADD(best); }
      } else {
        int rs[5]; if (t == 3) { rs[0]=3; rs[1]=2; rs[2]=1; rs[3]=0; rs[4]=12; } else for (int i = 0; i < 5; ++i) rs[i] = t - i;
        for (int i = 0; i < 5; ++i) for (int k = 0; k < n; ++k) if (!used[k] && sut[k] == fs && rnk[k] == rs[i]) { HH_ADD(k); break; }
      }
    } else if (type == HandType_STRAIGHT) {
      int rs[5]; if (t == 3) { rs[0]=3; rs[1]=2; rs[2]=1; rs[3]=0; rs[4]=12; } else for (int i = 0; i < 5; ++i) rs[i] = t - i;
      for (int i = 0; i < 5; ++i) for (int k = 0; k < n; ++k) if (!used[k] && rnk[k] == rs[i]) { HH_ADD(k); break; }
    } else {
      int want_r1 = -1, c1 = 0, want_r2 = -1, c2 = 0, kick = 0;
      if (type == HandType_ONEPAIR)      { want_r1 = t; c1 = 2; kick = 3; }
      else if (type == HandType_TWOPAIR) { want_r1 = t; c1 = 2; want_r2 = s; c2 = 2; kick = 1; }
      else if (type == HandType_TRIPS)   { want_r1 = t; c1 = 3; kick = 2; }
      else if (type == HandType_FULLHOUSE){ want_r1 = t; c1 = 3; want_r2 = s; c2 = 2; kick = 0; }
      else if (type == HandType_QUADS)   { want_r1 = t; c1 = 4; kick = 1; }
      else                               { kick = 5; }  // NOPAIR
      for (int got = 0; got < c1; ) { int b = -1; for (int k = 0; k < n; ++k) if (!used[k] && rnk[k] == want_r1) { b = k; break; } if (b < 0) break; HH_ADD(b); ++got; }
      for (int got = 0; got < c2; ) { int b = -1; for (int k = 0; k < n; ++k) if (!used[k] && rnk[k] == want_r2) { b = k; break; } if (b < 0) break; HH_ADD(b); ++got; }
      for (int got = 0; got < kick; ++got) { int b = -1; for (int k = 0; k < n; ++k) if (!used[k] && (b < 0 || rnk[k] > rnk[b])) b = k; if (b >= 0) HH_ADD(b); }
    }
  }
  #undef HH_ADD
  bracket = "[" + picks + "]";
  return true;
}

void CHandHistoryWriter::EmitUncalledReturn() {
  // Called when a street's betting closes. If exactly one player put in more than
  // anyone else could match, the excess is uncalled and returned (ACR shows this
  // before the next street header / the result). Handles all-in over-bets and a
  // bet everyone folded to.
  if (_uncalled_emitted || !_have_bet) return;
  int top = kUndefined; double topbet = 0.0, second = 0.0;
  for (int i = 0; i < _nchairs; ++i) {
    if (!_seat_in_hand[i]) continue;
    double b = _street_bet[i];
    if (b > topbet + kEpsilon) { second = topbet; topbet = b; top = i; }
    else if (b > second + kEpsilon) { second = b; }
  }
  double diff = topbet - second;
  if (top != kUndefined && diff > kEpsilon) {
    _body += "Uncalled bet (" + FmtMoney(diff) + ") returned to " + FmtName(top) + "\n";
    _street_bet[top] = second;   // the returned chips never entered the pot
    _uncalled_emitted = true;
  }
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
  if (!_output_complete.IsEmpty() && !_output_incomplete.IsEmpty()) return;
  CString root = OpenHoldemDirectory() + "\\handhistory";
  CreateDirectory(root, NULL);
  // Complete (PT4-import) and incomplete (review) hands are kept in separate
  // subfolders so PokerTracker 4 can be pointed at the clean folder only.
  CString complete_dir   = root + "\\complete";
  CString incomplete_dir = root + "\\incomplete";
  CreateDirectory(complete_dir, NULL);
  CreateDirectory(incomplete_dir, NULL);
  int sid = (p_sessioncounter != NULL) ? p_sessioncounter->session_id() : 0;
  // ACR-style filename so PokerTracker's ACR importer recognises it. The tournament
  // NAME goes in the "TN-" field (where PT4 reads it); fall back when not scraped.
  CString tn = _tourney_title;
  tn.Trim();
  if (tn.IsEmpty()) tn = "ScarletBeast";
  CString tbl = _table_name;
  tbl.Trim();
  if (tbl.IsEmpty()) tbl.Format("T%d", sid);
  // Strip characters that are illegal in a Windows filename.
  const char *illegal = "\\/:*?\"<>|";
  for (const char *p = illegal; *p; ++p) { tn.Remove(*p); tbl.Remove(*p); }
  SYSTEMTIME st; GetSystemTime(&st);
  CString fname;
  fname.Format("HH%04d%02d%02d %s TN-%s GAMETYPE-Hold'em LIMIT-no CUR-REAL.txt",
               st.wYear, st.wMonth, st.wDay, tbl.GetString(), tn.GetString());
  _output_complete.Format("%s\\%s", complete_dir.GetString(), fname.GetString());
  _output_incomplete.Format("%s\\%s", incomplete_dir.GetString(), fname.GetString());
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
