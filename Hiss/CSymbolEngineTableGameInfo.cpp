//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************

#include "stdafx.h"
#include "CSymbolEngineTableGameInfo.h"

#include "CScraper.h"   // g_tgi_* globals
#include "..\CTablemap\CTablemap.h"

// True if the tablemap defines a region with this name (the lobby buttons are added
// to the tablemap region list; the symbol reports whether this site has them mapped).
static bool TmRegionExists(const CString name) {
  if (p_tablemap == NULL) return false;
  return p_tablemap->r$()->find(name.GetString()) != p_tablemap->r$()->end();
}

CSymbolEngineTableGameInfo *p_symbol_engine_table_game_info = NULL;

CSymbolEngineTableGameInfo::CSymbolEngineTableGameInfo() {}
CSymbolEngineTableGameInfo::~CSymbolEngineTableGameInfo() {}

void CSymbolEngineTableGameInfo::InitOnStartup()    {}
void CSymbolEngineTableGameInfo::UpdateOnConnection() {}
void CSymbolEngineTableGameInfo::UpdateOnHandreset() {}
void CSymbolEngineTableGameInfo::UpdateOnNewRound()  {}
void CSymbolEngineTableGameInfo::UpdateOnMyTurn()    {}
void CSymbolEngineTableGameInfo::UpdateOnHeartbeat() {}

bool CSymbolEngineTableGameInfo::EvaluateSymbol(const CString name, double *result, bool log) {
  // Lobby navigation buttons: 1 if the tablemap maps the region, so the OHF / orchestration
  // knows the button is available to navigate to the lobby for tournament info.
  if (name == "goto_lobby_button")       { if (result) *result = TmRegionExists("goto_lobby_button") ? 1.0 : 0.0;       return true; }
  if (name == "leave_lobby_button")      { if (result) *result = TmRegionExists("leave_lobby_button") ? 1.0 : 0.0;      return true; }
  if (name == "return_to_tables_button") { if (result) *result = TmRegionExists("return_to_tables_button") ? 1.0 : 0.0; return true; }
  if (name == "lobby_more_info_button")  { if (result) *result = TmRegionExists("lobby_more_info_button") ? 1.0 : 0.0;  return true; }
  if (name != "table_game_info" && name != "table_game_info_2"
      && name.Left(4) != "tgi_" && name.Left(5) != "tgi2_") return false;
  if (name == "table_game_info")      { if (result) *result = g_tgi_set ? 1.0 : 0.0;     return true; }
  // table_game_info_2: current/previous hand numbers (Claude-parsed).
  if (name == "table_game_info_2")    { if (result) *result = (g_tgi2_handnumber > 0) ? 1.0 : 0.0; return true; }
  if (name == "tgi2_handnumber")      { if (result) *result = g_tgi2_handnumber;         return true; }
  if (name == "tgi2_prev_handnumber") { if (result) *result = g_tgi2_prev_handnumber;    return true; }
  if (name == "tgi_sblind")           { if (result) *result = g_tgi_sblind;              return true; }
  if (name == "tgi_bblind")           { if (result) *result = g_tgi_bblind;              return true; }
  if (name == "tgi_ante")             { if (result) *result = g_tgi_ante;                return true; }
  if (name == "tgi_chips_per_bb")     { if (result) *result = g_tgi_chips_per_bb;        return true; }
  if (name == "tgi_level")            { if (result) *result = g_tgi_level;               return true; }
  if (name == "tgi_players_remaining"){ if (result) *result = g_tgi_players_remaining;   return true; }
  return false;
}

CString CSymbolEngineTableGameInfo::SymbolsProvided() {
  return "table_game_info tgi_sblind tgi_bblind tgi_ante tgi_chips_per_bb "
         "tgi_level tgi_players_remaining "
         "table_game_info_2 tgi2_handnumber tgi2_prev_handnumber "
         "goto_lobby_button leave_lobby_button return_to_tables_button lobby_more_info_button ";
}
