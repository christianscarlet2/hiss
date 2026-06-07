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
#include "CHandHistoryUncontested.h"

#include "CEngineContainer.h"
#include "CHandHistoryWriter.h"

#include "CScraper.h"
#include "CSymbolEngineActiveDealtPlaying.h"
#include "CTableState.h"



CHandHistoryUncontested *p_handhistory_uncontested = NULL;

CHandHistoryUncontested::CHandHistoryUncontested() {
	// The values of some symbol-engines depend on other engines.
	// As the engines get later called in the order of initialization
	// we assure correct ordering by checking if they are initialized.
  assert(p_engine_container->symbol_engine_active_dealt_playing() != NULL);
	_job_done = false;
}

CHandHistoryUncontested::~CHandHistoryUncontested() {
}

void CHandHistoryUncontested::InitOnStartup() {
}

void CHandHistoryUncontested::UpdateOnConnection() {
}

void CHandHistoryUncontested::UpdateOnHandreset() {
  _job_done = false;
}

void CHandHistoryUncontested::UpdateOnNewRound() {
}

void CHandHistoryUncontested::UpdateOnMyTurn() {
}

void CHandHistoryUncontested::UpdateOnHeartbeat() {
  // Neutered: uncontested-win detection now lives in CHandHistoryWriter, which is
  // the single source of truth for hand histories. See CHandHistoryWriter.cpp.
}

bool CHandHistoryUncontested::EvaluateSymbol(const CString name, double *result, bool log /* = false */) {
  // No symbols provided
	return false;
}

CString CHandHistoryUncontested::SymbolsProvided() {
  // No symbols provided
  return " ";
}
	