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
#include "CHeartbeatThread.h"

#include <process.h>
#include "CAutoconnector.h"
#include "CAutoplayer.h"
#include "CAutoplayerFunctions.h"
#include "CBetroundCalculator.h"
#include "CHeartbeatDelay.h"
#include "CEngineContainer.h"
#include "CIteratorThread.h"
#include "CLazyScraper.h"
#include "COpenHoldemHopperCommunication.h"
#include "COpenHoldemStarter.h"
#include "COpenHoldemStatusbar.h"
#include "COpenHoldemTitle.h"

#include "CFormulaParser.h"
#include "CFunctionCollection.h"
#include "CScarletBeast.h"
#include "CScraper.h"
#include "CSymbolEngineAutoplayer.h"
#include "CSymbolEngineChipAmounts.h"
#include "CSymbolEngineUserchair.h"
#include "..\CTablemap\CTablemap.h"
#include "CTableMapLoader.h"
#include "CTablepointChecker.h"
#include "CTableTitle.h"
#include "CTablePositioner.h"
#include "CValidator.h"
#include "DialogScraperOutput.h"
#include "MainFrm.h"
#include "MemoryLogging.h"

#include "OpenHoldem.h"
#include "..\DLLs\Files_DLL\Files.h"

CHeartbeatThread	 *p_heartbeat_thread = NULL;
CRITICAL_SECTION	 CHeartbeatThread::cs_update_in_progress;
long int			     CHeartbeatThread::_heartbeat_counter = 0;
CHeartbeatThread   *CHeartbeatThread::pParent = NULL;
CHeartbeatDelay    CHeartbeatThread::_heartbeat_delay;
COpenHoldemStarter CHeartbeatThread::_openholdem_starter;

CHeartbeatThread::CHeartbeatThread() {
	InitializeCriticalSectionAndSpinCount(&cs_update_in_progress, 4000);
  _heartbeat_counter = 0;
  // Create events
	_m_stop_thread = CreateEvent(0, TRUE, FALSE, 0);
	_m_wait_thread = CreateEvent(0, TRUE, FALSE, 0);
}

CHeartbeatThread::~CHeartbeatThread() {
	// Trigger thread to stop
	::SetEvent(_m_stop_thread);

	// Wait until thread finished
	::WaitForSingleObject(_m_wait_thread, k_max_time_to_wait_for_thread_to_shutdown);

	// Close handles
	::CloseHandle(_m_stop_thread);
	::CloseHandle(_m_wait_thread);

	DeleteCriticalSection(&cs_update_in_progress);
	p_heartbeat_thread = NULL;
}

void CHeartbeatThread::StartThread() {
	// Start thread
	write_log(Preferences()->debug_heartbeat(), "[HeartBeatThread] Starting heartbeat thread\n");
    assert(this != NULL);
	AfxBeginThread(HeartbeatThreadFunction, this);
}

UINT CHeartbeatThread::HeartbeatThreadFunction(LPVOID pParam) {
  CTablepointChecker tablepoint_checker;
	pParent = static_cast<CHeartbeatThread*>(pParam);
  assert(pParent != NULL);
	// Seed the RNG
	srand((unsigned)GetTickCount());

	while (true) {
		_heartbeat_counter++;
		write_log(Preferences()->debug_heartbeat(), "[HeartBeatThread] Starting next cycle\n");
		// Check event for stop thread
		if(::WaitForSingleObject(pParent->_m_stop_thread, 0) == WAIT_OBJECT_0) {
			// Set event
      write_log(Preferences()->debug_heartbeat(), "[HeartBeatThread] Ending heartbeat thread\n");
      LogMemoryUsage("Hc");
			::SetEvent(pParent->_m_wait_thread);
			AfxEndThread(0);
		}
    assert(p_tablemap_loader != NULL);
    LogMemoryUsage("H1");
		p_tablemap_loader->ReloadAllTablemapsIfChanged();
    LogMemoryUsage("H2");
    assert(p_autoconnector != NULL);
    write_log(Preferences()->debug_alltherest(), "[CHeartbeatThread] location Johnny_B\n");
    if (p_autoconnector->IsConnectedToGoneWindow()) {
      LogMemoryUsage("H3");
      p_autoconnector->Disconnect("table disappeared");
    }
    // Scarlet Beast: if a window-less virtual connection is up but server-scrape
    // was turned off, drop it so we can fall back to a real poker window.
    if (p_autoconnector->IsVirtualConnection()
        && (p_scarlet_beast == NULL || !p_scarlet_beast->ScrapeFromServer())) {
      p_autoconnector->Disconnect("Scarlet Beast server-scrape turned off");
    }
    LogMemoryUsage("H4");
    if (!p_autoconnector->IsConnectedToAnything()) {
      // Not connected
      AutoConnect();
    }
    // No "else" here
    // We want one fast scrape immediately after connection
    // without any heartbeat-sleeping.
    LogMemoryUsage("H5");
    write_log(Preferences()->debug_alltherest(), "[CHeartbeatThread] location Johnny_C\n");
		if (p_autoconnector->IsConnectedToExistingWindow()) {
      // The tablepoint/theme check and tablemap live-reload only make sense for a
      // real window-backed connection, not the Scarlet Beast virtual connection.
      if (!p_autoconnector->IsVirtualConnection()
          && tablepoint_checker.TablepointsMismatchedTheLastNHeartbeats()) {
        LogMemoryUsage("H6");
        p_autoconnector->Disconnect("table theme changed (tablepoints)");
      } else {
        LogMemoryUsage("H7");
        // Lightweight "settings changed" live-reload: every few beats, cheaply probe
        // the hiss DB revision and re-pull the connected map + OCR settings if the
        // trainer/Vision edited them (no disconnect, no game-state reset).
        if (!p_autoconnector->IsVirtualConnection() && (_heartbeat_counter % 8) == 0) {
          p_tablemap_loader->ReloadConnectedTablemapIfSettingsChanged();
        }
        ScrapeEvaluateAct();
      }
		}
    assert(p_watchdog != NULL);
    LogMemoryUsage("H8");
    p_watchdog->HandleCrashedAndFrozenProcesses();
    if (Preferences()->use_auto_starter()) {
      LogMemoryUsage("H9");
      _openholdem_starter.StartNewInstanceIfNeeded();
    }
    LogMemoryUsage("Ha");
    if (Preferences()->use_auto_shutdown()) {
      _openholdem_starter.CloseThisInstanceIfNoLongerNeeded();
    }
    LogMemoryUsage("Hb");
    _heartbeat_delay.FlexibleSleep();
		write_log(Preferences()->debug_heartbeat(), "[HeartBeatThread] Heartbeat cycle ended\n");
    LogMemoryUsage("End of heartbeat cycle");
	}
}

void CHeartbeatThread::ScrapeEvaluateAct() {
	// No window to keep in position for the Scarlet Beast virtual connection.
	if (!p_autoconnector->IsVirtualConnection()) {
		p_table_positioner->AlwaysKeepPositionIfEnabled();
	}
	// This critical section lets other threads know that the internal state is being updated
	EnterCriticalSection(&pParent->cs_update_in_progress);

	////////////////////////////////////////////////////////////////////////////////////////////
	// Scrape window
  p_table_title->UpdateTitle();
  write_log(Preferences()->debug_heartbeat(), "[HeartBeatThread] Calling DoScrape.\n");
  DWORD t_scrape0 = GetTickCount();
  p_lazyscraper->DoScrape();
  DWORD t_scrape_ms = GetTickCount() - t_scrape0;
  // We must not check if the scrape of the table changed, because:
  //   * some symbol-engines must be evaluated no matter what
  //   * we might need to act (sitout, ...) on empty/non-changing tables
  //   * auto-player needs stable frames too
  DWORD t_eval0 = GetTickCount();
	p_engine_container->EvaluateAll();
  DWORD t_eval_ms = GetTickCount() - t_eval0;
	// Reply-frames no longer here in the heartbeat.
  // we have a "ReplayFrameController for that.
  LeaveCriticalSection(&pParent->cs_update_in_progress);
	p_openholdem_title->UpdateTitle();
	////////////////////////////////////////////////////////////////////////////////////////////
	// Update scraper output dialog if it is present
	if (m_ScraperOutputDlg) {
		m_ScraperOutputDlg->UpdateDisplay();
	}
  
	////////////////////////////////////////////////////////////////////////////////////////////
	// OH-Validator
	write_log(Preferences()->debug_heartbeat(), "[HeartBeatThread] Calling Validator.\n");
  p_validator->Validate();

	////////////////////////////////////////////////////////////////////////////////////////////
	// Autoplayer
	write_log(Preferences()->debug_heartbeat(), "[HeartBeatThread] autoplayer_engaged(): %s\n", 
		Bool2CString(p_autoplayer->autoplayer_engaged()));
	write_log(Preferences()->debug_heartbeat(), "[HeartBeatThread] p_engine_container->symbol_engine_userchair()->userchair()_confirmed(): %s\n", 
		Bool2CString(p_engine_container->symbol_engine_userchair()->userchair_confirmed()));
	// If autoplayer is engaged, we know our chair, and the DLL hasn't told us to wait, then go do it!
	DWORD t_act0 = GetTickCount();
	if (p_autoplayer->autoplayer_engaged()) {
		write_log(Preferences()->debug_heartbeat(), "[HeartBeatThread] Calling DoAutoplayer.\n");
		p_autoplayer->DoAutoplayer();
	}
	DWORD t_act_ms = GetTickCount() - t_act0;

	// Per-cycle heartbeat timing: scrape vs symbol-engine (EvaluateAll) vs
	// validate+autoplayer. Only when heartbeat-debugging is enabled.
	if (Preferences()->debug_heartbeat()) {
		CString path = LogsDirectory() + "scrape_perf.log";
		FILE *f = fopen(path.GetString(), "a");
		if (f != NULL) {
			SYSTEMTIME st; GetLocalTime(&st);
			fprintf(f, "%02d:%02d:%02d.%03d  [heartbeat] scrape=%lu ms  evaluate=%lu ms  validate+act=%lu ms\n",
				st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
				(unsigned long)t_scrape_ms, (unsigned long)t_eval_ms, (unsigned long)t_act_ms);
			fclose(f);
		}
	}
}

void CHeartbeatThread::AutoConnect() {
  write_log(Preferences()->debug_alltherest(), "[CHeartbeatThread] location Johnny_D\n");
	assert(!p_autoconnector->IsConnectedToAnything());
	// Scarlet Beast server-scrape: connect window-lessly (no poker window needed) so
	// the heartbeat scrapes the table from poker.scarletbeast.com. Only wait until the
	// formula parser is idle so we don't connect mid-parse; we do NOT require a fully
	// parsed bot, so the table still displays even with no/invalid bot loaded.
	if (p_scarlet_beast != NULL && p_scarlet_beast->ScrapeFromServer()) {
		// Wait until the OpenPPL library is loaded (and the parser is idle) before
		// connecting. At startup the heartbeat thread is created BEFORE
		// ParseDefaultLibraries() runs; connecting too early makes
		// UpdateOnConnection's OpenPPL init-function check run against an unloaded
		// library and pop the spurious "can't find UpdateMemorySymbolsOnHandReset"
		// warning (which also blocks the main window from finishing startup).
		if (p_formula_parser != NULL && !p_formula_parser->IsParsing()
			&& p_function_collection != NULL && p_function_collection->OpenPPLLibraryLoaded()) {
			p_autoconnector->ConnectVirtual();
		}
		return;
	}
	if (Preferences()->autoconnector_when_to_connect() == k_AutoConnector_Connect_Permanent) {
		if (p_autoconnector->SecondsSinceLastFailedAttemptToConnect() > 1 /* seconds */) {
			write_log(Preferences()->debug_autoconnector(), "[CHeartbeatThread] going to call Connect()\n");
			p_autoconnector->Connect(NULL);
		}	else {
			write_log(Preferences()->debug_autoconnector(), "[CHeartbeatThread] Reconnection blocked. Other instance failed previously.\n");
		}
	}
}