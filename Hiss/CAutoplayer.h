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

public:
	// public accessors
	const bool autoplayer_engaged() { return _autoplayer_engaged; }
	bool TimeToHandleSecondaryFormulas();

public:
	// public mutators
	void EngageAutoplayer(bool to_be_enabled_or_not);

private:
	// private functions and variables - not available via accessors or mutators
	void DoRebuyIfNeccessary();
	// Verbose per-cadence dump of button detection + decision state to
	// logs\button_debug.log (diagnoses why an action isn't clicking).
	void DumpButtonDebug();
	// Concise OpenPPL decision trace into the Terminal's Decisions window
	// (betround + chosen action + size), one line per decision change.
	void EmitDecisionTrace();
	bool ExecutePrimaryFormulasIfNecessary();
	// Scarlet Beast server-scrape: decide via the f$-formulas and POST the action to
	// poker.scarletbeast.com (/act) instead of clicking screen buttons.
	void DoAutoplayerServer();
	bool ExecuteSecondaryFormulasIfNecessary();
	bool ExecuteRaiseCallCheckFold();
	// Two-successive-clicks bet/raise (phone keypad), run from the primary-formula
	// path so it is gated by ismyturn + isfinalanswer like the other actions.
	bool HandleTwoSuccessiveClicksBetRaise();
	bool ExecuteBeep();
	bool AnyPrimaryFormulaTrue();
	bool AnySecondaryFormulaTrue();
	bool DoAllin();
	bool DoBetPot();
	bool HandleInterfacebuttonsI86();
	// Fast Sit-In: re-seat ASAP. Clicks the Sit-In button EVERY heartbeat (not on the slow
	// ~3s secondary-formula cadence) with a short cooldown so it can't double-click.
	bool HandleSitinFast();
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
