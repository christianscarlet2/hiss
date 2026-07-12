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

#include "StdAfx.h"
#include "CAutoplayer.h"
#include "CursorRestore.h"

#include <complex>
#include "AllinAdjustment.h"
#include "BetpotCalculations.h"
#include "BringKeyboard.h"
#include "CAutoplayerTrace.h"
#include "CAutoconnector.h"
#include "CAutoplayerFunctions.h"
#include "CCasinoInterface.h"
#include "CEngineContainer.h"
#include "CFlagsToolbar.h"
#include "CFunctionCollection.h"
#include "CHeartbeatThread.h"
#include "CIteratorThread.h"
#include "CBetroundCalculator.h"
#include "ChatTerminalWindow.h"

#include "CRebuyManagement.h"
#include "CReplayFrame.h"
#include "CScarletBeast.h"
#include "CTwoSuccessiveClicks.h"
#include "CScraper.h"
#include "CSymbolEngineTableLimits.h"
#include <string>
#include "CStableFramesCounter.h"
#include "CSymbolEngineAutoplayer.h"
#include "CSymbolEngineCasino.h"
#include "CSymbolEngineChipAmounts.h"
#include "CSymbolEngineHistory.h"
#include "CSymbolEngineUserchair.h"
#include "CSymbolEnginePrwin.h"
#include "CTableState.h"
#include "CHandresetDetector.h"
#include "CLogWriter.h"
#include "MainFrm.h"
#include "OpenHoldem.h"
#include "PokerChat.hpp"
#include "..\DLLs\StringFunctions_DLL\string_functions.h"
#include "..\DLLs\Files_DLL\Files.h"
#include "CMyMutex.h"

CAutoplayer	*p_autoplayer = NULL;

// Always-on breadcrumb into logs\button_debug.log so we can see exactly where
// the autoplayer decision path stops before a click ("isfinalanswer is true but
// nothing happens"). Pairs with the [click] lines emitted by CAutoplayerButton.
static void APTrace(const char *fmt, ...) {
  if (!Preferences()->debug_autoplayer()) return;
  CString path = LogsDirectory() + "button_debug.log";
  FILE *f = fopen(path.GetString(), "a");
  if (f == NULL) return;
  fprintf(f, "  [path] ");
  va_list args;
  va_start(args, fmt);
  vfprintf(f, fmt, args);
  va_end(args);
  fprintf(f, "\n");
  fclose(f);
}

CAutoplayer::CAutoplayer(void) {
	// Autoplayer is not enabled at startup.
	// We can't call EngageAutoplayer() here,
	// because the toolbar does not yet exist,
	// so we can't set the autoplayer-button.
	// However the toolbar is guaranteed to initialize correctly later.
	_autoplayer_engaged = false;
	action_sequence_needs_to_be_finished = false;
  _already_executing_allin_adjustment = false;
  _last_server_act_version = 0;
  _acted_this_turn = false;
  _was_my_turn = false;
  _acted_heartbeat = 0;
  _retry_count = 0;
}


CAutoplayer::~CAutoplayer(void) {
	FinishActionSequenceIfNecessary();
}

void CAutoplayer::EngageAutoPlayerUponConnectionIfNeeded() {
  write_log(Preferences()->debug_alltherest(), "[CAutoplayer] location Johnny_5\n");
  if (p_autoconnector->IsConnectedToAnything() && Preferences()->engage_autoplayer()) {
		EngageAutoplayer(true);
	}
}

void CAutoplayer::PrepareActionSequence() {
	// This function should be called at the beginning of 
	// ExecutePrimaryFunctions and ExecuteSecondaryFunctions
	// which both will start exactly one action-sequence.
	//
	// At the end of an action sequence FinishAction() has to be called
	// to restore the mouse-position.
	//
	// Getting the cursor position has to be done AFTER  we got the mutex,
	// otherwise it could happen that other applications move the mouse
	// while we wait, leading to funny jumps when we "clean up".
	// http://www.maxinmontreal.com/forums/viewtopic.php?f=111&t=15324
	GetCursorPos(&cursor_position);
	window_with_focus = GetFocus();
	// We got the mutex and everything is prepared.
	// We now assume an action-sequence will be executed.
	// This makes cleanup simpler, as we now can handle it once,
	// instead of everywhere where an action can happen.
	action_sequence_needs_to_be_finished = true;
}

void CAutoplayer::FinishActionSequenceIfNecessary() {
	if (action_sequence_needs_to_be_finished) {
    if (p_engine_container->symbol_engine_casino()->ConnectedToOHReplay() && Preferences()->use_auto_replay()) {
      // Needs to be done very early
      // before we restore the focus
      p_casino_interface->PressTabToSwitchOHReplayToNextFrame();
    }
    // avoid multiple-clicks within a short frame of time
    p_stableframescounter->UpdateOnAutoplayerAction();
    if (p_engine_container->symbol_engine_casino()->ConnectedToOfflineSimulation() || Preferences()->restore_position_and_focus()) {
      // Restore mouse position and window focus
      // Only for simulations, not for real casinos (stealth).
		  // Restoring the original state has to be done in reversed order
		  SetFocus(window_with_focus);
		  SetCursorPos(cursor_position.x, cursor_position.y);
    }
		action_sequence_needs_to_be_finished = false;
	}
}

bool CAutoplayer::TimeToHandleSecondaryFormulas() {
	// Disabled (N-1) out of N heartbeats (3 out of 4 seconds)
	// to avoid multiple fast clicking on the sitin / sitout-button.
	// Contrary to the old f$play-function we use a heartbeat-counter
	// for that logic, as with a small scrape-delay it was
	// still possible to act multiple times within the same second.
	// Scrape_delay() should always be > 0, there's a check in the GUI.
	assert(Preferences()->scrape_delay() > 0);
  // We need milli-seconds here, just like in the preferences
  // and also floating-point-division (3000.0) before we truncate to integer.
  // Otherwise we get always 0 and a constant secondary formula
  // would be executed every heartbeat and block all primary ones.
	int hearbeats_to_pause = 3000.0 / Preferences()->scrape_delay();
	if  (hearbeats_to_pause < 1) {
 		hearbeats_to_pause = 1;
 	}
	write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] TimeToHandleSecondaryFormulas() heartbeats to pause: %i\n",
		hearbeats_to_pause);
	bool act_this_heartbeat = ((p_heartbeat_thread->heartbeat_counter() % hearbeats_to_pause) == 0);
	write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] TimeToHandleSecondaryFormulas() act_this_heartbeat: %s\n",
		Bool2CString(act_this_heartbeat));
	return act_this_heartbeat;
}

bool CAutoplayer::HandleSitinFast() {
	// Sitting out bleeds blinds, so re-seat ASAP: check + click the Sit-In button EVERY
	// heartbeat instead of waiting for the ~3s TimeToHandleSecondaryFormulas cadence. A short
	// cooldown stops us re-clicking before the table registers we're back in; the "I Am Back"
	// label disappears after a good click, so this can't toggle us back out.
	static DWORD last_sitin_click = 0;
	if ((GetTickCount() - last_sitin_click) < 1200) {
		return false;
	}
	p_autoplayer_functions->CalcSecondaryFormulas();
	if (!p_autoplayer_functions->GetAutoplayerFunctionValue(k_hopper_function_sitin)) {
		return false;
	}
	if (p_casino_interface->LogicalAutoplayerButton(k_hopper_function_sitin)->Click()) {
		last_sitin_click = GetTickCount();
		write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] FAST Sit-In click (re-seating)\n");
		return true;
	}
	return false;
}

bool CAutoplayer::DoBetPot(void) {
	bool success = false;
	// Start with 2 * potsize, continue with lower betsizes, finally 1/4 pot
	for (int i=k_autoplayer_function_betpot_2_1; i<=k_autoplayer_function_betpot_1_4; i++) {
		if (p_autoplayer_functions->GetAutoplayerFunctionValue(i)) 	{
			write_log(Preferences()->debug_autoplayer(), 
        "[AutoPlayer] Function %s true.\n", k_standard_function_names[i]);
      if (ChangeBetPotActionToAllin(i)) {
        write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] Adjusting bhetpot_X_Y to allin.\n");
        return DoAllin();
      }
			if (p_tablemap->betpotmethod() == BETPOT_RAISE)	{
				success = p_casino_interface->ClickButtonSequence(i, k_autoplayer_function_raise, Preferences()->swag_delay_3());
			}	else {
				// Default: click only betpot
				success = p_casino_interface->LogicalAutoplayerButton(i)->Click();
			}
      if (!success) {
        // Backup action> try yo swag betpot_X_Y
        double betpot_amount = BetsizeForBetpot(i);
        write_log(Preferences()->debug_autoplayer(), 
          "[AutoPlayer] Betpot with buttons failed\n");
        write_log(Preferences()->debug_autoplayer(), 
          "[AutoPlayer] Trying to swag %.2f instead\n", betpot_amount);
        success = p_casino_interface->EnterBetsize(betpot_amount);
      }
		}
    if (success) {
			// Register the action
			// Treat betpot like swagging, i.e. raising a user-defined amount
      p_engine_container->UpdateAfterAutoplayerAction(k_autoplayer_function_betsize);
      p_autoplayer_trace->Print(ActionConstantNames(i), kAlwaysLogAutoplayerFunctions);
			return true;
    }
		// Else continue trying with the next betpot function
	}
	// We didn't click any betpot-button
	return false;
}

bool CAutoplayer::AnyPrimaryFormulaTrue() { 
  // Some auto-player-functions MUST exist. If not then they get auto-generated.
  // Missing all autoplayer-functions would be a bug that leads to time-outs.
  assert(p_function_collection != NULL);
  assert(p_function_collection->Exists(k_standard_function_names[k_autoplayer_function_fold]));
	for (int i=k_autoplayer_function_beep; i<=k_autoplayer_function_fold; ++i)
	{
		double function_result = p_autoplayer_functions->GetAutoplayerFunctionValue(i);
		if (i == k_autoplayer_function_betsize)
		{
			write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] AnyPrimaryFormulaTrue(): [%s]: %s\n",
				k_standard_function_names[i], Number2CString(function_result));
		}
		else
		{
			write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] AnyPrimaryFormulaTrue(): [%s]: %s\n",
				k_standard_function_names[i], Bool2CString(function_result));
		}
		if (function_result)
		{
			write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] AnyPrimaryFormulaTrue(): yes\n");
			return true;
		}
	}
	write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] AnyPrimaryFormulaTrue(): no\n");
	return false;
}

bool CAutoplayer::AnySecondaryFormulaTrue() {
  // Considering all hopper functions
  // and the functions f$prefold and f$chat.
	for (int i=k_hopper_function_sitin; i<=k_standard_function_chat; ++i)	{
		bool function_result = p_autoplayer_functions->GetAutoplayerFunctionValue(i);
		write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] AnySecondaryFormulaTrue(): [%s]: %s\n",
			k_standard_function_names[i], Bool2CString(function_result));
		if (function_result) {
			write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] AnySecondaryFormulaTrue(): yes\n");
			return true;
		}
	}
	write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] AnySecondaryFormulaTrue(): no\n");
	return false;
}

bool CAutoplayer::ExecutePrimaryFormulasIfNecessary() {
	write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] ExecutePrimaryFormulasIfNecessary()\n");
	// Return the mouse to where the user left it after the ENTIRE click sequence
	// completes (FCKRA / two-successive-clicks / numpad / nOkay / nConfirm), not
	// between clicks. Restores on every return path.
	CCursorRestorer _cursor_restorer;
	if (!AnyPrimaryFormulaTrue())	{
		APTrace("ExecutePrimaryFormulas -> STOP: AnyPrimaryFormulaTrue()=false (no f$ action true)");
		write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] No primary formula true. Nothing to do\n");
		return false;
	}
	APTrace("ExecutePrimaryFormulas -> AnyPrimaryFormulaTrue()=true");
	// Execute beep (if necessary) independent of all other conditions (mutex, etc.)
	// and with autoplayer-actions.
	ExecuteBeep();
  assert(p_engine_container->symbol_engine_autoplayer()->isfinalanswer());
	assert(p_engine_container->symbol_engine_autoplayer()->ismyturn());
	// Log EnhancedPrwin HandRank info if enhanced prwin is used
	if (p_iterator_thread->UseEnhancedPrWin() && Preferences()->debug_enhanced_prwin())
		p_engine_container->symbol_engine_prwin()->LogHandRank();
	// Precondition: my turn and isfinalanswer
	// So we have to take an action and are able to do so.
	// This function will ALWAYS try to click a button,
	// so we can handle the preparation once at the very beginning.
	CMyMutex mutex;
  if (!mutex.IsLocked()) {
		APTrace("ExecutePrimaryFormulas -> STOP: anti-collision mutex NOT locked (another OH/Hiss holds it?)");
		return false;
	}
	PrepareActionSequence();
	if (p_function_collection->EvaluateAutoplayerFunction(k_autoplayer_function_allin))	{
		if (DoAllin()) {
			APTrace("ExecutePrimaryFormulas -> handled by DoAllin()");
			return true;
		}
		// Else continue with swag and betpot
	}
	// Two-successive-clicks bet/raise (phone on-screen keypad). Same gating as the
	// other primary actions: only reached when ismyturn && isfinalanswer (asserted
	// above) under the action-sequence mutex. Fires only when the decision is a
	// bet/raise and a labelled region matches the configured text.
	if (HandleTwoSuccessiveClicksBetRaise()) {
		APTrace("ExecutePrimaryFormulas -> handled by two-successive-clicks bet/raise");
		return true;
	}
	if (DoBetPot())	{
		APTrace("ExecutePrimaryFormulas -> handled by DoBetPot()");
		return true;
	}
	if (DoBetsize()) {
		APTrace("ExecutePrimaryFormulas -> handled by DoBetsize()");
		return true;
	}
	APTrace("ExecutePrimaryFormulas -> falling through to ExecuteRaiseCallCheckFold()");
	return ExecuteRaiseCallCheckFold();
}

bool CAutoplayer::HandleTwoSuccessiveClicksBetRaise() {
	if (p_two_successive_clicks == NULL || p_function_collection == NULL) {
		return false;
	}
	// The raw OpenPPL decision for this betround is already expressed in BIG BLINDS
	// (RaiseTo N -> N, RaiseBy N -> ncallbets + N, percentage bets -> ncallbets +
	// pct*pot). A POSITIVE value means "bet/raise to this many big blinds".
	double decision_bb = 0.0;
	int betround = p_betround_calculator->betround();
	if (betround >= kBetroundPreflop && betround <= kBetroundRiver) {
		double d = p_function_collection->Evaluate(k_OpenPPL_function_names[betround]);
		if (d > 0) decision_bb = d;
	}
	// f$betsize is in DOLLARS (= decision * bblind). On this BB-only setup the
	// blind-guesser sometimes reports bblind == 0, which zeroes f$betsize even on a
	// genuine RaiseTo/RaiseBy decision -- the bot would then skip the keypad and
	// fall through to clicking Call (the reported "raise-to-3 calls instead" bug).
	double f_betsize = p_function_collection->Evaluate(k_standard_function_names[k_autoplayer_function_betsize]);
	bool wants_raise = (p_function_collection->Evaluate(k_standard_function_names[k_autoplayer_function_raise]) != 0);
	bool wants_allin = (p_function_collection->Evaluate(k_standard_function_names[k_autoplayer_function_allin]) != 0);
	// Fire on a BET, RAISE or ALL-IN decision (phone tables expose a keypad rather than a
	// betsize textbox, so we click the two configured region-centres to open it).
	if (!wants_raise && !wants_allin && f_betsize <= 0 && decision_bb <= 0) {
		return false;
	}
	// Keypad amount: PREFER the raw big-blind decision (decision_bb). f$betsize is
	// decision * bblind, and the blind-guesser is unreliable on this BB-only setup
	// (it has reported bblind = 0 and even 0.02, which makes f$betsize round down to
	// 0 on the numpad -- the "typed 0" bug). The on-screen keypad wants the big-blind
	// amount directly, so type decision_bb (e.g. RaiseTo 3 -> 3); only fall back to
	// f$betsize if the OpenPPL decision somehow isn't a positive bet/raise size.
	double betsize = (decision_bb > 0) ? decision_bb : f_betsize;
	// ALL-IN / RaiseMax: f$preflop returns the all-in action code, NOT a numeric RaiseTo
	// size, so decision_bb and f$betsize are both 0. On a phone table with no AllIn button
	// (DoAllin's button/slider/swag all fail), the jam must be typed on the keypad -- so
	// substitute our FULL stack (posted bet + remaining balance). Without this the size is
	// 0, the keypad path SKIPS, and the bot folds a hand it wanted to shove. This is the
	// leak that folded 77/AJ-type hands facing a shove. [equity]
	if (wants_allin && betsize <= 0 && p_table_state != NULL && p_table_state->User() != NULL) {
		betsize = p_table_state->User()->_bet.GetValue() + p_table_state->User()->_balance.GetValue();
		APTrace("two-successive-clicks: ALL-IN with no numeric size -> typing full stack");
	}
	write_log(k_always_log_basic_information,
		"[TwoClicks] BetRaise entry: wants_raise=%d wants_allin=%d f$betsize=%.2f decision_bb=%.2f -> keypad betsize=%.2f\n",
		wants_raise ? 1 : 0, wants_allin ? 1 : 0, f_betsize, decision_bb, betsize);
	if (betsize <= 0) {
		APTrace("two-successive-clicks: SKIP, no positive size (f$betsize=0 AND OpenPPL decision<=0)");
		return false;
	}
	if (!p_two_successive_clicks->HandleCycle(true)) {
		APTrace("two-successive-clicks: HandleCycle returned false (see [TwoClicks] SKIP/label line for why)");
		return false;
	}
	write_log(k_always_log_basic_information, "[AutoPlayer] Two-successive-clicks handled (primary path), betsize=%.2f\n", betsize);
	// After the two clicks open the on-screen keypad, type the bet/raise amount on
	// the numpad and press nOkay. Use the RAW entry: betsize is already the exact
	// big-blind amount from the .ohf decision; the casino AdjustedBetsize() would
	// clamp it to 0 against a BB-scale balance/min-raise (the "typed 0" bug).
	Sleep(p_two_successive_clicks->DelayMs());
	p_casino_interface->EnterBetsizeNumpadRaw(betsize);
	action_sequence_needs_to_be_finished = true;
	return true;
}

bool CAutoplayer::ExecuteRaiseCallCheckFold() {
	write_log(Preferences()->debug_autoplayer(), 
    "[AutoPlayer] ExecuteRaiseCallCheckFold()\n");
  // Some auto-player-functions MUST exist. If not then they get auto-generated.
  // Missing all autoplayer-functions would be a bug that leads to time-outs.
  assert(p_function_collection != NULL);
  assert(p_function_collection->Exists(k_standard_function_names[k_autoplayer_function_fold]));
	for (int i=k_autoplayer_function_raise; i<=k_autoplayer_function_fold; i++)	{
    if ((i == k_autoplayer_function_check) && p_engine_container->symbol_engine_chip_amounts()->call() > 0) {
      APTrace("ExecuteRaiseCallCheckFold -> skipping f$check (there is a bet to call)");
      write_log(k_always_log_errors,
        "[AutoPlayer] WARNING! Can't execute f$check because there is a bet to call\n");
      continue;
    }
		double v = p_function_collection->Evaluate(k_standard_function_names[i]);
		APTrace("ExecuteRaiseCallCheckFold -> %s = %.2f", k_standard_function_names[i], v);
		if (v) 	{
			CAutoplayerButton *btn = p_casino_interface->LogicalAutoplayerButton(i);
			APTrace("ExecuteRaiseCallCheckFold -> %s is TRUE; resolved button clickable=%d, calling Click()",
				k_standard_function_names[i], (btn != NULL && btn->IsClickable()) ? 1 : 0);
			if (btn->Click()) 			{
        p_engine_container->UpdateAfterAutoplayerAction(i);
        // Replay logging: capture the OHF decision tree for this clicked action. Read the
        // trace BEFORE Print() (which clears it). Off-loaded to the background writer.
        if (p_log_writer != NULL && p_log_writer->Enabled()) {
          FILETIME _ft; GetSystemTimeAsFileTime(&_ft);
          ULARGE_INTEGER _u; _u.LowPart = _ft.dwLowDateTime; _u.HighPart = _ft.dwHighDateTime;
          long long _ms = (long long)((_u.QuadPart - 116444736000000000ULL) / 10000ULL);
          CString _hand = (p_handreset_detector != NULL) ? p_handreset_detector->GetHandNumber() : CString("");
          int _br = (p_betround_calculator != NULL) ? p_betround_calculator->betround() : 0;
          CString _cards = (p_table_state != NULL && p_table_state->User() != NULL)
            ? p_table_state->User()->Cards() : CString("");
          CString _trace = (p_autoplayer_trace != NULL) ? p_autoplayer_trace->GetTraceText() : CString("");
          double _amt = p_function_collection->Evaluate(k_standard_function_names[k_autoplayer_function_betsize]);
          p_log_writer->LogDecision(_ms, CStringA(_hand).GetString(), _br, CStringA(_cards).GetString(),
            ActionConstantNames(i), _amt,
            p_function_collection->Evaluate(k_standard_function_names[k_autoplayer_function_fold]),
            p_function_collection->Evaluate(k_standard_function_names[k_autoplayer_function_call]),
            p_function_collection->Evaluate(k_standard_function_names[k_autoplayer_function_check]),
            p_function_collection->Evaluate(k_standard_function_names[k_autoplayer_function_raise]),
            p_function_collection->Evaluate(k_standard_function_names[k_autoplayer_function_allin]),
            _amt, CStringA(_trace).GetString());
          // Per-decision SYMBOL snapshot DISABLED per Emrald (advanced-logger symbols off).
          // Frames / decisions / hands / scrapes still log. To restore the replay Symbols
          // panel, re-enable a loop over EvaluateSymbol(...) -> p_log_writer->LogSymbol(...)
          // with the curated names (StackSize, balance, dealposition, betposition,
          // nopponentsplaying/seated, nplayersdealt, PotSize, AmountToCall, Raises, Calls,
          // prwin, f$PushFoldStack, bblind).
          // Replay context: snapshot the SCRAPED table state at this decision (each seated
          // seat's balance + bet, pot, board, hero cards) so the replay UI's Scrapes panel
          // shows what the bot actually READ -- and a mis-scrape (e.g. a garbage balance like
          // 1362222) is visible at the exact moment it mattered. is_crop=true (crop-derived).
          if (p_table_state != NULL) {
            for (int c = 0; c < kMaxNumberOfPlayers; ++c) {
              CPlayer *pl = p_table_state->Player(c);
              if (pl == NULL || !pl->seated()) continue;
              CString rn, vs;
              rn.Format("p%d_balance", c); vs.Format("%.2f", pl->_balance.GetValue());
              p_log_writer->LogScrape(_ms, CStringA(_hand).GetString(), _br,
                CStringA(rn).GetString(), CStringA(vs).GetString(), true);
              rn.Format("p%d_bet", c); vs.Format("%.2f", pl->_bet.GetValue());
              p_log_writer->LogScrape(_ms, CStringA(_hand).GetString(), _br,
                CStringA(rn).GetString(), CStringA(vs).GetString(), true);
            }
            CString vs; vs.Format("%.2f", p_table_state->Pot(0));
            p_log_writer->LogScrape(_ms, CStringA(_hand).GetString(), _br, "pot",
              CStringA(vs).GetString(), true);
            CString board;
            for (int b = 0; b < kNumberOfCommunityCards; ++b) {
              Card *cc = p_table_state->CommonCards(b);
              // Only log DEALT board cards. Undealt slots ToString() to junk (e.g. "9"),
              // which polluted the scrape log with boards like "9 9 9 9 9".
              if (cc != NULL && cc->IsKnownCard()) board += cc->ToString() + " ";
            }
            board.Trim();
            p_log_writer->LogScrape(_ms, CStringA(_hand).GetString(), _br, "board",
              CStringA(board).GetString(), true);
            p_log_writer->LogScrape(_ms, CStringA(_hand).GetString(), _br, "hero_cards",
              CStringA(_cards).GetString(), true);
          }
        }
        p_autoplayer_trace->Print(ActionConstantNames(i), kAlwaysLogAutoplayerFunctions);
				return true;
			}
			APTrace("ExecuteRaiseCallCheckFold -> Click() returned false for %s (button not clickable / no rect)",
				k_standard_function_names[i]);
		}
	}
	APTrace("ExecuteRaiseCallCheckFold -> nothing clicked (no true f$ matched a clickable button)");
	return false;
}

bool CAutoplayer::ExecuteBeep() {
	write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] ExecuteBeep (if f$beep is true)\n");
	if (p_function_collection->Evaluate(k_standard_function_names[k_autoplayer_function_beep]))	{
		// Pitch standard: 440 Hz, 1/2 second
		// http://en.wikipedia.org/wiki/A440_%28pitch_standard%29
		Beep(440, 500);
	}
	return false;
}

bool CAutoplayer::ExecuteSecondaryFormulasIfNecessary() {
	int executed_secondary_function = kUndefined;
	if (!AnySecondaryFormulaTrue())	{
		write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] All secondary formulas false.\n");
		write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] Nothing to do.\n");
		return false;
	}
	CMyMutex mutex;
	if (!mutex.IsLocked()) {
		return false;
	}
	PrepareActionSequence();
	// Prefold, close, rebuy and chat work require different treatment,
	// more than just clicking a simple region...
	if (p_autoplayer_functions->GetAutoplayerFunctionValue(k_standard_function_prefold)) {
		// Prefold is technically more than a simple button-click,
		// because we need to create an autoplayer-trace afterwards.
		if (DoPrefold()) {
			executed_secondary_function = k_standard_function_prefold;
		}
	}	else if (p_autoplayer_functions->GetAutoplayerFunctionValue(k_hopper_function_close))	{
		// CloseWindow is "final".
		// We don't expect any further action after that
		// and can return immediatelly.
		if (p_casino_interface->CloseWindow()) {
			executed_secondary_function = k_hopper_function_close;
		}
	}	else if (p_autoplayer_functions->GetAutoplayerFunctionValue(k_standard_function_chat)) 	{
			if (DoChat()) {
				executed_secondary_function = k_standard_function_chat;
			}
	}
	// Otherwise: handle the simple simple button-click
	// k_hopper_function_sitin,
	// k_hopper_function_sitout,
	// k_hopper_function_leave,
  // k_hopper_function_rematch,
	// k_hopper_function_autopost,
	else {
    for (int i=k_hopper_function_sitin; i<=k_hopper_function_autopost; ++i)	{
  		if (p_autoplayer_functions->GetAutoplayerFunctionValue(i))	{
        if (p_casino_interface->LogicalAutoplayerButton(i)->Click()) {
          executed_secondary_function = i;
          break;
        }
			}
		}
	}
  // Move f$rebuy to the VERY end
  // so that an (always?) positive f$rebuy-function
  // with blocked ir non-existing script
  // can't block all the other hopper-functions.
  // http://www.maxinmontreal.com/forums/viewtopic.php?f=156&t=19953
  if ((executed_secondary_function == kUndefined)
    && (p_autoplayer_functions->GetAutoplayerFunctionValue(k_hopper_function_rebuy))) {
    // This requires an external script and some time.
    // No further actions here eihter, but immediate return.
    bool result = p_rebuymanagement->TryToRebuy();
    if (result) {
      executed_secondary_function = k_hopper_function_rebuy;
    }
  }
	if (executed_secondary_function != kUndefined) {
		FinishActionSequenceIfNecessary();
    if (Preferences()->log_hopper_functions()) {
      // No update after action required here,
      // as prefold already cares about that
      // and the other actions don't need it.
      p_autoplayer_trace->Print(ActionConstantNames(executed_secondary_function), false);
    }
		return true;
	}
	action_sequence_needs_to_be_finished = false;
	return false;
}

#define ENT CSLock lock(m_critsec);
	
void CAutoplayer::EngageAutoplayer(bool to_be_enabled_or_not) { 
	ENT 
	// Set correct button state
	// We have to be careful, as during initialization the GUI does not yet exist.
	assert(p_flags_toolbar != NULL);
	p_flags_toolbar->CheckButton(ID_MAIN_TOOLBAR_AUTOPLAYER, to_be_enabled_or_not);

	if (to_be_enabled_or_not) 
	{
		if (!p_function_collection->BotLogicCorrectlyParsed())
		{
			// Invalid formula
			// Can't autoplay
			to_be_enabled_or_not = false;
    }
	}
  if (to_be_enabled_or_not) {
    p_flags_toolbar->ResetButtonsOnAutoplayerOn();
  } else {
    p_flags_toolbar->ResetButtonsOnAutoplayerOff();
  }
	// Set value at the very last to be extra safe
	// and avoid problems with multiple threads
	// despite we use synchronization ;-)
	_autoplayer_engaged = to_be_enabled_or_not;
	// NEW DRIVER MODEL [Emrald]: the autoplayer and the NN driver are NO LONGER mutually exclusive.
	// The autoplayer is ALWAYS the executor when enabled; on NLH the NN (when engaged) BYPASSES the
	// OHF read in DoAutoplayer and forces the action itself, while the autoplayer runs the OHF as the
	// always-on fallback (PLO/PLO8, or whenever the NN is off). So engaging the autoplayer must NOT
	// disengage the NN driver any more.
}

#undef ENT

bool CAutoplayer::DoChat(void) {
	assert(p_function_collection->EvaluateAutoplayerFunction(k_standard_function_chat) != 0);
	if (!IsChatAllowed())	{
		write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] No chat, because chat turned off.\n");
		return false;
	}
	// Converting the result of the $chat-function to a string.
	// Will be ignored, if we already have an unhandled chat message.
	RegisterChatMessage(p_function_collection->EvaluateAutoplayerFunction(k_standard_function_chat));
	if (_the_chat_message == NULL) {
		write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] No chat, because wrong chat code. Please read: ""Available chat messages"" .\n");
		return false ;
	}

	return p_casino_interface->EnterChatMessage(CString(_the_chat_message));
}

bool CAutoplayer::DoAllin(void) {
	bool success = false;
	write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] Starting DoAllin...\n");

	int number_of_clicks = 1; // Default is: single click with the mouse
	if (p_tablemap->buttonclickmethod() == BUTTON_DOUBLECLICK) 	{
		number_of_clicks = 2;
	}
  // Trying to go allin using these 3 methods in the following order:
  //	1) click max (or allin), then optionally raise, depending on allinconfirmationmethod
  //	2) use the slider if it exists in the TM
	//	3) swag the balance 
	if (p_tablemap->allinconfirmationmethod() != 0)	{
		// Clicking max (or allin) and then raise
    success = p_casino_interface->ClickButtonSequence(k_autoplayer_function_allin,
      k_autoplayer_function_raise, Preferences()->swag_delay_3());
	}	else {
    // Clicking only max (or allin), but not raise
		success = p_casino_interface->LogicalAutoplayerButton(k_autoplayer_function_allin)->Click();
  }
	if (!success) {
    // Try the slider
		success = p_casino_interface->UseSliderForAllin();
  }
  if (!success) {
		// Last case: try to swagging the balance
		success = p_casino_interface->EnterBetsizeForAllin();
	}
	if (success) {
		// Not really necessary to register the action,
		// as the game is over and there is no doallin-symbol,
		// but it does not hurt to register it anyway.
    p_engine_container->UpdateAfterAutoplayerAction(k_autoplayer_function_allin);
    p_autoplayer_trace->Print(ActionConstantNames(k_autoplayer_function_allin), kAlwaysLogAutoplayerFunctions);
		return true;
	}
	return false;
}

// Verbose, self-contained dump of the whole button-decision state to its own file
// (logs\button_debug.log), one block per cadence. Written to diagnose why an action
// (fold/call/check/all-in) isn't clicking: shows what buttons Hiss sees (i#state /
// i#label / classified type / clickable), my-turn/final-answer, and the f$ decisions.
void CAutoplayer::DumpButtonDebug() {
	if (!Preferences()->debug_autoplayer()) return;
	if (p_casino_interface == NULL || p_engine_container == NULL
			|| p_function_collection == NULL || p_scraper == NULL) {
		return;
	}
	CSymbolEngineAutoplayer *ap = p_engine_container->symbol_engine_autoplayer();
	CFunctionCollection *fc = p_function_collection;
	int visible = p_casino_interface->NumberOfVisibleAutoplayerButtons();

	// Only log the interesting cadences. NOTE: do NOT gate on "any f$ decision is
	// true" - a forcing .ohf (e.g. WHEN Others Fold FORCE) makes f$fold=1 every
	// heartbeat, which would log (and OCR) on every single scrape and lag the bot.
	if (!(visible > 0 || ap->ismyturn() || ap->isfinalanswer())) {
		return;
	}

	// Formula values are cached per-heartbeat (cheap); region OCR is NOT, so below
	// we read button state/label from the already-scraped CAutoplayerButton objects
	// instead of calling p_scraper->EvaluateRegion() (which re-runs OCR).
	double f_fold  = fc->Evaluate(k_standard_function_names[k_autoplayer_function_fold]);
	double f_check = fc->Evaluate(k_standard_function_names[k_autoplayer_function_check]);
	double f_call  = fc->Evaluate(k_standard_function_names[k_autoplayer_function_call]);
	double f_raise = fc->Evaluate(k_standard_function_names[k_autoplayer_function_raise]);
	double f_allin = fc->Evaluate(k_standard_function_names[k_autoplayer_function_allin]);
	double f_bet   = fc->Evaluate(k_standard_function_names[k_autoplayer_function_betsize]);

	CString path = LogsDirectory() + "button_debug.log";
	FILE *f = fopen(path.GetString(), "a");
	if (f == NULL) return;

	SYSTEMTIME st; GetLocalTime(&st);
	fprintf(f, "==== %02d:%02d:%02d.%03d  cadence (engaged=%d) ====\n",
		st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, autoplayer_engaged() ? 1 : 0);
	fprintf(f, "ismyturn=%d isfinalanswer=%d myturnbits=0x%02x visible=%d FCKRA=\"%s\"\n",
		ap->ismyturn() ? 1 : 0, ap->isfinalanswer() ? 1 : 0, ap->myturnbits(),
		visible, ap->GetFCKRAString().GetString());
	// Why isfinalanswer might be false (the gate that blocks fold/call/check/allin):
	bool iter = (p_iterator_thread != NULL) && p_iterator_thread->IteratorThreadWorking();
	bool hero_known = p_table_state->User()->HasKnownCards();
	int stable = (p_stableframescounter != NULL) ? p_stableframescounter->NumberOfStableFrames() : -1;
	int need_frames = Preferences()->frame_delay();
	int uchair = p_engine_container->symbol_engine_userchair()->userchair();
	bool uconf = p_engine_container->symbol_engine_userchair()->userchair_confirmed();
	fprintf(f, "  finalanswer_factors: userchair=%d(confirmed=%d) hero_has_known_cards=%d iterator_working=%d stable_frames=%d (need %d)\n",
		uchair, uconf ? 1 : 0, hero_known ? 1 : 0, iter ? 1 : 0, stable, need_frames);

	for (int i = 0; i < k_max_number_of_buttons; ++i) {
		char hc = (i < 10) ? (char)('0' + i) : (char)('a' + i - 10);
		CAutoplayerButton *b = &p_casino_interface->_technical_autoplayer_buttons[i];
		CString label = b->Label();   // cached scrape result, no OCR
		// Skip empty/uninteresting buttons to keep the dump short.
		if (label.IsEmpty() && !b->IsClickable()) continue;
		const char *type = b->IsFold() ? "FOLD" : b->IsCall() ? "CALL" : b->IsCheck() ? "CHECK"
			: b->IsRaise() ? "RAISE" : b->IsAllin() ? "ALLIN" : "(unclassified)";
		fprintf(f, "  i%c: label=\"%s\" -> type=%s clickable=%d\n",
			hc, label.GetString(), type, b->IsClickable() ? 1 : 0);
	}

	fprintf(f, "  decision: f$fold=%.0f f$check=%.0f f$call=%.0f f$raise=%.0f f$allin=%.0f f$betsize=%.2f\n",
		f_fold, f_check, f_call, f_raise, f_allin, f_bet);

	// Raw OpenPPL decision for this betround + bblind, to diagnose RaiseTo/RaiseBy:
	//   positive  -> bet/raise size in big blinds (e.g. RaiseTo 3 -> 3.00)
	//   <= -1000  -> elementary action constant (Raise/Call/Check/Fold)
	{
		int dbr = (p_betround_calculator != NULL) ? p_betround_calculator->betround() : 0;
		double raw_decision = 0.0;
		if (dbr >= kBetroundPreflop && dbr <= kBetroundRiver) {
			raw_decision = p_function_collection->Evaluate(k_OpenPPL_function_names[dbr]);
		}
		double bb = (p_engine_container->symbol_engine_tablelimits() != NULL)
			? p_engine_container->symbol_engine_tablelimits()->bblind() : -1.0;
		fprintf(f, "  openppl: betround=%d raw_decision=%.3f bblind=%.3f (positive=>raise-to-BB, <=-1000=>elementary)\n",
			dbr, raw_decision, bb);
	}

	fclose(f);
}

void CAutoplayer::EmitDecisionTrace() {
	// One concise line per decision change into the Terminal's Decisions pane:
	//   [betround] ACTION [size]   -- shows how the .ohf decision resolved.
	if (p_function_collection == NULL || p_engine_container == NULL) return;
	CSymbolEngineAutoplayer *ap = p_engine_container->symbol_engine_autoplayer();
	// PERSIST "as a memory": do NOT clear the on-table decision when it's no longer our turn -- keep the
	// last decided action shown above the hole cards until the NEXT decision replaces it (otherwise it
	// only flashed for the single decision heartbeat and was never visible). [Emrald]
	if (ap == NULL || !ap->ismyturn() || !ap->isfinalanswer()) return;
	double f_fold  = p_function_collection->Evaluate(k_standard_function_names[k_autoplayer_function_fold]);
	double f_check = p_function_collection->Evaluate(k_standard_function_names[k_autoplayer_function_check]);
	double f_call  = p_function_collection->Evaluate(k_standard_function_names[k_autoplayer_function_call]);
	double f_raise = p_function_collection->Evaluate(k_standard_function_names[k_autoplayer_function_raise]);
	double f_allin = p_function_collection->Evaluate(k_standard_function_names[k_autoplayer_function_allin]);
	double f_bet   = p_function_collection->Evaluate(k_standard_function_names[k_autoplayer_function_betsize]);
	int br = (p_betround_calculator != NULL) ? p_betround_calculator->betround() : 0;
	const char *brn = (br == 1) ? "preflop" : (br == 2) ? "flop" : (br == 3) ? "turn"
		: (br == 4) ? "river" : "?";
	CString action;
	if (f_allin != 0)            action = "\x1b[31mALL-IN\x1b[0m";
	else if (f_raise != 0 || f_bet > 0) action.Format("\x1b[33mRAISE\x1b[0m \x1b[1;37m%.2f\x1b[0m", f_bet);
	else if (f_call != 0)        action = "\x1b[36mCALL\x1b[0m";
	else if (f_check != 0)       action = "\x1b[32mCHECK\x1b[0m";
	else if (f_fold != 0)        action = "\x1b[90mFOLD\x1b[0m";
	else                         action = "(none)";
	// Publish the PLAIN action for the on-table RED decision overlay (HudOverlayWindow, drawn above the
	// hero's cards with the table name so Emrald isn't confused which table it's for). [Emrald]
	{
		CString plain;
		if (f_allin != 0)            plain = "ALL-IN";
		else if (f_raise != 0 || f_bet > 0) plain.Format("RAISE %.2f", f_bet);
		else if (f_call != 0)        plain = "CALL";
		else if (f_check != 0)       plain = "CHECK";
		else if (f_fold != 0)        plain = "FOLD";
		else                         plain = "";
		if (!plain.IsEmpty()) {
			// Real decision: publish it + STAMP the time. Keep the last decision/text AFTER the turn ends so
			// the RED overlay can TRAIL ~10s and fade out (HudOverlayWindow), unlike the per-hand HUD. The
			// overlay's own 10s timer hides it; we no longer clear the text the instant it's not our turn.
			strcpy_s(g_hero_decision_text, sizeof(g_hero_decision_text), CStringA(plain).GetString());
			g_hero_decision_tick = GetTickCount();
		}
		g_hero_decision_active = !plain.IsEmpty();
	}
	CString line;
	line.Format("\x1b[36m[%s]\x1b[0m %s", brn, action.GetString());
	if (line == _last_decision_line) return;     // only on change
	_last_decision_line = line;
	SYSTEMTIME st; GetLocalTime(&st);
	CString stamped;
	stamped.Format("\x1b[90m%02d:%02d:%02d\x1b[0m %s\r\n", st.wHour, st.wMinute, st.wSecond, line.GetString());
	ChatTerminalAppendToScreen("main", kChatTerminalDecisions, stamped);
}

// ---------------------------------------------------------------------------
// SHARED TURN-LOCK: hold the cross-instance mouse lock for the WHOLE turn -- every click of a multi-step
// raise progression (Raise/Options -> preset/keypad -> confirm), which spans several heartbeats and can
// CLICK-MISS RETRY -- so the OTHER phone bot cannot click in between. Acquired once when we're about to
// act, released when our turn ends (ismyturn falls) or a TTL fires (crash/abort safety). It is the SAME
// named mutex as CMyMutex, so it is recursive on our own heartbeat thread (CMyMutex re-enters freely)
// and blocks the OTHER process for the whole progression. [Emrald: lock the other bot until the
// two-successive-clicks progression is fully done]
static HANDLE g_turn_mouse_lock = NULL;
static bool   g_turn_lock_held  = false;
static DWORD  g_turn_lock_tick  = 0;
static const DWORD kTurnLockTtlMs = 8000;          // never hold longer than one realistic turn

static bool AcquireTurnLock() {
	if (g_turn_lock_held) return true;             // already ours for this turn
	if (g_turn_mouse_lock == NULL)
		g_turn_mouse_lock = ::CreateMutex(NULL, FALSE, _T("HissSharedMouseClickLock"));
	if (g_turn_mouse_lock == NULL) return true;    // can't create the lock -> never block ourselves
	DWORD w = ::WaitForSingleObject(g_turn_mouse_lock, 0);
	if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) {   // ABANDONED = prior holder crashed; we own it now
		g_turn_lock_held = true; g_turn_lock_tick = ::GetTickCount();
		return true;
	}
	return false;                                  // another bot is mid-progression -> wait our turn
}
static void ReleaseTurnLock() {
	if (g_turn_lock_held && g_turn_mouse_lock != NULL) {
		::ReleaseMutex(g_turn_mouse_lock);
		g_turn_lock_held = false;
	}
}

// Cache the FCKRA (primary) + TIOLP (secondary/hopper) clickable-button indicators so the React table
// view can mirror the main view's bottom-corner indicators (the HTTP thread reads these cached strings
// -- never the casino interface directly), AND so /api/table-state can tell a DRIVER which buttons
// actually exist. [Emrald]
//
// This must run on EVERY heartbeat, which is why it no longer lives in DoAutoplayer(): the heartbeat
// only calls DoAutoplayer() when the autoplayer is ENGAGED. Under the NN driver (autoplayer off) it
// therefore never ran, and g_fckra_indicator stayed EMPTY -- so nothing in the system knew which
// buttons were on screen. The phone tablemap stacks check/call/raise-options on the SAME rectangle and
// distinguishes them by LABEL, so a "check" with no Check button present was clicked blind and landed
// on RAISE OPTIONS: the raise panel popped open and the hand stalled (hand 2777062344, AK).
void CAutoplayer::CacheButtonIndicators(void) {
	extern char g_fckra_indicator[8]; extern char g_tiolp_indicator[8];
	char f[8]; int fi = 0; char t[8]; int ti = 0;
	if (p_casino_interface != NULL) {
		if (p_casino_interface->LogicalAutoplayerButton(k_autoplayer_function_fold)->IsClickable())  f[fi++] = 'F';
		if (p_casino_interface->LogicalAutoplayerButton(k_autoplayer_function_call)->IsClickable())  f[fi++] = 'C';
		if (p_casino_interface->LogicalAutoplayerButton(k_autoplayer_function_check)->IsClickable()) f[fi++] = 'K';
		if (p_casino_interface->LogicalAutoplayerButton(k_autoplayer_function_raise)->IsClickable()) f[fi++] = 'R';
		if (p_casino_interface->LogicalAutoplayerButton(k_autoplayer_function_allin)->IsClickable()) f[fi++] = 'A';
		if (p_casino_interface->LogicalAutoplayerButton(k_hopper_function_autopost)->IsClickable())  t[ti++] = 'T';
		if (p_casino_interface->LogicalAutoplayerButton(k_hopper_function_sitin)->IsClickable())     t[ti++] = 'I';
		if (p_casino_interface->LogicalAutoplayerButton(k_hopper_function_sitout)->IsClickable())    t[ti++] = 'O';
		if (p_casino_interface->LogicalAutoplayerButton(k_hopper_function_leave)->IsClickable())     t[ti++] = 'L';
		if (p_casino_interface->LogicalAutoplayerButton(k_standard_function_prefold)->IsClickable()) t[ti++] = 'P';
	}
	f[fi] = '\0'; t[ti] = '\0';
	strcpy_s(g_fckra_indicator, sizeof(g_fckra_indicator), f);
	strcpy_s(g_tiolp_indicator, sizeof(g_tiolp_indicator), t);
}

void CAutoplayer::DoAutoplayer(void) {
	write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] Starting Autoplayer cadence...\n");
	DumpButtonDebug();
	EmitDecisionTrace();
	// Scarlet Beast server-scrape: there are no screen buttons to click; decide via
	// the formulas and POST the action to poker.scarletbeast.com instead.
	if (p_scarlet_beast != NULL && p_scarlet_beast->ScrapeFromServer()) {
		APTrace("DoAutoplayer -> SERVER mode (ScrapeFromServer=1): POSTing to API, NOT clicking screen buttons");
		DoAutoplayerServer();
		return;
	}
  // NOTE: the two-successive-clicks bet/raise used to fire here, BEFORE the
  // isfinalanswer gate. It now runs inside ExecutePrimaryFormulasIfNecessary()
  // alongside fold/call/check/allin, so it is gated by ismyturn + isfinalanswer
  // (and the anti-collision mutex / action-sequence) like every other action.
  CheckBringKeyboard();
  write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] Number of visible buttons: %d (%s)\n", 
		p_casino_interface->NumberOfVisibleAutoplayerButtons(),
		p_engine_container->symbol_engine_autoplayer()->GetFCKRAString());
	// Care about i86X regions first, because they are usually used 
	// to handle popups which occlude the table (unstable input)
	if (p_casino_interface->HandleInterfacebuttonsI86())	{
    write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] Interface buttons (popups) handled\n");
    action_sequence_needs_to_be_finished = true;
	  goto AutoPlayerCleanupAndFinalization;
  }
  // Fast Sit-In: re-seat ASAP -- handle the Sit-In button every heartbeat (bypasses the ~3s
  // secondary-formula cadence), so a sat-out bot doesn't blind off waiting to click "I Am Back".
  if (HandleSitinFast()) {
    write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] Fast Sit-In handled\n");
    action_sequence_needs_to_be_finished = true;
    goto AutoPlayerCleanupAndFinalization;
  }
  // Care about sitin, sitout, leave, etc.
  if (TimeToHandleSecondaryFormulas())	{
	  p_autoplayer_functions->CalcSecondaryFormulas();	  
    if (ExecuteSecondaryFormulasIfNecessary())	{
      write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] Secondary formulas executed\n");
      goto AutoPlayerCleanupAndFinalization;
    } else {
      write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] No secondary formulas to be handled.\n");
    }
  } else {
    write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] Not executing secondary formulas this heartbeat\n");
	}
  if (!p_engine_container->symbol_engine_userchair()->userchair_confirmed()) {
    // Since OH 4.0.5 we support autoplaying immediatelly after connection
		// without the need to know the userchair to act on secondary formulas.
		// However: for primary formulas (f$alli, f$rais, etc.)
		// knowing the userchair (combination of cards and buttons) is a must.
		APTrace("DoAutoplayer -> STOP: userchair not confirmed, skipping primary formulas");
		write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] Skipping primary formulas because userchair unknown\n");
		goto AutoPlayerCleanupAndFinalization;
  }
	// FCKRA once-per-turn: a turn is one contiguous stretch of ismyturn. On the
	// rising edge (it just became my turn) clear the latch; once we take a primary
	// action this turn we won't take another until the next turn.
	bool my_turn_now = p_engine_container->symbol_engine_autoplayer()->ismyturn();
	if (my_turn_now && !_was_my_turn) {
		_acted_this_turn = false;
		_retry_count = 0;
		APTrace("DoAutoplayer -> new turn detected (ismyturn rising edge), FCKRA latch cleared");
	}
	// CLICK-MISS RETRY: a successful action ends our turn (ismyturn -> false). If we acted
	// but it's STILL our turn N heartbeats later AND the buttons are still present
	// (isfinalanswer), the click did not register (e.g. a missed/mis-aimed tap, or the
	// two-successive-clicks keypad entry that didn't land) -- clear the latch to re-click.
	// Wait N heartbeats so we don't false-trigger on scrape lag; cap retries per turn.
	static const int kRetryAfterHeartbeats = 4;
	static const int kMaxClickRetries = 4;
	if (my_turn_now && _acted_this_turn && p_heartbeat_thread != NULL
	    && p_engine_container->symbol_engine_autoplayer()->isfinalanswer()) {
		int elapsed = p_heartbeat_thread->heartbeat_counter() - _acted_heartbeat;
		if (elapsed >= kRetryAfterHeartbeats && _retry_count < kMaxClickRetries) {
			_acted_this_turn = false;
			_retry_count++;
			write_log(k_always_log_basic_information,
				"[AutoPlayer] CLICK-MISS RETRY: still my turn %d heartbeats after acting and buttons still present "
				"-> re-clicking (retry %d/%d)\n", elapsed, _retry_count, kMaxClickRetries);
			APTrace("DoAutoplayer -> click-miss retry: latch cleared to re-act");
		}
	}
	_was_my_turn = my_turn_now;

	// Release the shared turn-lock the moment our turn ENDS (ismyturn fell) or a TTL fires, so the other
	// bot can take its turn. While our turn lasts we keep it -> every click of our progression is exclusive.
	if (!my_turn_now || (g_turn_lock_held && (::GetTickCount() - g_turn_lock_tick) > kTurnLockTtlMs)) {
		ReleaseTurnLock();
	}

	write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] Going to evaluate primary formulas.\n");
	// NEW DRIVER MODEL [Emrald]: the autoplayer is ALWAYS the executor (it already handled sit-in /
	// popups / secondary formulas above), but on a NLH table when the NN driver (or ULTRA, which drives
	// through the NN) is engaged, the NN BYPASSES the OHF read -- it computes the decision and forces it
	// via /api/action. So skip the OHF PRIMARY action here to avoid double-acting. On PLO/PLO8 the NN is
	// gated off (Hold'em-only) so the OHF runs as the always-on fallback; same whenever the NN is
	// disengaged. g_table_is_omaha covers both PLO and PLO8.
	extern bool g_nn_driver_engaged; extern bool g_table_is_omaha;
	bool nn_bypasses_ohf_nlh = (g_nn_driver_engaged && !g_table_is_omaha);
	if (nn_bypasses_ohf_nlh) {
		APTrace("DoAutoplayer -> NN bypasses OHF on NLH: deferring the primary decision to the NN driver");
	}
	if (!nn_bypasses_ohf_nlh && p_engine_container->symbol_engine_autoplayer()->isfinalanswer())	{
		if (_acted_this_turn) {
			APTrace("DoAutoplayer -> STOP: already took an FCKRA action this turn (once-per-turn latch)");
		} else if (!AcquireTurnLock()) {
			// Another Hiss bot is mid-progression on the shared mouse -> wait. We retry every heartbeat
			// (still within our turn window) until it releases; our turn-latch is untouched so nothing is lost.
			APTrace("DoAutoplayer -> STOP: another Hiss bot holds the shared mouse lock; waiting for our turn");
			write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] Waiting: the other bot holds the shared mouse lock\n");
		} else {
			APTrace("DoAutoplayer -> isfinalanswer=1, calling ExecutePrimaryFormulasIfNecessary()");
			p_autoplayer_functions->CalcPrimaryFormulas();
			if (ExecutePrimaryFormulasIfNecessary()) {
				_acted_this_turn = true;   // latch: no more FCKRA actions until next turn
				// Stamp the heartbeat so the click-miss retry above can tell whether this
				// click actually registered (turn should end within a few heartbeats). For
				// the two-successive-clicks bet/raise this is set AFTER the final keypad
				// entry, so the retry window measures from the final click as intended.
				_acted_heartbeat = (p_heartbeat_thread != NULL) ? p_heartbeat_thread->heartbeat_counter() : 0;
				APTrace("DoAutoplayer -> primary action taken; FCKRA latch set for this turn");
			}
		}
	}	else {
		APTrace("DoAutoplayer -> STOP: isfinalanswer=0 at action time, not executing autoplayer logic");
		write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] No final answer, therefore not executing autoplayer-logic.\n");
	}
  // Gotos are usually considered bad code.
  // Here it simplifies the control-flow.
AutoPlayerCleanupAndFinalization:  
	FinishActionSequenceIfNecessary();
	write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] ...ending Autoplayer cadence.\n");
}

void CAutoplayer::DoAutoplayerServer() {
  // Acting through the Scarlet Beast API (POST /act) rather than clicking buttons.
  // Verbs: fold|check|call|bet|raise. bet.amount = chips ADDED this street;
  // raise.amount = total street commitment ("raise to"). All-in = bet/raise of the
  // whole stack. We act exactly once per server state-version.
  if (p_scarlet_beast == NULL) return;
  if (!p_engine_container->symbol_engine_userchair()->userchair_confirmed()) {
    write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] Server: userchair unknown, not acting\n");
    return;
  }
  int my_server_seat = p_engine_container->symbol_engine_userchair()->userchair() + 1; // 1-based
  if (p_scarlet_beast->ServerToAct() != my_server_seat) {
    return;  // not our turn
  }
  long version = p_scarlet_beast->ServerStateVersion();
  if (version != 0 && version == _last_server_act_version) {
    return;  // already acted on this exact state
  }

  // Make sure the primary formulas are freshly evaluated.
  p_autoplayer_functions->CalcPrimaryFormulas();

  long   current_bet  = p_scarlet_beast->ServerCurrentBet();
  long   min_raise    = p_scarlet_beast->ServerMinRaise(
                          (long)p_engine_container->symbol_engine_tablelimits()->bblind());
  if (min_raise < 1) min_raise = 1;
  double my_committed = p_table_state->User()->_bet.GetValue();
  double my_stack     = p_table_state->User()->_balance.GetValue();
  double call_amt     = p_engine_container->symbol_engine_chip_amounts()->call();
  long   max_raise_to = (long)(my_committed + my_stack);

  const char *verb = NULL;
  long amount = 0;
  bool with_amount = false;
  int  action_code = k_autoplayer_function_fold;

  if (p_autoplayer_functions->GetAutoplayerFunctionValue(k_autoplayer_function_allin)) {
    if (current_bet <= 0) { verb = "bet";   amount = (long)my_stack; }
    else                  { verb = "raise"; amount = max_raise_to; }
    with_amount = true;
    action_code = k_autoplayer_function_allin;
  } else {
    // betpot fractions (largest first), else f$betsize
    double target = 0;
    for (int i = k_autoplayer_function_betpot_2_1; i <= k_autoplayer_function_betpot_1_4; ++i) {
      if (p_autoplayer_functions->GetAutoplayerFunctionValue(i)) { target = BetsizeForBetpot(i); break; }
    }
    if (target <= 0) {
      target = p_autoplayer_functions->GetAutoplayerFunctionValue(k_autoplayer_function_betsize);
    }
    if (target > 0) {
      if (current_bet <= 0) {
        verb = "bet";
        amount = (long)(target - my_committed);          // chips added
        if (amount > (long)my_stack) amount = (long)my_stack;
        if (amount < min_raise)      amount = min_raise;  // respect table minimum
      } else {
        verb = "raise";
        amount = (long)target;                            // raise-to total
        long min_to = current_bet + min_raise;
        if (amount < min_to)      amount = min_to;
        if (amount > max_raise_to) amount = max_raise_to;
      }
      with_amount = true;
      action_code = k_autoplayer_function_betsize;
    } else if (p_autoplayer_functions->GetAutoplayerFunctionValue(k_autoplayer_function_raise)) {
      verb = "raise";
      amount = current_bet + min_raise;                   // a min-raise
      if (amount > max_raise_to) amount = max_raise_to;
      with_amount = true;
      action_code = k_autoplayer_function_raise;
    } else if (p_autoplayer_functions->GetAutoplayerFunctionValue(k_autoplayer_function_call)) {
      verb = "call";  action_code = k_autoplayer_function_call;
    } else if (p_autoplayer_functions->GetAutoplayerFunctionValue(k_autoplayer_function_check)
               && call_amt <= 0) {
      verb = "check"; action_code = k_autoplayer_function_check;
    } else if (p_autoplayer_functions->GetAutoplayerFunctionValue(k_autoplayer_function_fold)) {
      verb = "fold";  action_code = k_autoplayer_function_fold;
    }
  }

  if (verb == NULL) {
    // No formula fired: avoid an unnecessary timeout-fold when we can check for free.
    if (call_amt <= 0) { verb = "check"; action_code = k_autoplayer_function_check; }
    else               { verb = "fold";  action_code = k_autoplayer_function_fold; }
  }

  std::string body = std::string("{\"action\":\"") + verb + "\"";
  if (with_amount) {
    char b[40];
    sprintf_s(b, sizeof(b), ",\"amount\":%ld", amount);
    body += b;
  }
  body += "}";

  p_scarlet_beast->Act(p_scarlet_beast->TableId(), body);
  _last_server_act_version = version;
  write_log(kAlwaysLogAutoplayerFunctions,
    "[AutoPlayer] Scarlet Beast act seat %d: %s -> HTTP %d (%s)\n",
    my_server_seat, body.c_str(), p_scarlet_beast->LastStatus(),
    p_scarlet_beast->LastOk() ? "ok" : "FAILED");
  p_engine_container->UpdateAfterAutoplayerAction(action_code);
  p_autoplayer_trace->Print(ActionConstantNames(action_code), kAlwaysLogAutoplayerFunctions);
}

bool CAutoplayer::DoBetsize() {
  double betsize = p_function_collection->EvaluateAutoplayerFunction(k_autoplayer_function_betsize);
  double betsize_for_allin = p_table_state->User()->_bet.GetValue()
	  + p_table_state->User()->_balance.GetValue();
	if (betsize > 0) 	{
    if (!_already_executing_allin_adjustment) {
      // We have to prevent a potential endless loop here>
      // swag -> adjusted allin -> swag allin -> adjusted allin ...
      if (ChangeBetsizeToAllin(betsize)) {
        _already_executing_allin_adjustment = true;
        write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] Adjusting betsize to allin.\n");
        bool success = DoAllin();
        _already_executing_allin_adjustment = false;
        return success;
      }
    }
		// Try the slider
		int success = p_casino_interface->UseSliderForBetsize(betsize, betsize_for_allin);
		if (!success) {
			success = p_casino_interface->EnterBetsize(betsize);
		}
		if (success) {
      write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] betsize %.2f (adjusted) entered\n",
        betsize);
      p_engine_container->UpdateAfterAutoplayerAction(k_autoplayer_function_betsize);
      p_autoplayer_trace->Print(ActionConstantNames(k_autoplayer_function_betsize), kAlwaysLogAutoplayerFunctions);
			return true;
		}
    write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] Failed to enter betsize %.2f\n",
      betsize);
    return false;
	}
	write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] Don't f$betsize, because f$betsize evaluates to 0.\n");
	return false;
}

bool CAutoplayer::DoPrefold(void) {
	assert(p_function_collection->EvaluateAutoplayerFunction(k_standard_function_prefold) != 0);
	if (!p_table_state->User()->HasKnownCards()) {
		write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] Prefold skipped. No known cards.\n");
		write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] Smells like a bad f$prefold-function.\n");
	}
	if (p_casino_interface->LogicalAutoplayerButton(k_standard_function_prefold)->Click())	{
    p_engine_container->UpdateAfterAutoplayerAction(k_autoplayer_function_fold);
		write_log(Preferences()->debug_autoplayer(), "[AutoPlayer] Prefold executed.\n");
		return true;
	}
	return false;
}

