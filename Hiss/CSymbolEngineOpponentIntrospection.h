//******************************************************************************
//
// This file is part of the OpenHoldem project (Hiss fork)
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Per-opponent INTROSPECTION engine. Surfaces the gametype-separated
//   rolling-100-hand behavioural model (opponent_profile, built by
//   introspect_aggregator.py) as native chair-suffixed intro_* / exploit_*
//   symbols the OHF + NN consume, and captures live action-latency (timing
//   tells) on the ScarletBeast server-scrape path (the only path where
//   whose-turn is known; ACR falls back to the postgres timing tell).
//   See mcp/INTROSPECTION_PLAN.md.
//
//******************************************************************************

#ifndef INC_CSYMBOLENGINEOPPONENTINTROSPECTION_H
#define INC_CSYMBOLENGINEOPPONENTINTROSPECTION_H

#include "CVirtualSymbolEngine.h"
#include "..\CTablemap\CTablemapDB.h"   // SOppProfile, SOppTimingRow
#include <vector>

class CSymbolEngineOpponentIntrospection : public CVirtualSymbolEngine {
 public:
	CSymbolEngineOpponentIntrospection();
	~CSymbolEngineOpponentIntrospection();
 public:
	// Mandatory reset-functions
	void InitOnStartup();
	void UpdateOnConnection();
	void UpdateOnHandreset();
	void UpdateOnNewRound();
	void UpdateOnMyTurn();
	void UpdateOnHeartbeat();
 public:
	bool EvaluateSymbol(const CString name, double *result, bool log = false);
	CString SymbolsProvided();
 private:
	CString CurrentGametype();          // "nlhe" / "plo" / "plo8" from isomaha/isplo8
	void    LoadProfileIfStale(int chair);
	void    CaptureTiming();
	void    FlushTiming();
	double  RangeStrength(int chair);
 private:
	static const int kIntroChairs = 10;
	SOppProfile _profile[kIntroChairs];
	CString     _loaded_key[kIntroChairs];     // "name|gametype" last loaded per chair
	DWORD       _last_load_tick[kIntroChairs];
	// live timing capture (server-scrape path only, where whose-turn is known)
	int         _toact_chair;
	DWORD       _toact_since;
	int         _last_latency_ms[kIntroChairs];
	std::vector<SOppTimingRow> _timing_batch;
	CString     _timing_handnumber;
	// table-level read: how many seated players are donks (fish/station/loose-passive). A donk-heavy
	// freeroll = get in cheap + bet heavy for value to stack up.
	int         _ndonks;
	DWORD       _table_scan_tick;
	void        ScanTableDonks();
};

#endif // INC_CSYMBOLENGINEOPPONENTINTROSPECTION_H
