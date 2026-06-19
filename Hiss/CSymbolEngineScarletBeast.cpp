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
  // ALL of the work below makes synchronous HTTP requests to
  // poker.scarletbeast.com on the heartbeat thread. When the server isn't the
  // active scrape source (e.g. screen-scraping a phone mirror), those requests
  // just time out (~30s each) and stall the entire bot. So do nothing unless
  // Scarlet Beast server-scrape is actually enabled.
  if (p_scarlet_beast == NULL || !p_scarlet_beast->ScrapeFromServer()) {
    _connected_ok = false;
    return;
  }
  // Each heartbeat, pull the current seat view and flatten it into sb_* symbols...
  RefreshFromServer();
  // ...and keep the per-table Hiss instances in sync with the seated tables.
  p_scarlet_beast->ManageInstances();
  // Auto buy-back-in when we bust out (stack hit zero).
  p_scarlet_beast->AutoRebuyIfBusted();
  // Keep the HUD payload fresh (throttled) so the React table display can render it.
  p_scarlet_beast->RefreshHud();
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
  // sb_beastfavor — the 666 Card Oracle resonance (0..1), pushed via /api/beast by card_oracle.py.
  // Goes STALE to 0 if not refreshed within ~15s, so "superstition mode" auto-disables the instant
  // the oracle stops feeding it (the OHF draw-chase lines gate on this -> 0 means they never fire).
  if (name == "sb_beastfavor") {
    extern double g_beast_favor;
    extern unsigned long g_beast_favor_tick;
    *result = (GetTickCount() - g_beast_favor_tick < 15000) ? g_beast_favor : 0.0;
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
  return "sb_connected sb_table_id sb_pot sb_to_call sb_to_act sb_my_seat sb_beastfavor ";
}
