//******************************************************************************
// Scarlet Beast symbol engine — implementation.
//******************************************************************************

#include "stdafx.h"
#include "CSymbolEngineScarletBeast.h"

#include "CScarletBeast.h"
#include "CParseErrors.h"
#include <cstdlib>

CSymbolEngineScarletBeast *p_symbol_engine_scarlet_beast = NULL;

CSymbolEngineScarletBeast::CSymbolEngineScarletBeast() {
  // No dependencies on other engines: it sources its data from the network.
  _connected_ok = false;
}

CSymbolEngineScarletBeast::~CSymbolEngineScarletBeast() {}

void CSymbolEngineScarletBeast::InitOnStartup() {}
void CSymbolEngineScarletBeast::UpdateOnConnection() { _symbols.clear(); _connected_ok = false; }
void CSymbolEngineScarletBeast::UpdateOnHandreset() {}
void CSymbolEngineScarletBeast::UpdateOnNewRound() {}
void CSymbolEngineScarletBeast::UpdateOnMyTurn() { RefreshFromServer(); }

void CSymbolEngineScarletBeast::UpdateOnHeartbeat() {
  // Each heartbeat, if the operator chose the server as the scrape source,
  // pull the current seat view and flatten it into sb_* symbols...
  RefreshFromServer();
  // ...and keep the per-table Hiss instances in sync with the seated tables.
  if (p_scarlet_beast != NULL) {
    p_scarlet_beast->ManageInstances();
  }
}

void CSymbolEngineScarletBeast::UpdateOnAutoPlayerAction() {}

void CSymbolEngineScarletBeast::RefreshFromServer() {
  if (p_scarlet_beast == NULL || !p_scarlet_beast->ScrapeFromServer()) {
    _connected_ok = false;
    return;
  }
  std::map<std::string, std::string> fresh;
  _connected_ok = p_scarlet_beast->PopulateSymbols(p_scarlet_beast->TableId(), fresh);
  if (_connected_ok) {
    _symbols = fresh;
  }
}

bool CSymbolEngineScarletBeast::EvaluateSymbol(const CString name, double *result, bool log) {
  if (memcmp(name, "sb_", 3) != 0) {
    return false;  // not ours
  }
  // sb_connected — 1 when the last server pull succeeded.
  if (name == "sb_connected") {
    *result = _connected_ok ? 1 : 0;
    return true;
  }
  std::string key = (LPCSTR)name;
  std::map<std::string, std::string>::const_iterator it = _symbols.find(key);
  if (it == _symbols.end()) {
    *result = 0;
    return true;  // a recognised sb_* symbol with no value yet -> 0
  }
  // Numeric sb_* symbols (pot, to_call, to_act, my_seat) parse cleanly; the
  // string fields (board/hole/street) evaluate to 0 here but are available to
  // the rest of the engine via p_scarlet_beast for advanced use.
  *result = atof(it->second.c_str());
  return true;
}

CString CSymbolEngineScarletBeast::SymbolsProvided() {
  return "sb_connected sb_table_id sb_pot sb_to_call sb_to_act sb_my_seat ";
}
