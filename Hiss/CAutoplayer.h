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

	CCritSec	m_critsec;
};

extern CAutoplayer *p_autoplayer;


#endif //INC_CAUTOPLAYER_H
