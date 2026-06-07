//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose:
//
//******************************************************************************

#include "stdafx.h"
#include "CHandHistoryDealPhase.h"

#include "CBetroundCalculator.h"
#include "CEngineContainer.h"
#include "CHandHistoryWriter.h"

#include "CScraper.h"
#include "CSymbolEngineActiveDealtPlaying.h"
#include "CSymbolEngineChipAmounts.h"
#include "CSymbolEngineDealerchair.h"
#include "CSymbolEngineTableLimits.h"
#include "..\CTablemap\CTablemap.h"
#include "CTableState.h"


CHandHistoryDealPhase *p_handhistory_deal_phase = NULL;

CHandHistoryDealPhase::CHandHistoryDealPhase() {
	// The values of some symbol-engines depend on other engines.
	// As the engines get later called in the order of initialization
	// we assure correct ordering by checking if they are initialized.
	assert(p_engine_container->symbol_engine_active_dealt_playing() != NULL);
  assert(p_engine_container->symbol_engine_chip_amounts() != NULL);
  assert(p_engine_container->symbol_engine_dealerchair() != NULL);
  assert(p_engine_container->symbol_engine_tablelimits() != NULL);
  // No dependency to CHandHistoryWriter as this modules
  // does not compute any symbols but collects our data.
}

CHandHistoryDealPhase::~CHandHistoryDealPhase() {
}

void CHandHistoryDealPhase::InitOnStartup() {
}

void CHandHistoryDealPhase::UpdateOnConnection() {
}

void CHandHistoryDealPhase::UpdateOnHandreset() {
  _job_done = false;
}

void CHandHistoryDealPhase::UpdateOnNewRound() {
}

void CHandHistoryDealPhase::UpdateOnMyTurn() {
}

void CHandHistoryDealPhase::UpdateOnHeartbeat() {
  // Neutered: blind/ante detection now lives in CHandHistoryWriter, which is the
  // single source of truth for hand histories. See CHandHistoryWriter.cpp.
}

bool CHandHistoryDealPhase::EvaluateSymbol(const CString name, double *result, bool log /* = false */) {
  // No symbols provided
	return false;
}

CString CHandHistoryDealPhase::SymbolsProvided() {
  // No symbols provided
  return " ";
}
	