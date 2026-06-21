//******************************************************************************
//
// This file is part of the OpenHoldem project (Hiss fork)
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************

#include "stdafx.h"
#include "CSymbolEngineOpponentIntrospection.h"

#include "CBetroundCalculator.h"
#include "CEngineContainer.h"
#include "CHandresetDetector.h"
#include "CScarletBeast.h"
#include "CSymbolEngineIsOmaha.h"
#include "CSymbolEngineRaisers.h"
#include "CTableState.h"
#include "..\CTablemap\CTablemap.h"

CSymbolEngineOpponentIntrospection::CSymbolEngineOpponentIntrospection() {
	// Depends on isomaha + raisers (current gametype + villain chair). They are created earlier.
	assert(p_engine_container->symbol_engine_isomaha() != NULL);
	assert(p_engine_container->symbol_engine_raisers() != NULL);
	_toact_chair = -1;
	_toact_since = 0;
	for (int i = 0; i < kIntroChairs; ++i) { _last_load_tick[i] = 0; _last_latency_ms[i] = -1; }
	UpdateOnConnection();
}

CSymbolEngineOpponentIntrospection::~CSymbolEngineOpponentIntrospection() {}

void CSymbolEngineOpponentIntrospection::InitOnStartup() { UpdateOnConnection(); }

void CSymbolEngineOpponentIntrospection::UpdateOnConnection() {
	for (int i = 0; i < kIntroChairs; ++i) {
		memset(&_profile[i], 0, sizeof(SOppProfile));
		_profile[i].found = false;
		_loaded_key[i] = "";
		_last_load_tick[i] = 0;
		_last_latency_ms[i] = -1;
	}
	_toact_chair = -1;
	_timing_batch.clear();
	_timing_handnumber = "";
}

void CSymbolEngineOpponentIntrospection::UpdateOnHandreset() {
	FlushTiming();        // persist this hand's latency rows for the timing tell, then start fresh
	_toact_chair = -1;
}

void CSymbolEngineOpponentIntrospection::UpdateOnNewRound() {}
void CSymbolEngineOpponentIntrospection::UpdateOnMyTurn() {}

CString CSymbolEngineOpponentIntrospection::CurrentGametype() {
	CSymbolEngineIsOmaha *o = p_engine_container->symbol_engine_isomaha();
	if (o != NULL && o->isomaha()) return o->isplo8() ? CString("plo8") : CString("plo");
	return CString("nlhe");
}

void CSymbolEngineOpponentIntrospection::LoadProfileIfStale(int chair) {
	if (chair < 0 || chair >= kIntroChairs) return;
	if (p_table_state == NULL || p_tablemap_db == NULL) return;
	CString name = p_table_state->Player(chair)->name();
	if (name.IsEmpty()) { _profile[chair].found = false; return; }
	CString gt = CurrentGametype();
	CString key = name + "|" + gt;
	DWORD now = GetTickCount();
	// Reload when the seat's player or gametype changed, or the cache is >8s old. Throttled so we
	// only hit postgres for chairs the formula actually asks about, never every heartbeat for all.
	if (key == _loaded_key[chair] && (now - _last_load_tick[chair]) < 8000) return;
	_loaded_key[chair] = key;
	_last_load_tick[chair] = now;
	p_tablemap_db->GetOpponentProfile(name, gt, &_profile[chair]);
}

void CSymbolEngineOpponentIntrospection::CaptureTiming() {
	// Timing tells need to know WHEN it became an opponent's turn. That is only available on the
	// ScarletBeast server-scrape (ServerToAct); on the ACR phone scrape toact is unknown, so we skip
	// and fall back to the postgres timing tell. Best-effort action classification via raischair.
	if (p_scarlet_beast == NULL || !p_scarlet_beast->ScrapeFromServer()) return;
	long ta = p_scarlet_beast->ServerToAct();   // 1-based seat; <= 0 unknown
	int cur = (ta > 0) ? (int)ta - 1 : -1;
	DWORD now = GetTickCount();
	if (cur == _toact_chair) return;            // same actor still deciding
	// The previous actor just acted -> record their decision latency.
	if (_toact_chair >= 0 && _toact_chair < kIntroChairs && _toact_since != 0 && p_table_state != NULL) {
		int latency = (int)(now - _toact_since);
		int rc = p_engine_container->symbol_engine_raisers()->raischair();
		CString action = (rc == _toact_chair) ? CString("raises") : CString("acts");
		_last_latency_ms[_toact_chair] = latency;
		CString nm = p_table_state->Player(_toact_chair)->name();
		if (!nm.IsEmpty()) {
			SOppTimingRow row;
			row.player = nm;
			row.street = p_betround_calculator->betround();
			row.action = action;
			row.latency_ms = latency;
			row.ts_ms = (long long)now;
			_timing_batch.push_back(row);
			if (_timing_handnumber.IsEmpty() && p_handreset_detector != NULL)
				_timing_handnumber = p_handreset_detector->GetHandNumber();
		}
	}
	_toact_chair = cur;
	_toact_since = now;
}

void CSymbolEngineOpponentIntrospection::FlushTiming() {
	if (!_timing_batch.empty() && p_tablemap_db != NULL) {
		CString hn = _timing_handnumber;
		if (hn.IsEmpty() && p_handreset_detector != NULL) hn = p_handreset_detector->GetHandNumber();
		if (!hn.IsEmpty()) p_tablemap_db->EmitOpponentTiming(CurrentGametype(), hn, _timing_batch);
	}
	_timing_batch.clear();
	_timing_handnumber = "";
}

void CSymbolEngineOpponentIntrospection::UpdateOnHeartbeat() {
	CaptureTiming();
	// Profiles load lazily in EvaluateSymbol (only chairs the formula asks about), throttled by
	// LoadProfileIfStale -- nothing heavy here, so the heartbeat stays sub-ms.
}

double CSymbolEngineOpponentIntrospection::RangeStrength(int chair) {
	// Profile-only PRIOR for the villain's range strength (0 = capped/bluffy .. 1 = strong/nutted).
	// The OHF f$Range_* refines this with the live betting line. Unknown -> -1.
	const SOppProfile &p = _profile[chair];
	if (!p.found) return -1.0;
	double r = 0.5;
	if (p.honest) r += 0.20;
	if (p.sd_strong_rate >= 0) r += (p.sd_strong_rate - 0.5) * 0.30;
	if (p.keeps_firing) r -= 0.15;                       // sticky aggressor -> wider/bluffier when betting
	if (p.aggr_index >= 0) r -= (p.aggr_index - 0.5) * 0.20;
	if (r < 0.0) r = 0.0;
	if (r > 1.0) r = 1.0;
	return r;
}

bool CSymbolEngineOpponentIntrospection::EvaluateSymbol(const CString name, double *result, bool log) {
	FAST_EXIT_ON_OPENPPL_SYMBOLS(name);
	bool is_intro = (memcmp(name, "intro_", 6) == 0);
	bool is_exploit = (memcmp(name, "exploit_", 8) == 0);
	if (!is_intro && !is_exploit) return false;
	CString last = name.Right(1);
	if (last.IsEmpty() || !isdigit((unsigned char)last[0])) return false;
	int chair = atoi(last.GetString());
	if (chair < 0 || chair >= kIntroChairs) return false;
	LoadProfileIfStale(chair);
	const SOppProfile &p = _profile[chair];
	CString base = name.Left(name.GetLength() - 1);
	double r = 0.0;
	if      (base == "intro_known")         r = p.found ? 1.0 : 0.0;
	else if (base == "intro_simN")          r = p.window_hands;
	else if (base == "intro_contfreq")      r = p.cont_freq;
	else if (base == "intro_aggrindex")     r = p.aggr_index;
	else if (base == "intro_foldpress")     r = p.fold_to_pressure;
	else if (base == "intro_sdstrong")      r = p.sd_strong_rate;
	else if (base == "intro_fasttell")      r = p.fastbet_tell;
	else if (base == "intro_profile")       r = p.profile_code;
	else if (base == "intro_rangestrength") r = RangeStrength(chair);
	else if (base == "intro_lastspeed")     r = (_last_latency_ms[chair] < 0) ? -1.0
	                                            : (_last_latency_ms[chair] <= 1500 ? 2.0
	                                            : (_last_latency_ms[chair] <= 4000 ? 1.0 : 0.0));
	else if (base == "exploit_overfold")    r = p.overfold;
	else if (base == "exploit_folds3bet")   r = p.folds_to_3bet;
	else if (base == "exploit_givesup")     r = p.gives_up;
	else if (base == "exploit_keepsfiring") r = p.keeps_firing;
	else if (base == "exploit_neverfolds")  r = p.never_folds;
	else if (base == "exploit_honest")      r = p.honest;
	else if (base == "exploit_fastweak")    r = p.fast_is_weak;
	else if (base == "exploit_faststrong")  r = p.fast_is_strong;
	else return false;
	if (result != NULL) *result = r;
	return true;
}

CString CSymbolEngineOpponentIntrospection::SymbolsProvided() {
	return "intro_known intro_simN intro_contfreq intro_aggrindex intro_foldpress intro_sdstrong "
	       "intro_fasttell intro_profile intro_rangestrength intro_lastspeed "
	       "exploit_overfold exploit_folds3bet exploit_givesup exploit_keepsfiring exploit_neverfolds "
	       "exploit_honest exploit_fastweak exploit_faststrong ";
}
