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

#ifndef INC_CAUTOPLAYER_H
#define INC_CAUTOPLAYER_H


#include "MainFrm.h"
#include "../CTablemap/CTablemap.h"
#include "../CTablemap/CTableMapAccess.h"
#include "OpenHoldem.h"

class CAutoplayer 
{
public:
	// public functions
	CAutoplayer();
	~CAutoplayer();
public:
	void EngageAutoPlayerUponConnectionIfNeeded();
	void DoAutoplayer();
	// Fast Sit-In: re-seat ASAP. PUBLIC so the heartbeat can fire it under the NN driver too (the NN
	// driver POSTs actions but never clicks the Sit-In button, so a sat-out bot would blind off).
	// Self-guards: only clicks when the Sit-In button is actually present + on a short cooldown.
	bool HandleSitinFast();
	// Cache the FCKRA / TIOLP clickable-button indicators. PUBLIC + called from the heartbeat EVERY
	// beat, because this used to live inside DoAutoplayer() -- which never runs when the autoplayer
	// is disengaged. Under the NN driver that left fckra permanently EMPTY, so nothing could tell
	// which buttons actually exist: the NN's "check" was clicked blind and landed on Raise Options
	// (the phone stacks check/call/raise on the same rect and separates them by LABEL only).
	void CacheButtonIndicators();

public:
	// public accessors
	const bool autoplayer_engaged() { return _autoplayer_engaged; }
	bool TimeToHandleSecondaryFormulas();

public:
	// public mutators
	void EngageAutoplayer(bool to_be_enabled_or_not);
	// Concise OpenPPL decision trace -> the Terminal's Decisions pane + the RED decision overlay. PUBLIC
	// so the heartbeat can publish the would-be decision even when the autoplayer is disabled. [Emrald]
	void EmitDecisionTrace();

private:
	// private functions and variables - not available via accessors or mutators
	void DoRebuyIfNeccessary();
	// Verbose per-cadence dump of button detection + decision state to
	// logs\button_debug.log (diagnoses why an action isn't clicking).
	void DumpButtonDebug();
	bool ExecutePrimaryFormulasIfNecessary();
	// Scarlet Beast server-scrape: decide via the f$-formulas and POST the action to
	// poker.scarletbeast.com (/act) instead of clicking screen buttons.
	void DoAutoplayerServer();
	bool ExecuteSecondaryFormulasIfNecessary();
	bool ExecuteRaiseCallCheckFold();
	// Write a hiss_log_decisions row for an action taken OUTSIDE the elementary-click loop
	// (all-ins, keypad bet/raises, desperation shoves) -- those paths logged nothing.
	void LogClickedDecision(int action_code, double amount = 0.0);
	// Two-successive-clicks bet/raise (phone keypad), run from the primary-formula
	// path so it is gated by ismyturn + isfinalanswer like the other actions.
	// forced_bb > 0 bypasses the strategy's own sizing and types exactly that many big blinds.
	// Used by the sub-2BB desperation rule, which must not be able to be talked out of shoving.
	bool HandleTwoSuccessiveClicksBetRaise(double forced_bb = 0.0);
	// "If my bb is under 2, automatically bet 3 or call anything." Overrides the strategy tree.
	bool ExecuteDesperationShoveOrCall();
	bool ExecuteBeep();
	bool AnyPrimaryFormulaTrue();
	bool AnySecondaryFormulaTrue();
	bool DoAllin();
	bool DoBetPot();
	bool HandleInterfacebuttonsI86();
	void PrepareActionSequence();
	void FinishActionSequenceIfNecessary();
	bool DoBetsize();
	bool DoPrefold();
	bool DoChat();

private:
	// private variables - use public accessors and public mutators to address these
	bool	_autoplayer_engaged;

private:
	POINT	cursor_position;
	HWND	window_with_focus;
	bool	action_sequence_needs_to_be_finished;
	bool	_already_executing_allin_adjustment;
	// Last server table-state version we acted on (act exactly once per state).
	long	_last_server_act_version;
	// Dedup for the Decisions-window trace (only emit when the decision changes).
	CString	_last_decision_line;
	// FCKRA once-per-turn latch: we take at most ONE primary action (fold / check /
	// call / raise / allin) per turn. Set when a primary action executes; cleared
	// when a fresh turn begins (ismyturn rising edge).
	bool	_acted_this_turn;
	bool	_was_my_turn;
	// Click-miss retry: stamp the heartbeat when we take a primary action. If it's STILL
	// our turn (buttons still present) several heartbeats later, the click didn't register
	// -- clear the latch to re-click. _retry_count caps retries per turn (reset each turn).
	int		_acted_heartbeat;
	int		_retry_count;

	CCritSec	m_critsec;
};

extern CAutoplayer *p_autoplayer;


#endif //INC_CAUTOPLAYER_H
