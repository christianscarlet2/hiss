//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Detecting if we play a holdem or omaha,
//   needed e.g. for automatic adaption of PT-queries. 
//
//******************************************************************************

#include "stdafx.h"
#include "CSymbolEngineIsOmaha.h"

#include <assert.h>
#include "CEngineContainer.h"

#include "CScraper.h"
#include "CAutoConnector.h"
#include "..\CTablemap\CTablemap.h"
#include "CTableState.h"

#include "..\DLLs\StringFunctions_DLL\string_functions.h"

// The number of cards per player depends on the game-type.
// This affects cards to be scraped and evaluated.
// The data containers must be large enough to store kMaxNumberOfCardsPerPlayer.
int NumberOfCardsPerPlayer() {
  if (p_engine_container->symbol_engine_isomaha() == NULL) {
    // Not yet initialized. Keep the OpenHoldem default
    return kNumberOfCardsPerPlayerHoldEm;
  }
  if (p_engine_container->symbol_engine_isomaha()->isomaha()) {
    return kNumberOfCardsPerPlayerOmaha;
  }
  return kNumberOfCardsPerPlayerHoldEm;
}

CSymbolEngineIsOmaha::CSymbolEngineIsOmaha() {
	// The values of some symbol-engines depend on other engines.
	// As the engines get later called in the order of initialization
	// we assure correct ordering by checking if they are initialized.
  _isomaha = false;
  _isplo8 = false;
}

CSymbolEngineIsOmaha::~CSymbolEngineIsOmaha()
{}

void CSymbolEngineIsOmaha::InitOnStartup() {
	UpdateOnConnection();
}

void CSymbolEngineIsOmaha::UpdateOnConnection() {
  _isomaha = false;
  _isplo8 = false;
}

// PLO8 (Omaha Hi/Lo, 8-or-better) is indistinguishable from plain Omaha by the
// cards alone, so we read the variant from the attached table-window title. The
// common markers across sites (ACR/WPN, PokerStars, partypoker, etc.):
//   "Hi/Lo", "Hi-Lo", "HiLo", "8 or Better", "8-or-Better", "8/B",
//   "O8", "PLO8", "Omaha/8", "Omaha 8".
bool CSymbolEngineIsOmaha::TitleLooksLikeHiLo() {
  if (p_autoconnector == NULL) return false;
  HWND table = p_autoconnector->attached_hwnd();
  if (table == NULL) return false;
  char raw[MAX_WINDOW_TITLE] = { 0 };
  if (GetWindowText(table, raw, sizeof(raw)) <= 0) return false;
  CString title = CString(raw);
  title.MakeLower();
  // Strip separators so "hi/lo", "hi-lo", "hi lo" all collapse to "hilo".
  CString squeezed = title;
  squeezed.Remove(' ');
  squeezed.Remove('/');
  squeezed.Remove('-');
  squeezed.Remove('_');
  if (squeezed.Find("hilo") >= 0) return true;
  if (squeezed.Find("8orbetter") >= 0) return true;
  if (squeezed.Find("omaha8") >= 0) return true;
  if (squeezed.Find("plo8") >= 0) return true;
  // Bare "o8" / "8b" tokens (kept with separators to avoid matching e.g. "o8" inside a stake string).
  if (title.Find(" o8") >= 0 || title.Find("(o8") >= 0 || title.Find("o8 ") >= 0) return true;
  if (title.Find("8/b") >= 0) return true;
  return false;
}

void CSymbolEngineIsOmaha::UpdateOnHandreset()
{}

void CSymbolEngineIsOmaha::UpdateOnNewRound()
{}

void CSymbolEngineIsOmaha::UpdateOnMyTurn() {
}

void CSymbolEngineIsOmaha::UpdateOnHeartbeat() {
  if (_isomaha) {
    write_log(Preferences()->debug_symbolengine(), "[CSymbolEngineIsOmaha] Already Omaha\n");
    return;
  }
  if (!p_tablemap->SupportsOmaha()) {
    write_log(Preferences()->debug_symbolengine(), "[CSymbolEngineIsOmaha] Omaha not supported by tablemap\n");
    return;
  }
  // Checking the two "additional" cards
  if (p_table_state->User()->hole_cards(2)->IsKnownCard()
    && p_table_state->User()->hole_cards(3)->IsKnownCard()) {
    write_log(Preferences()->debug_symbolengine(), "[CSymbolEngineIsOmaha] Found Omaha hole-cards\n");
    _isomaha = true;
    // Once we know it is Omaha, decide hi-only vs hi/lo split (PLO8) from the title.
    // Latched: like _isomaha we only ever turn this ON within a session (the title is
    // stable for a given table), so a transient empty title can't flip us back to hi-only.
    if (!_isplo8 && TitleLooksLikeHiLo()) {
      write_log(Preferences()->debug_symbolengine(), "[CSymbolEngineIsOmaha] Title indicates Omaha Hi/Lo (PLO8)\n");
      _isplo8 = true;
    }
    return;
  }
  write_log(Preferences()->debug_symbolengine(), "[CSymbolEngineIsOmaha] No indications for Omaha found\n");
}

bool CSymbolEngineIsOmaha::EvaluateSymbol(const CString name, double *result, bool log /* = false */) {
  FAST_EXIT_ON_OPENPPL_SYMBOLS(name);
  if (memcmp(name, "isomaha", 7)==0 && strlen(name)==7)	{
		*result = isomaha();
    return true;
	}
  if (memcmp(name, "isplo8", 6)==0 && strlen(name)==6)	{
    // Omaha Hi/Lo (8-or-better). Implies isomaha == true.
		*result = isplo8();
    return true;
	}
  // Symbol of a different symbol-engine
  return false;
}

CString CSymbolEngineIsOmaha::SymbolsProvided() {
  return "isomaha isplo8 ";
}