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
  // 1) Claude-posted game-info (set_table_game_info -> g_tgi_gametype) is the authoritative source on
  //    these phone tables, where the "Omaha H/L" / "PLO8" text is in the TABLE image, not the window
  //    title. It is a clean string (not garbled OCR), so the short "hl" (from "H/L") marker is safe.
  if (!g_tgi_gametype.IsEmpty()) {
    CString g = g_tgi_gametype; g.MakeLower();
    CString sq = g; sq.Remove(' '); sq.Remove('/'); sq.Remove('-'); sq.Remove('_');
    if (sq.Find("hilo") >= 0 || sq.Find("hl") >= 0 || sq.Find("8orbetter") >= 0
        || sq.Find("omaha8") >= 0 || sq.Find("plo8") >= 0 || sq.Find("o8") >= 0
        || sq.Find("8b") >= 0) {
      return true;
    }
  }
  // 1b) LIVE table-info OCR (read the game type live) -- the "Omaha H/L" / "PLO8" text is in the table
  //     image, not the window title, and survives a table switch even with a stale posted gametype.
  //     Use only the DISTINCTIVE hi/lo markers here (the live OCR is arbitrary, so avoid the bare
  //     "hl"/"o8"/"8b" short tokens that are only safe on the clean posted gametype above).
  if (p_scraper != NULL) {
    CString a, b, cc;
    p_scraper->EvaluateRegion("c0table_name", &a);
    p_scraper->EvaluateRegion("c0tourney_title", &b);
    p_scraper->EvaluateRegion("c0tourney_id", &cc);
    CString live = a + " " + b + " " + cc; live.MakeLower();
    if (live.Find("h/l") >= 0) return true;
    CString lsq = live; lsq.Remove(' '); lsq.Remove('/'); lsq.Remove('-'); lsq.Remove('_');
    if (lsq.Find("hilo") >= 0 || lsq.Find("8orbetter") >= 0 || lsq.Find("omaha8") >= 0
        || lsq.Find("plo8") >= 0) {
      return true;
    }
  }
  // 2) Attached window title (fallback).
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

void CSymbolEngineIsOmaha::UpdateOnHandreset() {
  // Re-detect the game-type EACH HAND so the OHF strategy dispatch (Hold'em / PLO / PLO8 trees) keeps
  // up as Emrald rotates between games (and busts/rejoins different tables). A per-session latch kept
  // using a prior table's Omaha/PLO8 strategy on the next table (e.g. stayed PLO8 after busting PLO8
  // and sitting at a PLO table). isomaha/isplo8 still latch ON *within* the hand (re-set below once the
  // cards + game-info confirm), so a flickered mid-hand scrape can't flip the tree. Card SCRAPING is
  // unaffected -- that follows the tablemap's SupportsOmaha(), not these symbols.
  _isomaha = false;
  _isplo8 = false;
}

void CSymbolEngineIsOmaha::UpdateOnNewRound()
{}

void CSymbolEngineIsOmaha::UpdateOnMyTurn() {
}

void CSymbolEngineIsOmaha::UpdateOnHeartbeat() {
  // GROUND TRUTH FIRST: if we can actually SEE the hero's 3rd AND 4th hole cards, this IS Omaha --
  // regardless of the (noisy, brand-named) live game-type text. PennyHoot's mixed-game tourneys brand
  // tables "PLO"/"Holdem"/"Omaha" inconsistently, so the name-based g_table_is_omaha was FLICKERING
  // false on real PLO tables -> the Hold'em preflop tree then ran on a 4-card hand, read only cards
  // 0-1, and FOLDED premium holdings (e.g. AA = Ah3cAs7h folded as "A3o"). The loaded Omaha tablemap
  // only yields four KNOWN hole cards on an actual Omaha table (a Hold'em hand scrapes nocard for cards
  // 2-3 even with the Omaha map), so "four known" is a safe, name-independent Omaha signal. The OHF
  // hole-card validity gate (CalculateFinalAnswer) still blocks acting on misread/duplicate cards.
  // [Emrald: "PLO is folding every hand".]
  bool four_known = (p_tablemap != NULL) && p_tablemap->SupportsOmaha()
    && p_table_state->User()->hole_cards(2)->IsKnownCard()
    && p_table_state->User()->hole_cards(3)->IsKnownCard();
  if (four_known) {
    if (!_isomaha) {
      write_log(Preferences()->debug_symbolengine(), "[CSymbolEngineIsOmaha] Found Omaha hole-cards (4 known)\n");
    }
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
  // No 4th hole card visible -> fall back to the live name-based read for the QUICK REVERT to Hold'em.
  // When the phone moves to an NLH table the (lingering) Omaha map yields only 2 known cards, so this
  // branch flips isomaha/isplo8 OFF promptly. [Emrald: NLH OHF/badge must load quickly when leaving Omaha.]
  if (!g_table_is_omaha) {
    _isomaha = false;
    _isplo8 = false;
    return;
  }
  if (_isomaha) {
    write_log(Preferences()->debug_symbolengine(), "[CSymbolEngineIsOmaha] Already Omaha\n");
    return;
  }
  if (!p_tablemap->SupportsOmaha()) {
    write_log(Preferences()->debug_symbolengine(), "[CSymbolEngineIsOmaha] Omaha not supported by tablemap\n");
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