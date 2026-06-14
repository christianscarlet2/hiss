//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Exposes the Claude/MCP-parsed "table_game_info" to the OHF.
//
//   Claude reads the live table image (a heartbeat frame), determines the real
//   blinds / ante / level / chips-per-big-blind / players-remaining, and POSTs
//   them to /api/table-game-info. Those values are stored in the g_tgi_* globals
//   and (a) drive the blind guesser authoritatively, (b) are surfaced here as
//   symbols the strategy / logging can read:
//
//     table_game_info        1 if Claude has populated the info, else 0
//     tgi_sblind             operating small blind (scraped unit)
//     tgi_bblind             operating big blind (scraped unit; BB-display -> 1.0)
//     tgi_ante               ante in the operating unit
//     tgi_chips_per_bb       real chips per big blind (e.g. 400) -- the true level
//     tgi_level              tournament level number
//     tgi_players_remaining  players left in the tournament
//
//******************************************************************************

#ifndef INC_CSYMBOLENGINETABLEGAMEINFO_H
#define INC_CSYMBOLENGINETABLEGAMEINFO_H

#include "CVirtualSymbolEngine.h"

class CSymbolEngineTableGameInfo : public CVirtualSymbolEngine {
 public:
  CSymbolEngineTableGameInfo();
  ~CSymbolEngineTableGameInfo();
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

extern CSymbolEngineTableGameInfo *p_symbol_engine_table_game_info;

#endif  // INC_CSYMBOLENGINETABLEGAMEINFO_H
