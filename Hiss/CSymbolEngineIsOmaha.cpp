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
        || lsq.Find("plo8") >= 0 || lsq.Find("plos") >= 0) {   // "plos" = ACR's OCR of the PLO8 "8" as "S"
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
  if (squeezed.Find("plos") >= 0) return true;   // ACR OCRs the PLO8 "8" as "S" ("PLOS") [Emrald: PLO8]
  if (squeezed.Find("omahahilo") >= 0 || squeezed.Find("omahahl") >= 0) return true;
  // Bare "o8" / "8b" tokens (kept with separators to avoid matching e.g. "o8" inside a stake string).
  if (title.Find(" o8") >= 0 || title.Find("(o8") >= 0 || title.Find("o8 ") >= 0) return true;
  if (title.Find("8/b") >= 0) return true;
  return false;
}

void CSymbolEngineIsOmaha::UpdateOnHandreset() {
  // CARRY the latched table-level Omaha signal across the hand reset. The autoplayer can evaluate
  // f$preflop on the hand-reset cycle BEFORE UpdateOnHeartbeat re-detects isomaha, so resetting to
  // false here made the PREFLOP decision run the Hold'em tree on PLO/PLO8 (the symbol read 1 between
  // hands but 0 at the decision -> PLO/PLO8 never raised). g_table_is_omaha is the time-latched
  // table-type (CHandHistoryWriter), stable across hands and reverted only on an explicit NLH name, so
  // seeding isomaha from it makes the preflop dispatch correct from the first heartbeat. [Emrald: "I
  // dont see PLO or PLO8 raising at all".]
  _isomaha = g_table_is_omaha;
  _isplo8 = false;   // hi/lo (PLO8) re-detected each hand via TitleLooksLikeHiLo / game-info
  ApplyGametypeOverride();
}

// A human has told us what this table is -> that IS the game type. The strategy tree dispatches on
// isomaha/isplo8 (f$preflop's game-type dispatch), so the override has to land here as well as on
// g_table_is_omaha, or the bot would load the right MAP and still play the wrong TREE.
void CSymbolEngineIsOmaha::ApplyGametypeOverride() {
  if (g_gametype_override == kGametypeOverrideAuto) {
    return;
  }
  bool want_omaha = GametypeOverrideSaysOmaha();
  bool want_plo8  = (g_gametype_override == kGametypeOverridePLO8);
  if (_isomaha != want_omaha || _isplo8 != want_plo8) {
    write_log(k_always_log_basic_information,
      "[Gametype] override=%d -> isomaha %d->%d, isplo8 %d->%d\n",
      g_gametype_override, (int)_isomaha, (int)want_omaha, (int)_isplo8, (int)want_plo8);
  }
  _isomaha = want_omaha;
  _isplo8  = want_plo8;
}

void CSymbolEngineIsOmaha::UpdateOnNewRound()
{}

void CSymbolEngineIsOmaha::UpdateOnMyTurn() {
}

void CSymbolEngineIsOmaha::UpdateOnHeartbeat() {
  // A MANUAL OVERRIDE SHORT-CIRCUITS DETECTION ENTIRELY. Not "detect, then correct" -- the detector
  // must not get a say at all, or a 4-card misread would keep flipping isomaha back underneath us.
  if (g_gametype_override != kGametypeOverrideAuto) {
    ApplyGametypeOverride();
    return;
  }
  // ALWAYS-OMAHA-MAP design: the 4-card map is always loaded (CTableMapLoader), so SupportsOmaha() is
  // always true and can't distinguish the game. The hero's CARD COUNT is the per-hand ground truth:
  // 4 known hole cards => Omaha; only 2 (cards 2,3 read nocard on the 4-card map) => Hold'em. Because the
  // map no longer switches mid-hand, the 3rd/4th cards are scraped from the very first heartbeat, so they
  // are KNOWN by the hero's turn -> isomaha is correct AT PREFLOP -> the Omaha preflop tree runs and PLO
  // RAISES (the old Hold'em-map-at-preflop bug folded 4-card hands as 2-card junk). Latched within the
  // hand (UpdateOnHandreset re-detects each hand) so a reveal-timing flicker can't drop us mid-hand; the
  // sticky g_table_is_omaha cache (CHandHistoryWriter) is an extra confirm for the badge / pot-limit cap.
  // [Emrald: "I dont see PLO or PLO8 raising at all".]
  bool four_known = (p_tablemap != NULL && p_tablemap->SupportsOmaha() && p_table_state != NULL
      && p_table_state->User()->hole_cards(2)->IsKnownCard()
      && p_table_state->User()->hole_cards(3)->IsKnownCard());
  // [Emrald] LINKED TO THE HAND NUMBER: g_table_is_omaha is now resolved per hand number (see
  // CHandHistoryWriter), so it flips within one heartbeat of a table switch and is stable mid-hand.
  // Follow it directly (+ the live 4-card read) rather than carrying our own _isomaha latch -- the old
  // "_isomaha ||" kept the PREVIOUS table's Omaha state alive into the next felt's decision, which is
  // exactly why PLO<->NLH "wasn't switching quick enough" when tabbing tables fast.
  if (four_known || g_table_is_omaha) {
    if (!_isomaha) {
      write_log(Preferences()->debug_symbolengine(),
        "[CSymbolEngineIsOmaha] Omaha (four_known=%d sticky=%d)\n", (int)four_known, (int)g_table_is_omaha);
    }
    _isomaha = true;
    // hi-only vs hi/lo (PLO8) from the title; latched ON like _isomaha.
    if (!_isplo8 && TitleLooksLikeHiLo()) {
      write_log(Preferences()->debug_symbolengine(), "[CSymbolEngineIsOmaha] Title indicates Omaha Hi/Lo (PLO8)\n");
      _isplo8 = true;
    }
    return;
  }
  _isomaha = false;
  _isplo8 = false;
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