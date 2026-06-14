//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************

#include "stdafx.h"
#include "CSymbolEngineTilt.h"

#include "CEngineContainer.h"
#include "CSymbolEngineRaisers.h"
#include "CSymbolEngineUserchair.h"
#include "CTableState.h"
#include "..\CTablemap\CTablemap.h"

CSymbolEngineTilt *p_symbol_engine_tilt = NULL;

CSymbolEngineTilt::CSymbolEngineTilt() {
  _tilt_threshold = 0.33;   // lost a third of the recent peak stack -> tilt-risk
  UpdateOnConnection();
}

CSymbolEngineTilt::~CSymbolEngineTilt() {}

void CSymbolEngineTilt::InitOnStartup() { UpdateOnConnection(); }

void CSymbolEngineTilt::UpdateOnConnection() {
  for (int i = 0; i < kMaxNumberOfPlayers; ++i) {
    _count[i] = 0;
    _idx[i] = 0;
    for (int j = 0; j < kRing; ++j) _stack_hist[i][j] = 0.0;
  }
}

void CSymbolEngineTilt::UpdateOnHandreset() {
  // Record each seated chair's stack once per hand (the natural sample point).
  if (p_tablemap == NULL || p_table_state == NULL) return;
  int n = p_tablemap->nchairs();
  if (n <= 0 || n > kMaxNumberOfPlayers) n = kMaxNumberOfPlayers;
  for (int i = 0; i < n; ++i) {
    double s = p_table_state->Player(i)->stack();
    if (s <= 0) continue;                 // empty / unscraped seat
    _stack_hist[i][_idx[i]] = s;
    _idx[i] = (_idx[i] + 1) % kRing;
    if (_count[i] < kRing) ++_count[i];
  }
}

void CSymbolEngineTilt::UpdateOnNewRound() {}
void CSymbolEngineTilt::UpdateOnMyTurn() {}
void CSymbolEngineTilt::UpdateOnHeartbeat() {}

double CSymbolEngineTilt::Drawdown(int chair) {
  if (chair < 0 || chair >= kMaxNumberOfPlayers) return 0.0;
  if (_count[chair] < 3) return 0.0;       // need a few hands of history
  if (p_table_state == NULL) return 0.0;
  double peak = 0.0;
  for (int j = 0; j < _count[chair]; ++j) if (_stack_hist[chair][j] > peak) peak = _stack_hist[chair][j];
  double cur = p_table_state->Player(chair)->stack();
  if (peak <= 0.0 || cur >= peak) return 0.0;
  return (peak - cur) / peak;
}

bool CSymbolEngineTilt::EvaluateSymbol(const CString name, double *result, bool log) {
  if (name.Left(5) != "hero_" && name.Left(7) != "raiser_") return false;

  int userchair = (p_engine_container != NULL && p_engine_container->symbol_engine_userchair() != NULL)
                  ? p_engine_container->symbol_engine_userchair()->userchair() : kUndefined;
  int raischair = (p_engine_container != NULL && p_engine_container->symbol_engine_raisers() != NULL)
                  ? p_engine_container->symbol_engine_raisers()->raischair() : kUndefined;

  if (name == "hero_drawdown") {
    if (result) *result = Drawdown(userchair);
    return true;
  }
  if (name == "hero_tilting") {
    if (result) *result = (Drawdown(userchair) >= _tilt_threshold) ? 1.0 : 0.0;
    return true;
  }
  if (name == "raiser_drawdown") {
    if (result) *result = Drawdown(raischair);
    return true;
  }
  if (name == "raiser_recent_bigloss") {
    if (result) *result = (Drawdown(raischair) >= _tilt_threshold) ? 1.0 : 0.0;
    return true;
  }
  if (name == "raiser_maybe_tilting") {
    // The raiser took a big recent loss and is firing into this pot now.
    bool t = (raischair >= 0) && (Drawdown(raischair) >= _tilt_threshold);
    if (result) *result = t ? 1.0 : 0.0;
    return true;
  }
  return false;   // a symbol for a different engine
}

CString CSymbolEngineTilt::SymbolsProvided() {
  return "hero_drawdown hero_tilting raiser_drawdown raiser_recent_bigloss raiser_maybe_tilting ";
}
