//****************************************************************************** 
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//****************************************************************************** 
//
// Purpose: first guess of blind values for CTableLimits,
//   which might be overridden by blind-locking.
//
//****************************************************************************** 

#include "stdafx.h"
#include "CBlindGuesser.h"

#include "CBlindLevels.h"
#include "CEngineContainer.h"

#include "CScraper.h"
#include "CSymbolEngineActiveDealtPlaying.h"
#include "CSymbolEngineDealerchair.h"
#include "CSymbolEngineGameType.h"
#include "CSymbolEngineHistory.h"
#include "..\CTablemap\CTablemap.h"
#include "CTableState.h"

CBlindGuesser::CBlindGuesser() {
}

CBlindGuesser::~CBlindGuesser() {
}

// All parameters are out-parameters only. Public entry: guess, then apply the symbol-level OCR correction
// on WHATEVER value any internal path produced (so every early return is covered).
void CBlindGuesser::Guess(double *sblind, double *bblind, double *bbet, double *ante) {
  GuessRaw(sblind, bblind, bbet, ante);
  FixOcrMisreadBlinds(sblind, bblind, bbet);
}

// OCR hack [Emrald]: with sb==0.5 the bb is 1.0, but the BB region sometimes OCR's as "11" or "111"
// (lost/extra decimal). That inflates read depth ~10-100x, so the bot misreads push/fold and folds
// premiums (this is what was folding AK). Rewrite bb to 1.0 at the symbol level -- NOT an OCR retrain.
// Gated on sb==0.5 so it can never touch a legitimate blind level.
void CBlindGuesser::FixOcrMisreadBlinds(double *sblind, double *bblind, double *bbet) {
  if (sblind == NULL || bblind == NULL) return;
  if (fabs(*sblind - 0.5) < 0.01 && (fabs(*bblind - 11.0) < 0.5 || fabs(*bblind - 111.0) < 0.5)) {
    write_log(Preferences()->debug_table_limits(),
      "[CBlindGuesser] OCR fix: sb=0.5 with bb=%.2f -> rewriting bb to 1.0 [Emrald]\n", *bblind);
    *bblind = 1.0;
    if (bbet != NULL && (fabs(*bbet - 11.0) < 0.5 || fabs(*bbet - 111.0) < 0.5 || *bbet <= 0.0)) *bbet = 1.0;
  }
}

void CBlindGuesser::GuessRaw(double *sblind, double *bblind, double *bbet, double *ante) {
  // table_game_info override: if Claude has parsed the blinds from the table image and
  // posted them (/api/table-game-info), use those AUTHORITATIVE values and skip guessing.
  // This is how a big-blind-denominated table (stacks shown in BB) gets the correct unit
  // (bb=1.0) instead of the 0.01/0.02 fallback that made the bot misread depth ~50x.
  if (g_tgi_set && g_tgi_bblind > 0) {
    *sblind = g_tgi_sblind;
    *bblind = g_tgi_bblind;
    *bbet   = g_tgi_bblind;
    *ante   = g_tgi_ante;
    return;
  }
  // No debug-messages needed here. Subroutines log enough.
  GetFirstBlindDataFromScraper(sblind, bblind, bbet, ante);
  // Take what we have and check the blinds on the list of known levels
  // (either complete or partial match)
  if (_blind_levels.BestMatchingBlindLevel(sblind, bblind, bbet)) {
    return;
  }
  if (CompletePartiallyKnownBlinds(sblind, bblind, bbet)) {
    return;
  }
  // Missing input.
  // We have to guess everything from the table
  // And then again try to verify/complete the data.
  GetFirstBlindDataFromBetsAtTheTable(sblind, bblind, bbet, ante);
  if (_blind_levels.BestMatchingBlindLevel(sblind, bblind, bbet)) {
    return;
  }
  if (CompletePartiallyKnownBlinds(sblind, bblind, bbet)) {
    return;
  }
  write_log(Preferences()->debug_table_limits(),
    "[CBlindGuesser] Complete failure\n");
  // A big-blind-denominated table (stacks/bets shown in BB, e.g. ACR mobile) can set the
  // tablemap symbol "bblind_fallback" to 1.0 so depth is read in the correct unit even when
  // the blind level can't be scraped -- WITHOUT needing /api/table-game-info posted each
  // session. Otherwise assume the lowest cash level. This is the fix for the ~50x depth
  // misread that kept the bot out of push/fold mode and folding short-stack jams.
  double tm_fallback_bb = (p_tablemap != NULL)
    ? atof(p_tablemap->GetTMSymbol("bblind_fallback").GetString()) : 0.0;
  if (tm_fallback_bb > 0) {
    write_log(Preferences()->debug_table_limits(),
      "[CBlindGuesser] Using tablemap bblind_fallback = %.4f (BB-denominated table)\n", tm_fallback_bb);
    *bblind = tm_fallback_bb;
    *sblind = tm_fallback_bb / 2.0;
    *bbet   = tm_fallback_bb;
  } else {
    write_log(Preferences()->debug_table_limits(),
      "[CBlindGuesser] Assuming lowest level: 0.01 / 0.02 / 0.04\n");
    *sblind = 0.01;
    *bblind = 0.02;
    *bbet   = 0.04;
  }
}

bool CBlindGuesser::CompletePartiallyKnownBlinds(double *sblind, 
                                                 double *bblind, 
                                                 double *bbet) {
  write_log(Preferences()->debug_table_limits(), 
    "[CBlindGuesser] CompletePartiallyKnownBlinds %.2f / %.2f / %.2f\n",
    *sblind, *bblind, *bbet);
  // Not on the list, potentially still ok.
  // But we have to do some guess-work
  if (SBlindBBlindCombinationReasonable(*sblind, *bblind)
      && SBlindBBetCombinationReasonable(*sblind, *bbet)
      && BBlindBBetCombinationReasonable(*bblind, *bbet)) {
    // This looks VERY reasonable
    // keep everything as is.
  } // else if...
  // Otherwise: take just 2 values to guess the 3rd one,
  // starting with sblind, bblind, which we mostly should know.
  else if (SBlindBBlindCombinationReasonable(*sblind, *bblind)) {
    *bbet =  2 * *bblind;
  } else if (SBlindBBetCombinationReasonable(*sblind, *bbet)) {
    *bblind =  ReasonableLookingHalfBlindValue(*bbet);
  } else if (BBlindBBetCombinationReasonable(*bblind, *bbet)) {
    *sblind = ReasonableLookingHalfBlindValue(*bblind);
  } // else if...
  // Finally: Take 1 value to guess all the rest,
  // starting with bblind, which we usually know
  else if (*bblind >= 0.02) {
    *sblind = ReasonableLookingHalfBlindValue(*bblind);
    *bbet   = 2 * *bblind;
  } else if (*bbet >= 0.04) {
    *bblind = ReasonableLookingHalfBlindValue(*bbet);
    *sblind = ReasonableLookingHalfBlindValue(*bblind);
  } else if (*sblind >= 0.01) {
    *bblind = 2 * *sblind;
    *bbet   = 2 * *bblind;
  } else {
    write_log(Preferences()->debug_table_limits(), 
      "[CBlindGuesser] Can't complete blinds because of bad input\n");
    return false;
  }
  write_log(Preferences()->debug_table_limits(), 
    "[CBlindGuesser] Blinds completed to %.2f / %.2f / %.2f\n",
    *sblind, *bblind, *bbet);
  return true;
}

bool CBlindGuesser::SBlindBBlindCombinationReasonable(double sblind, double bblind) {
  // Reasonable are for example 10/10, 10/15, 10/20, 10&25 and 10&30
  if (sblind <= 0.00) return false;
  if (bblind <= 0.00) return false;
  if (sblind < 0.33 * bblind) return false;
  if (sblind > 1.00 * bblind) return false;
  write_log(Preferences()->debug_table_limits(), 
    "[CBlindGuesser] SBlindBBlindCombinationReasonable(%.2f, %.2f)\n",
    sblind, bblind);
  return true;
}

bool CBlindGuesser::SBlindBBetCombinationReasonable(double sblind, double bbet) {
  if (sblind <= 0.00) return false;
  if (bbet   <= 0.00) return false;
  if (sblind < 0.16 * bbet) return false;
  if (sblind > 0.50 * bbet) return false;
  write_log(Preferences()->debug_table_limits(), 
    "[CBlindGuesser] SBlindBBetCombinationReasonable(%.2f, %.2f)\n",
    sblind, bbet);
  return true;
}

bool CBlindGuesser::BBlindBBetCombinationReasonable(double bblind, double bbet) {
  if (bblind <= 0.00) return false;
  if (bbet   <= 0.00) return false;
  if (bblind < 0.40 * bbet) return false;
  if (bblind > 0.67 * bbet) return false;
  write_log(Preferences()->debug_table_limits(), 
    "[CBlindGuesser] BBlindBBetCombinationReasonable(%.2f, %.2f)\n",
    bblind, bbet);
  return true;
}

// Some beautiful number, usually in the range of 40..50%
double CBlindGuesser::ReasonableLookingHalfBlindValue(double known_value) {
  assert(known_value >= 0.02);
  double half_amount = 0.50 * known_value;
  // Turn it into something reasonable
  // and use smaller or equal, because sometimes "half" is just 40%
  double result = _blind_levels.GetNextSmallerOrEqualBlindOnList(half_amount);
  write_log(Preferences()->debug_table_limits(), 
    "[CBlindGuesser] About half of %.2f is %.2f\n",
    known_value, result);
  return result;
}

// All parameters are out-parameters only
// Guessing according to Want2Learns method
// http://www.maxinmontreal.com/forums/viewtopic.php?f=117&t=17380&start=60
void CBlindGuesser::GetFirstBlindDataFromBetsAtTheTable(double *sblind,
  double *bblind,
  double *bbet,
  double *ante) {
  // Everything is unknown, init to zero
  *sblind = kUndefinedZero;
  *bblind = kUndefinedZero;
  *bbet = kUndefinedZero;
  //*ante = kUndefinedZero;
  // Search first two bets...
  double first_bet_after_dealer = 0.0;
  double second_bet_after_dealer = 0.0;
  bool   first_chair_immediatelly_after_dealer_betting = false;

  // Dealerchair gets only calculated after blind-guessing,
  // therefore blind-duessing will only work after the first heartbeat,
  // but this looks acceptable, because we have to guess only
  // verz few times and don't have to act at the verz first heartbeat
  // (because of stable frames).
  int dealer = p_engine_container->symbol_engine_dealerchair()->dealerchair();
  // Exit on undefined or wrong dealer (outdated, from last hand)
  if ((dealer == kUndefined) || (p_table_state->Player(dealer)->dealer() == false)) {
    return;
  }
  int first_chair = dealer + 1;
  int last_chair = dealer + p_tablemap->nchairs();
  for (int i = first_chair; i <= last_chair; ++i) {
    int normalized_chair = i % p_tablemap->nchairs();
    double players_bet = p_table_state->Player(normalized_chair)->_bet.GetValue();
    if (players_bet <= 0) continue;
    if (first_bet_after_dealer <= 0) {
      // Probably SB found
      first_bet_after_dealer = players_bet;
      // Also check if the verz first player after dealer is betting,
      // which for sure excludes a missing small blind
      if (normalized_chair == (first_chair % p_tablemap->nchairs())) {
        first_chair_immediatelly_after_dealer_betting = true;
      }
    }
    else if (second_bet_after_dealer <= 0) {
      second_bet_after_dealer = players_bet;
      // 2nd blind found. No need to search any further
      break;
    }
  }
  assert(p_engine_container->symbol_engine_active_dealt_playing() != NULL);
  // Checking for reversed blinds.
  // Using 0.61% here to support limits like 0.15/0.25
  if ((second_bet_after_dealer < 0.61 * first_bet_after_dealer) &&
	  (second_bet_after_dealer > 0.0) &&
    p_engine_container->symbol_engine_active_dealt_playing()->nplayersdealt() == 2) {
    // Special handling for reveresed blinds headsup
    // http://www.maxinmontreal.com/forums/viewtopic.php?f=156&t=19102
    write_log(Preferences()->debug_table_limits(),
      "[CBlindGuesser] Game is headsup\n");
    write_log(Preferences()->debug_table_limits(),
      "[CBlindGuesser] Swapping reversed blinds\n");
    // settings sblind is bad idea, because sblind can call/raise
    *bblind = first_bet_after_dealer;
  }
  else if (second_bet_after_dealer < 2 * first_bet_after_dealer) {
	  // sblind is raising/calling.
	  *bblind = second_bet_after_dealer;
  }
  else if (first_chair_immediatelly_after_dealer_betting) {
    // Can't be a missing small-blind.
    // Therefore this is the small blind,
    // except maybe for headsup
    assert(first_bet_after_dealer > 0);
    *sblind = first_bet_after_dealer;
  }
  else if (second_bet_after_dealer == 0.0) {
    // Only one blind, must be big-blind, small-blind missing
    *bblind = first_bet_after_dealer;
	  write_log(Preferences()->debug_table_limits(),
		  "[CBlindGuesser] Only one blind, must be big-blind, small-blind missing\n");
  }
  else if (second_bet_after_dealer > 2.5 * first_bet_after_dealer) {
    // Missing small blind and UTG raising to more than 2 big blinds.
    *bblind = first_bet_after_dealer;
	  write_log(Preferences()->debug_table_limits(),
		  "[CBlindGuesser] Missing small blind and UTG\n");
  }
  else if (second_bet_after_dealer == first_bet_after_dealer) {
    // Either completing small-blind
    // or missing small-blind and limping UTG
    *bblind = first_bet_after_dealer;
	write_log(Preferences()->debug_table_limits(),
		"[CBlindGuesser] Either completing small-blind\n");
  }
  else if (second_bet_after_dealer == 2 * first_bet_after_dealer) {
    // Could be either small-blind/bblind
    // or missing small-blind and min-raising UTG
    // We don't know exactly
    *sblind = first_bet_after_dealer;
  }
  else {
    // Assume the first bet is "normal" and therefore small-blind
    *sblind = first_bet_after_dealer;
  }
  if (p_engine_container->symbol_engine_istournament() && p_table_state->AntesVisible() && *ante <= 0) {
	  if (*sblind == 0) *sblind = *bblind / 2;
	  if (*bblind == 0) *bblind = *sblind * 2;
	  *ante = round(*bblind * 0.125);  // Approximating ante guess to 12.5% of big blind
  }
  write_log(Preferences()->debug_table_limits(), 
    "[CBlindGuesser] Data guessed from bets: %.2f / %.2f / %.2ff / %.2f\n",
    *sblind, *bblind, *bbet, *ante);
  write_log(Preferences()->debug_table_limits(), 
    "[CBlindGuesser] Data needs to be completed\n");
  // Still to do (by the caller):
  // verify and complete these values
}

void CBlindGuesser::GetFirstBlindDataFromScraper(double *sblind, 
                                                 double *bblind, 
                                                 double *bbet,
												 double *ante) {
  // Get values from the scraper (ttlimits / c0limits)
  *sblind = p_table_state->_s_limit_info.sblind();
  *bblind = p_table_state->_s_limit_info.bblind();
  *bbet   = p_table_state->_s_limit_info.bbet();
  *ante	  = p_table_state->_s_limit_info.ante();
  write_log(Preferences()->debug_table_limits(), 
    "[CBlindGuesser] Data from scraper (ttlimits, c0limits): %.2f / %.2f / %.2ff / %.2f\n",
    *sblind, *bblind, *bbet, *ante);
}
