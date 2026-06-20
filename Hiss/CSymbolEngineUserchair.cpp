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
#include "CSymbolEngineUserchair.h"

#include "CBetroundCalculator.h"
#include "CCasinoInterface.h"

#include "CScraper.h"
#include "CStringMatch.h"
#include "CTableState.h"


CSymbolEngineUserchair::CSymbolEngineUserchair() {
	// The values of some symbol-engines depend on other engines.
	// As the engines get later called in the order of initialization
	// we assure correct ordering by checking if they are initialized.
	//
	// This engine does not use any other engines.
}

CSymbolEngineUserchair::~CSymbolEngineUserchair()
{}

void CSymbolEngineUserchair::InitOnStartup()
{
	UpdateOnConnection();
}

void CSymbolEngineUserchair::UpdateOnConnection()
{
	_userchair = kUndefined;
}

void CSymbolEngineUserchair::UpdateOnHandreset()
{
}

void CSymbolEngineUserchair::UpdateOnNewRound()
{}

void CSymbolEngineUserchair::UpdateOnMyTurn()
{}

void CSymbolEngineUserchair::UpdateOnHeartbeat() {
	// Reset the confirmed userchair on a TABLE SWITCH so a stale hero seat from a previous (seated)
	// table never carries onto a new or OBSERVED one. g_table_identity (tourney_id|table_name, debounced
	// in CHandHistoryWriter so it changes only on a real switch) is the signal. Without this, after the
	// bot moved to a RAILED/observed table the seat-3 opponent stayed flagged as the hero and the observer
	// gate (CScraper::RefreshObserverState) was suppressed -> p3 shown as hero instead of observer. On a
	// genuine seat CalculateUserChair re-confirms as soon as the hero's own cards are dealt. [Emrald]
	extern CString g_table_identity;
	static CString s_uc_last_identity;
	if (g_table_identity != s_uc_last_identity) {
		s_uc_last_identity = g_table_identity;
		_userchair = kUndefined;
	}
	if (!userchair_confirmed() || (p_casino_interface->IsMyTurn())) {
		CalculateUserChair();
	}
}

bool CSymbolEngineUserchair::IsNotShowdown() {
  int num_buttons_enabled = p_casino_interface->NumberOfVisibleAutoplayerButtons();
  if (num_buttons_enabled >= k_min_buttons_needed_for_my_turn) return true;
  if (p_betround_calculator->betround() < kBetroundRiver) return true;
  return false;
}

void CSymbolEngineUserchair::CalculateUserChair() {
	if (userchair_confirmed() && p_table_state->User()->HasKnownCards()) {
		write_log(Preferences()->debug_symbolengine(),
			"[CSymbolEngineUserchair] CalculateUserChair() Known cards for known chair. Keeping userchair as is\n");
	}	else {
		// Either not confirmed or no known cards when it is my turn
		// Looking for known cards and new chair
		for (int i=0; i<p_tablemap->nchairs(); i++)
		{
			if (p_table_state->Player(i)->HasKnownCards() && IsNotShowdown()) {
				_userchair = i;
				write_log(Preferences()->debug_symbolengine(),
					"[CSymbolEngineUserchair] CalculateUserChair() Setting userchair to %d\n",
					_userchair);
				return;
			}
		}
		write_log(Preferences()->debug_symbolengine(),
			"[CSymbolEngineUserchair] CalculateUserChair() Userchair not found, because no cards found. Keeping as is\n");
	}
}

bool CSymbolEngineUserchair::EvaluateSymbol(const CString name, double *result, bool log /* = false */)
{
  FAST_EXIT_ON_OPENPPL_SYMBOLS(name);
	if (memcmp(name, "userchair", 9)==0 && strlen(name)==9)
	{
		*result = userchair();
		return true;
	}
	// Symbol of a different symbol-engine
	return false;
}

CString CSymbolEngineUserchair::SymbolsProvided() {
  return "userchair ";
}
