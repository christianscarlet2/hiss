//******************************************************************************
// Scarlet Beast symbol engine.
// Pulls live table state from poker.scarletbeast.com (via CScarletBeast — REST
// seat view + GraphQL) once per heartbeat when "scrape from server" is enabled,
// and exposes it as sb_* symbols the formula engine can read directly. This is
// the API-native alternative to screen-scraping the tablemap.
//******************************************************************************

#ifndef INC_CSYMBOLENGINESCARLETBEAST_H
#define INC_CSYMBOLENGINESCARLETBEAST_H

#include "CVirtualSymbolEngine.h"
#include <map>
#include <string>

class CSymbolEngineScarletBeast : public CVirtualSymbolEngine {
 public:
  CSymbolEngineScarletBeast();
  ~CSymbolEngineScarletBeast();
 public:
  // Mandatory reset-functions
  void InitOnStartup();
  void UpdateOnConnection();
  void UpdateOnHandreset();
  void UpdateOnNewRound();
  void UpdateOnMyTurn();
  void UpdateOnHeartbeat();
 public:
  void UpdateOnAutoPlayerAction();
 public:
  bool EvaluateSymbol(const CString name, double *result, bool log = false);
  CString SymbolsProvided();
 private:
  void RefreshFromServer();
  std::map<std::string, std::string> _symbols;
  bool _connected_ok;
};

extern CSymbolEngineScarletBeast *p_symbol_engine_scarlet_beast;

#endif  // INC_CSYMBOLENGINESCARLETBEAST_H
