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
  _output_complete = "";
  _output_incomplete = "";
  _tourney_title = "";
  _tourney_id = "";
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
  _joined_midhand = false;
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
  _joined_midhand = (BETROUND > kBetroundPreflop);

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
        _body += FmtName(i) + " posts the big blind " + FmtMoney(cb) + "\n";
        bb_seen = true;
        _street_bet[i] = cb;
        if (cb > _street_max) _street_max = cb;
        continue;
      }
      // No blind seen yet: usually the small blind, possibly a lone big blind.
      if (cb <= _sb + kEpsilon) {
        _body += FmtName(i) + " posts the small blind " + FmtMoney(cb) + "\n";
        sb_seen = true;
      } else {
        // Big blind with a missing / dead small blind.
        _body += FmtName(i) + " posts the big blind " + FmtMoney(cb) + "\n";
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
    // Use a token for the hero cards; the deal may not be scraped yet at this first
    // heartbeat, so resolve it at Flush from the best cards seen during the hand.
    _body += "Dealt to " + FmtName(_hero) + " [\x01HOLE\x01]\n";
  } else {
    _body += "Dealt to hero [? ?] (hero seat unknown)\n";
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
      CString allin = p_table_state->Player(i)->IsAllin() ? " and is all-in" : "";
      if (cur > _street_max + kEpsilon) {
        if (_street_max <= kEpsilon) {
          // Opening bet on this street.
          _body += FmtName(i) + " bets " + FmtMoney(cur) + allin + "\n";
        } else {
          // ACR raise syntax: "raises <by> to <to>".
          _body += FmtName(i) + " raises " + FmtMoney(cur - _street_max)
                 + " to " + FmtMoney(cur) + allin + "\n";
        }
        _street_max = cur;
      } else {
        _body += FmtName(i) + " calls " + FmtMoney(cur - _street_bet[i]) + allin + "\n";
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
        _body += FmtName(i) + " collected " + FmtMoney(_final_pot) + " from pot\n";
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

  // ---- ACR-format hand history (Americas Cardroom layout, for PokerTracker 4) --
  CString out;
  out += AcrHeader() + "\n";

  // Table + button line. ACR seats are 1-based; the button must be a seat number.
  int button_seat = (_have_dealer && _button >= 0 && _button < _nchairs)
                  ? AcrSeat(_button) : 1;
  CString table_line;
  table_line.Format("Table '%d' %d-max Seat #%d is the button\n",
                    (p_sessioncounter != NULL) ? p_sessioncounter->session_id() : 0,
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
  out += "Total pot " + FmtMoney(_final_pot);
  CString board = BoardTokens(5);
  if (!board.IsEmpty() && board != "??") out += " | Board [" + board + "]";
  out += "\n";
  for (int i = 0; i < _nchairs; ++i) {
    if (!_seat_in_hand[i]) continue;
    CString seat;
    CString tag;
    if (i == _button && _have_dealer) tag = " (button)";
    CString fate;
    if (i == _winner_uncontested) fate.Format("collected (%s)", FmtMoney(_final_pot).GetString());
    else if (_folded[i])          fate = "folded";
    else if (!_hole[i].IsEmpty()) fate.Format("showed [%s]", _hole[i].GetString());
    else                          fate = "mucked/—";
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
  if (_hero < 0 || _hero >= _nchairs) return false;
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
  CString s;
  s.Format("Game Hand #%s - Tournament #%s - Holdem (No Limit) - Level 1 (%s/%s) - %s UTC",
           hid.GetString(), tid.GetString(),
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
  if (p_scraper == NULL || p_tablemap == NULL) return;
  // Tournament NAME -> filename TN- field. Strip commas (they break the ACR
  // filename / are unwanted in the name).
  if (_tourney_title.IsEmpty() && RegionExists("c0tourney_title")) {
    CString t;
    if (p_scraper->EvaluateRegion("c0tourney_title", &t)) {
      t.Remove(',');
      t.Trim();
      if (!t.IsEmpty()) _tourney_title = t;
    }
  }
  // Tournament ID -> "Tournament #<id>" header. Strip parentheses.
  if (_tourney_id.IsEmpty() && RegionExists("c0tourney_id")) {
    CString id;
    if (p_scraper->EvaluateRegion("c0tourney_id", &id)) {
      id.Remove('(');
      id.Remove(')');
      id.Trim();
      if (!id.IsEmpty()) _tourney_id = id;
    }
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
  // Strip characters that are illegal in a Windows filename.
  const char *illegal = "\\/:*?\"<>|";
  for (const char *p = illegal; *p; ++p) tn.Remove(*p);
  SYSTEMTIME st; GetSystemTime(&st);
  CString fname;
  fname.Format("HH%04d%02d%02d T%d TN-%s GAMETYPE-Hold'em LIMIT-no CUR-REAL.txt",
               st.wYear, st.wMonth, st.wDay, sid, tn.GetString());
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
