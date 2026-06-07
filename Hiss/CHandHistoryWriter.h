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
#include "MagicNumbers.h"

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

  // ---- formatting (placeholder-aware) ----
  CString FmtName(int chair);
  CString FmtStack(int chair);
  CString FmtMoney(double v);
  CString FmtHoleCards(int chair);
  CString BoardTokens(int upto_count);   // "Ah Kd 2c" for the first upto_count cards
  bool    AnySeatRegion(const char *suffix);
  bool    RegionExists(const CString &name);
  void    AddLine(const CString &line);
  void    EnsureOutputPath();

 private:
  // ---- output ----
  CString _output_file;          // resolved once (per session)

  // ---- per-hand state ----
  bool    _meta_captured;        // metadata for the current hand recorded
  bool    _hand_dirty;           // something worth writing was recorded
  CString _hand_number;
  int     _nchairs;
  int     _button;
  int     _hero;
  double  _sb, _bb, _ante;
  // Which field types the tablemap actually provides (else -> placeholder).
  bool    _have_names, _have_balance, _have_bet, _have_cards, _have_board, _have_dealer;
  // Snapshot at the start of the hand.
  CString _seat_name[kMaxNumberOfPlayers];
  double  _seat_stack[kMaxNumberOfPlayers];
  bool    _seat_in_hand[kMaxNumberOfPlayers];
  CString _hole[kMaxNumberOfPlayers];     // hero + any shown cards, captured over the hand
  // Action tracking.
  int     _cur_street;                    // last observed betround
  double  _street_bet[kMaxNumberOfPlayers];
  double  _street_max;
  bool    _folded[kMaxNumberOfPlayers];
  bool    _blinds_done;
  CStringArray _lines;                    // body lines (street headers + actions)
  // Board captured as it appears.
  CString _board_flop, _board_turn, _board_river;
  bool    _flop_logged, _turn_logged, _river_logged;
  // Result.
  int     _winner_uncontested;            // chair, or -1
  bool    _showdown_logged;
  double  _final_pot;
};

extern CHandHistoryWriter *p_handhistory_writer;

#endif // INC_CHANDHISTORYWRITER_H
