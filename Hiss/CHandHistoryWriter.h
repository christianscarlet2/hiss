//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Central hand-history generator.
//
//   A single self-contained recorder (the other CHandHistory* engines are now
//   neutered no-ops). It observes the table every heartbeat, reconstructs a
//   best-effort hand history (blinds, per-street actions, board, result) and
//   writes it to <exedir>\handhistory\hh_<session>.txt at the end of each hand.
//
//   The bot's tablemap may be incomplete, so for every field type that is NOT in
//   the tablemap (names, balances, bets, hole cards, board, dealer) the output
//   substitutes a placeholder ("Seat N", "?", "??") instead of guessing. Action
//   reconstruction from scraping is inherently approximate; the file says so.
//
//******************************************************************************

#ifndef INC_CHANDHISTORYWRITER_H
#define INC_CHANDHISTORYWRITER_H

#include "CVirtualSymbolEngine.h"
#include "..\Shared\MagicNumbers\MagicNumbers.h"

const int kMaxLines = 256;

class CHandHistoryWriter: public CVirtualSymbolEngine {
 public:
	CHandHistoryWriter();
	~CHandHistoryWriter();
 public:
	// Mandatory reset-functions
	void InitOnStartup();
	void UpdateOnConnection();
	void UpdateOnHandreset();
	void UpdateOnNewRound();
	void UpdateOnMyTurn();
	void UpdateOnHeartbeat();
 public:
	// Public accessors
	bool EvaluateSymbol(const CString name, double *result, bool log = false);
  CString SymbolsProvided();
 public:
  // Kept for source-compatibility with the (now neutered) sibling engines.
  void AddMessage(CString message);
  void PostsSmallBlind(int chair);
  void PostsBigBlind(int chair);
  void PostsAnte(int chair);
  void Checks(int chair);
  void Folds(int chair);
  void Calls(int chair);
  void Raises(int chair);
  void WinsUncontested(int chair);

 private:
  // ---- recorder lifecycle ----
  void ResetHand();
  void CaptureMetadata();        // once per hand, when blinds/dealt are known
  void ObserveStreetTransition();
  void ObserveActions();
  void ObserveResult();
  void Flush();                  // write the finished hand to disk

  // ---- tournament identity (scraped once, cached for the session) ----
  void    ScrapeTourneyInfo();         // c0tourney_title / c0tourney_id
  // ---- name handling (avoid scraping the "ANTE"/"posts SB" status overlays) ----
  void    ObserveNames();              // cache real names continuously
  bool    LooksLikeStatus(CString n);  // is this an action/status label, not a name?
  CString ResolveName(int chair);      // best real name, or "Seat N"
  // ---- formatting (placeholder-aware) ----
  CString FmtName(int chair);          // returns a per-chair token, resolved at flush
  CString FmtStack(int chair);
  CString FmtMoney(double v);
  CString FmtHoleCards(int chair);
  CString BoardTokens(int upto_count);   // "Ah Kd 2c" for the first upto_count cards
  CString BoardCard(int idx);            // single card token at board index (0-based)
  CString StreetName(int betround);      // "Pre-Flop" / "Flop" / "Turn" / "River"
  // Evaluate a player's best 5-card hand at showdown: ACR description text +
  // the 5-card bracket + the raw handval (for picking the winner). false if cards unknown.
  bool    DescribeShowdownHand(int chair, CString &desc, CString &bracket, int &handval);
  void    EmitUncalledReturn();          // "Uncalled bet (X) returned to <name>"
  bool    AnySeatRegion(const char *suffix);
  bool    RegionExists(const CString &name);
  void    AddLine(const CString &line);
  void    EnsureOutputPath();
  // ---- ACR-format helpers ----
  int     AcrSeat(int chair) { return chair + 1; }   // ACR seats are 1-based
  CString AcrTimestampUtc();                          // "YYYY/MM/DD HH:MM:SS"
  CString AcrHeader();                                // "Game Hand #... - ..."
  bool    HandLooksComplete();                        // import-quality gate

 private:
  // ---- output ----
  CString _output_complete;      // <exedir>\handhistory\complete\...   (PT4-import folder)
  CString _output_incomplete;    // <exedir>\handhistory\incomplete\... (review folder)
  // Tournament identity, scraped once and kept for the whole session.
  CString _tourney_title;        // c0tourney_title (commas stripped)
  CString _tourney_id;           // c0tourney_id    (parentheses stripped)
  CString _table_name;           // c0table_name    (single-quotes stripped)
  CString _tourney_level;        // c0tourney_level (digits only; re-scraped per hand)

  // ---- per-hand state ----
  bool    _meta_captured;        // metadata for the current hand recorded
  bool    _hand_dirty;           // something worth writing was recorded
  bool    _joined_midhand;       // we did not see the start of this hand
  CString _hand_number;
  int     _nchairs;
  int     _button;
  int     _hero;
  double  _sb, _bb, _ante;
  // Which field types the tablemap actually provides (else -> placeholder).
  bool    _have_names, _have_balance, _have_bet, _have_cards, _have_board, _have_dealer;
  // Best real name seen per seat, persisted across hands (NOT reset each hand) so
  // a transient "ANTE"/"posts SB" overlay never becomes the player's name.
  CString _known_name[kMaxNumberOfPlayers];
  // Snapshot at the start of the hand.
  CString _seat_name[kMaxNumberOfPlayers];
  double  _seat_stack[kMaxNumberOfPlayers];
  bool    _seat_in_hand[kMaxNumberOfPlayers];
  CString _hole[kMaxNumberOfPlayers];     // hero + any shown cards, captured over the hand
  // Action tracking.
  int     _cur_street;                    // last observed betround
  int     _last_active_chair;             // to-act highlight last seen (for check detection)
  int     _sb_chair, _bb_chair;           // who posted the blinds (for SUMMARY positions)
  int     _fold_street[kMaxNumberOfPlayers]; // betround a player folded on (-1 = didn't)
  bool    _voluntary_bet[kMaxNumberOfPlayers]; // put chips in voluntarily (not just a blind)
  double  _street_bet[kMaxNumberOfPlayers];
  double  _street_max;
  bool    _folded[kMaxNumberOfPlayers];
  bool    _blinds_done;
  CString _body;                          // chronological body (blinds + street headers + actions + result)
  // Board captured as it appears.
  CString _board_flop, _board_turn, _board_river;
  bool    _flop_logged, _turn_logged, _river_logged;
  // Result.
  int     _winner_uncontested;            // chair, or -1
  bool    _showdown_logged;
  bool    _uncalled_emitted;              // "Uncalled bet returned" already written this hand
  int     _showdown_winner;               // chair that won at showdown, or -1
  CString _hand_desc[kMaxNumberOfPlayers];    // ACR hand description per shown player
  CString _hand_bracket[kMaxNumberOfPlayers]; // best-5 bracket per shown player
  double  _final_pot;
};

extern CHandHistoryWriter *p_handhistory_writer;

#endif // INC_CHANDHISTORYWRITER_H
