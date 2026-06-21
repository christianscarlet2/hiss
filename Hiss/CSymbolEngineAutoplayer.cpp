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
#include "CSymbolEngineAutoplayer.h"

#include "CAutoconnector.h"
#include "CAutoplayerFunctions.h"
#include "CCasinoInterface.h"
#include "CEngineContainer.h"
#include "CFunctionCollection.h"
#include "CIteratorThread.h"

#include "CScraper.h"  
#include "CStableFramesCounter.h"
#include "CSymbolEngineIsOmaha.h"   // isomaha() -- gates the prwin-grace in the clean-read latch
#include "CSymbolengineDebug.h"
#include "CSymbolEngineTime.h"
#include "CSymbolEngineUserchair.h"
#include "CTableState.h"



CSymbolEngineAutoplayer::CSymbolEngineAutoplayer() {
	// The values of some symbol-engines depend on other engines.
	// As the engines get later called in the order of initialization
	// we assure correct ordering by checking if they are initialized.
  assert(p_engine_container->symbol_engine_tablelimits() != NULL);
  assert(p_engine_container->symbol_engine_time() != NULL);
  assert(p_engine_container->symbol_engine_userchair() != NULL);
}

CSymbolEngineAutoplayer::~CSymbolEngineAutoplayer() {
}

void CSymbolEngineAutoplayer::InitOnStartup() {
	_myturnbits    = 0;
	_issittingin   = false;
	_isautopost    = false;
	_isfinalanswer = false;
	_clean_latched_this_turn = false;
	_last_ismyturn = false;
}


void CSymbolEngineAutoplayer::UpdateOnConnection() {
	_myturnbits      = 0;
	_issittingin     = false;
	_isautopost      = false;
	_isfinalanswer   = false;
	_last_myturnbits = 0;
	_clean_latched_this_turn = false;
	_last_ismyturn = false;
}

void CSymbolEngineAutoplayer::UpdateOnHandreset() {
}

void CSymbolEngineAutoplayer::UpdateOnNewRound() {
}

void CSymbolEngineAutoplayer::UpdateOnMyTurn() {
}

void CSymbolEngineAutoplayer::UpdateOnHeartbeat() {
	_last_myturnbits = _myturnbits;
	_myturnbits      = 0;
	_issittingin     = false;
	_isautopost      = false;
	_isfinalanswer   = false;
	CalculateMyTurnBits();
	CalculateSitInState();
	CalculateFinalAnswer();
}

void CSymbolEngineAutoplayer::CalculateMyTurnBits() {
	write_log(Preferences()->debug_symbolengine(), "[CSymbolEngineAutoplayer] myturnbits reset: %i\n", _myturnbits);
	for (int i=0; i<k_max_number_of_buttons; i++) {
		if (p_casino_interface->_technical_autoplayer_buttons[i].IsClickable()) {
      // myturnbits  
      // Since OH 7.7.2 in the form FCKRA 
      // like the butons in the GUI (F =lowest bit) 
      if (p_casino_interface->_technical_autoplayer_buttons[i].IsFold()) {
				_myturnbits |= kMyTurnBitsFold;
			}	else if (p_casino_interface->_technical_autoplayer_buttons[i].IsCall()) {
				_myturnbits |= kMyTurnBitsCall;
			}	else if (p_casino_interface->_technical_autoplayer_buttons[i].IsCheck()) {
				_myturnbits |= kMyTurnBitsCheck;
      }	else if (p_casino_interface->_technical_autoplayer_buttons[i].IsRaise()) {
				_myturnbits |= kMyTurnBitsRaise;
			}	else if (p_casino_interface->_technical_autoplayer_buttons[i].IsAllin()) {
				_myturnbits |= kMyTurnBitsAllin;
			}	else if (p_casino_interface->_technical_autoplayer_buttons[i].IsAutopost()) {
				_isautopost = true;
			}
		}
	}
	write_log(Preferences()->debug_symbolengine(), "[CSymbolEngineAutoplayer] myturnbits now: %i\n", _myturnbits);
}

void CSymbolEngineAutoplayer::CalculateSitInState() {
  for (int i=0; i<k_max_number_of_buttons; ++i) {
    if (p_casino_interface->_technical_autoplayer_buttons[i].IsSitin()) {
	    // Sitin-button found
      // We are sitting in if that button can NOT be clicked
	    _issittingin = !p_casino_interface->_technical_autoplayer_buttons[i].IsClickable();
	    return;
    } else if (p_casino_interface->_technical_autoplayer_buttons[i].IsSitout()) {
	    // Sitout-button found
      // We are sitting in if that button CAN be clicked
	    _issittingin = (p_casino_interface->_technical_autoplayer_buttons[i].IsClickable());
	    return;
    }
  }
}

bool CSymbolEngineAutoplayer::isfinaltable() {
  return p_table_state->_s_limit_info.is_final_table();
}

void CSymbolEngineAutoplayer::CalculateFinalAnswer() {
	// PER-TURN CLEAN-READ LATCH. On a jittery phone-mirror feed the old AND-gate (iterator's prwin
	// running, <2 buttons on a mid-flip frame, the hero hole-card flip unsettled, and the whole-table
	// StableFramesCounter resetting on ANY opponent jitter) rarely all passed on the SAME heartbeat, so
	// the bot saw ismyturn=1 but never committed. Instead we LATCH once a verified-clean frame is seen
	// this turn and stop re-gating on later mid-flip frames. The latch is only ever SET on a clean
	// frame and never re-reads the cards once set, so the bot acts WITHIN the turn without acting on a
	// mis-scrape. [Emrald: "isfinalanswer not latching -> bot doesn't act"]
	bool my_turn = ismyturn();
	if (my_turn && !_last_ismyturn) {
		_clean_latched_this_turn = false;          // rising edge of a new turn -> require a fresh clean read
	} else if (!my_turn) {
		_clean_latched_this_turn = false;
	}
	_last_ismyturn = my_turn;

	// Is THIS heartbeat a verified-clean read: enough buttons, hero has known cards, and the hole-card
	// face-up read is complete + internally consistent (no BACK, no duplicate, and no hero card that
	// also appears on the board -- the "Kd 8d 7d Kd" mid-flip mis-scrape).
	bool clean_this_frame = true;
	if (p_casino_interface->NumberOfVisibleAutoplayerButtons() < k_min_buttons_needed_for_my_turn) {
		write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] not clean: too few buttons (%i)\n",
			p_casino_interface->NumberOfVisibleAutoplayerButtons());
		clean_this_frame = false;
	} else if (!p_table_state->User()->HasKnownCards()) {
		write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] not clean: hero has no known cards\n");
		clean_this_frame = false;
	} else {
		int known = 0, back = 0, vals[kMaxNumberOfCardsPerPlayer]; bool dup = false;
		for (int i = 0; i < kMaxNumberOfCardsPerPlayer; ++i) {
			Card *hc = p_table_state->User()->hole_cards(i);
			if (hc == NULL) continue;
			if (hc->IsKnownCard()) {
				int v = hc->GetValue();
				for (int j = 0; j < known; ++j) if (vals[j] == v) dup = true;
				for (int b = 0; b < kNumberOfCommunityCards; ++b) {        // a hero card also on the board = mis-scrape
					Card *cc = p_table_state->CommonCards(b);
					if (cc != NULL && cc->IsKnownCard() && cc->GetValue() == v) dup = true;
				}
				vals[known++] = v;
			} else if (hc->IsCardBack()) {
				++back;
			}
		}
		if (back > 0 || dup) {
			write_log(Preferences()->debug_autoplayer(),
				"[AutoPlayer] not clean: hero hole cards not a clean face-up read (known=%d back=%d dup=%d)\n",
				known, back, dup ? 1 : 0);
			clean_this_frame = false;
		}
	}

	// DECOUPLE the prwin iterator: a running Monte-Carlo blocks the latch only during a brief grace
	// window; a slow/stuck iterator must never block the turn forever. Omaha doesn't gate on prwin.
	const double kPrwinGraceMs = 1200.0;
	bool prwin_needed = !p_engine_container->symbol_engine_isomaha()->isomaha();
	double milli_seconds_since_my_turn = p_engine_container->symbol_engine_time()->elapsedmyturn() * 1000;
	bool iter_blocking = prwin_needed
		&& (p_iterator_thread != NULL) && p_iterator_thread->IteratorThreadWorking()
		&& (milli_seconds_since_my_turn < kPrwinGraceMs);

	// Latch on the first verified-clean, non-iterator-blocked frame of the turn; hold it for the turn.
	if (my_turn && clean_this_frame && !iter_blocking) {
		_clean_latched_this_turn = true;
	}
	_isfinalanswer = my_turn && _clean_latched_this_turn;

	// Telemetry only (the latch replaced the whole-table stability the phone mirror never meets).
	if (_isfinalanswer) {
		p_stableframescounter->UpdateNumberOfStableFrames();
	}
	write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] stable frames (telemetry): % d  latched=%d\n",
		p_stableframescounter->NumberOfStableFrames(), _clean_latched_this_turn ? 1 : 0);

	// KEEP the f$delay throttle on the COMMIT (not the latch): do not click before f$delay ms into the
	// turn. f$delay is 0 by default here, so this is a no-op then. The StableFramesCounter < frame_delay
	// gate is GONE -- the per-turn latch supersedes it.
	CString delay_function = k_standard_function_names[k_standard_function_delay];
	double desired_delay_in_milli_seconds = p_function_collection->Evaluate(delay_function, Preferences()->log_delay_function());
	p_engine_container->symbol_engine_debug()->SetValue(1, desired_delay_in_milli_seconds);
	p_engine_container->symbol_engine_debug()->SetValue(2, milli_seconds_since_my_turn);
	if (milli_seconds_since_my_turn < desired_delay_in_milli_seconds) {
		write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] holding the commit for f$delay\n");
		_isfinalanswer = false;
	}
  p_engine_container->symbol_engine_debug()->SetValue(3, _isfinalanswer);
}

CString CSymbolEngineAutoplayer::GetFCKRAString()
{
	// Buttons visible (Fold, Call, Check, Raise, Allin)
	CString fckra_seen;
	fckra_seen.Format("%s%s%s%s%s",
    // According to the docu
    // myturnbits  
    // a bit-vector that tells you what buttons are visible 
    // bits 43210 correspond to buttons KARCF 
    // (check alli rais call fold). 
    // Bit 4 (check) was added in OpenHoldem 2.0, 
    // that�s why it is �out of order"
		(_myturnbits & kMyTurnBitsFold  ? "F" : "."),
    (_myturnbits & kMyTurnBitsCall  ? "C" : "."),
		(_myturnbits & kMyTurnBitsCheck ? "K" : "."),
		(_myturnbits & kMyTurnBitsRaise ? "R" : "."),
		(_myturnbits & kMyTurnBitsAllin ? "A" : "."));
	return fckra_seen;
}

bool CSymbolEngineAutoplayer::IsFirstHeartbeatOfMyTurn()
{
	return(ismyturn()
		&& (_last_myturnbits == 0));
}

bool CSymbolEngineAutoplayer::EvaluateSymbol(const CString name, double *result, bool log /* = false */)
{
  FAST_EXIT_ON_OPENPPL_SYMBOLS(name);
	if (memcmp(name, "is", 2)==0)
	{
		if (memcmp(name, "isfinaltable", 12)==0 && strlen(name)==12)	
		{
			*result = isfinaltable();
		}
		else if (memcmp(name, "ismyturn", 8)==0 && strlen(name)==8)		
		{
			*result = ismyturn();
		}
		else if (memcmp(name, "issittingin", 11)==0 && strlen(name)==11)	
		{
			*result = issittingin();
		}
		else if (memcmp(name, "issittingout", 12)==0 && strlen(name)==12)
		{
			*result = issittingout();
		}
		else if (memcmp(name, "isautopost", 10)==0 && strlen(name)==10)	
		{
			*result = isautopost();
		}
		else if (memcmp(name, "isfinalanswer", 13)==0 && strlen(name)==13)	
		{
			*result = isfinalanswer();
		}
		else
		{
			// Invalid symbol
			return false;
		}
		// Valid symbol
		return true;
	}
	else if (memcmp(name, "myturnbits", 10)==0 && strlen(name)==10)
	{
		*result = myturnbits();
		// Valid symbol
		return true;
	}

	// Symbol of a different symbol-engine
	return false;
}

CString CSymbolEngineAutoplayer::SymbolsProvided() {
  return "isfinaltable ismyturn issittingin issittingout isautopost "
    "isfinalanswer myturnbits ";
}