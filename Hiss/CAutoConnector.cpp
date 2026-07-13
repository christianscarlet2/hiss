//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: automatically connecting to unserved poker-tables,
//   using shared memory and a mutex to synchronize with other instaces.
//
//******************************************************************************

#include "stdafx.h"
#include "CAutoConnector.h"

#include <afxwin.h>
#include "..\CTablemap\CTablemap.h"
#include "..\CTablemap\CTablemapAccess.h"
#include "..\CTablemap\CTablemapDB.h"
#include "..\CTransform\CTransform.h"
#include "CAutoplayer.h"
#include "CCasinoInterface.h"
#include "CEngineContainer.h"
#include "CFlagsToolbar.h"
#include "CHeartbeatThread.h"
#include "CIteratorThread.h"
#include "COpenHoldemTitle.h"
#include "CPokerTrackerThread.h"
#include "CPopupHandler.h"
#include "CScarletBeast.h"
#include "CScraper.h"
#include "CSharedMem.h"
#include "CTableMapLoader.h"
#include "CTableState.h"
#include "CTablePositioner.h"
#include "CVersionInfo.h"
#include "DialogScraperOutput.h"

#include "MainFrm.h"
#include "..\DLLs\WindowFunctions_DLL\window_functions.h"
#include "OpenHoldem.h"

CAutoConnector *p_autoconnector = NULL;
CArray <STableList, STableList> g_tlist; 

CAutoConnector::CAutoConnector() {
	write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] CAutoConnector()\n");
  CString MutexName = CString(Preferences()->mutex_name()) + "AutoConnector";
	_autoconnector_mutex = new CMutex(false, MutexName);
	_virtual_connection = false;
	set_attached_hwnd(NULL);
}

CAutoConnector::~CAutoConnector() {
	// Releasing the mutex in case we hold it.
	// If we don't hold it, Unlock() will "fail" silently.
	write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] ~CAutoConnector()\n");
	_autoconnector_mutex->Unlock();
	if (_autoconnector_mutex != NULL)	{
    write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] ~CAutoConnector() Deleting auto-connector-mutex\n");
		delete _autoconnector_mutex;
		_autoconnector_mutex = NULL;
	}
	write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] ~CAutoConnector() Marking table as not atached\n");
	set_attached_hwnd(NULL);
	write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] ~CAutoConnector() Finished\n");
}

bool CAutoConnector::IsConnectedToAnything() {
  HWND table = attached_hwnd();
  bool result = (table != NULL);
  { static int s_ic_last = -1; if ((int)result != s_ic_last) { s_ic_last = (int)result; write_log(Preferences()->debug_autoconnector(), 
    "[CAutoConnector] IsConnectedToAnything: %s\n",
    Bool2CString(result)); } }
	return result;
}

bool CAutoConnector::IsConnectedToExistingWindow() {
  if (!IsConnectedToAnything()) {
    return false;
  }
  HWND table = attached_hwnd();
  bool result = IsWindow(table);
  // Log only on CHANGE. This fires ~15x/second and was the single largest source of log flood:
  // 65,487 lines in one session, all of them saying "yes, still connected". The change-only dedup
  // was already added to IsConnectedToAnything() six lines above -- it was simply never applied to
  // this sibling, so the flood survived the fix that was meant to kill it.
  { static int s_iw_last = -1;
    if ((int)result != s_iw_last) {
      s_iw_last = (int)result;
      write_log(Preferences()->debug_autoconnector(),
        "[CAutoConnector] IsConnectedToexistingWindow: %s\n",
        Bool2CString(result));
    } }
  return result;
}

bool CAutoConnector::IsConnectedToGoneWindow() {
  if (!IsConnectedToAnything()) {
    return false;
  }
  if (IsConnectedToExistingWindow()) {
    return false;
  }
  write_log(Preferences()->debug_autoconnector(), 
    "[CAutoConnector] IsConnectedToGoneWindow: true\n");
  return true;
}

void CAutoConnector::Check_TM_Against_All_Windows_Or_TargetHWND(int tablemap_index, HWND targetHWnd) {
	// ONE line per enumeration pass (not per window -- see EnumProcTopLevelWindowList below).
	write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Check_TM_Against_All_Windows(..) tablemap nr. %d\n", tablemap_index);
  if (targetHWnd == NULL) {
		EnumWindows(EnumProcTopLevelWindowList, (LPARAM) tablemap_index);
  } else {
		EnumProcTopLevelWindowList(targetHWnd, (LPARAM) tablemap_index);
  }
}

void CAutoConnector::CheckIfWindowMatchesMoreThanOneTablemap(HWND hwnd) {
  // In OpenHoldem 9.0.1 we replaced clientsize in favour of clientsizemin/max
  // and targetsize. This caused the problem that some people used a very large
  // range of clientsizemin/max in combination with a very common titletext
  // like "Poker". As a consequence some tablemaps connected to nearly every
  // window (not even table).
  // To solve this problem we now detect if a table could be served 
  // by more than one tablemap. For performance reasons we do this exactly once
  // per table at connection.
  // http://www.maxinmontreal.com/forums/viewtopic.php?f=110&t=19407&start=90#p138038
  int num_loaded_tablemaps = p_tablemap_loader->NumberOfTableMapsLoaded();
  int num_matching_tablemaps = 0;
  CString matching_tablemaps = "";
  for (int i=0; i<num_loaded_tablemaps; ++i) {
    // See if it matches the specified table map
    if (Check_TM_Against_Single_Window(i, hwnd)) {
      ++num_matching_tablemaps;
      // Build "list" of matching tablemaps
      matching_tablemaps += p_tablemap_loader->GetTablemapPathToLoad(i);
      matching_tablemaps += "\n";
    }
  }
  if (num_matching_tablemaps > 1) {
    char text[MAX_WINDOW_TITLE] = { 0 };
    GetWindowText(hwnd, text, sizeof(text));
    CString title = text;
    CString error_message;
    error_message.Format("%s%s%s%s%s%s%s%s%s",
      "More than one tablemap fits to the same table\n\n",
      matching_tablemaps,
      "\nTable: ", 
      title,
      "\n\nThese tablemaps need to be adapted:\n",
      "  * clientsizemin/max\n",
      "  * titletext(s)\n",
      "  * and/or tablepoints\n",
      "to make the tablemap-selection-process unambiguous.");
    MessageBox_Error_Warning(error_message);
  }
}

void CAutoConnector::set_attached_hwnd(const HWND table) {
  CSLock lock(m_critsec);
  _attached_hwnd = table;
  assert(p_sharedmem != NULL);
  p_sharedmem->MarkPokerWindowAsAttached(table);
}

BOOL CALLBACK EnumProcTopLevelWindowList(HWND hwnd, LPARAM lparam) {
	CString			title = "", winclass = "";
	char				text[MAX_WINDOW_TITLE] = {0};
	RECT				crect = {0};
	STableList	tablelisthold;
	int					tablemap_index = (int)(lparam);

  // NO per-window logging here. This callback fires once for EVERY top-level window on the desktop
  // (~430 of them) x every tablemap, and the autoconnector re-runs ~1x/second while disconnected.
  // Three unconditional lines here cost ~1700 log lines per connect attempt = ~61 KB/s = ~5 GB/day;
  // that is what grew oh_1.log to 15 GB. They carried no information anyway (no hwnd, no title), and
  // the two lines they duplicated already exist: the enumeration pass is logged ONCE by the caller
  // (Check_TM_Against_All_Windows_Or_TargetHWND), and every REAL candidate is logged WITH its hwnd
  // below ("Adding window candidate to the list" / "already served").
  if (!IsWindowVisible(hwnd)) {
    return true;
  }
  // Since OH 11.1.0 We do no longer check for (GetParent(hwnd) != NULL)
  // because we want OpenHoldem to be able to connect to popups
  // e.g. to click a confirmation-button
  // or maybe even do more complicated hopper-tasks in the future.
	// See if it matches the currently loaded table map
  if (Check_TM_Against_Single_Window(tablemap_index, hwnd)) { 
		// Filter out served tables already here,
		// otherwise the other list used in the dialog
		// to select windows manually will cause us lots of headaches,
		// as the lists will be of different size 
		// and the indexes will not match.
    if (p_sharedmem->PokerWindowAttached(hwnd)) {
      write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Window candidate already served: [%d]\n", hwnd);
    } else if (p_popup_handler->WinIsOpenHoldem(hwnd)) {
      write_log(Preferences()->debug_popup_blocker(), "[CAutoConnector] Window belongs to Hiss\n");
		}	else {
			write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Adding window candidate to the list: [%d]\n", hwnd);
			tablelisthold.hwnd = hwnd;
      tablelisthold.tablemap_index = tablemap_index;
			g_tlist.Add(tablelisthold);
		}
	}
  return true;  // keep processing through entire list of windows
}

void CAutoConnector::WriteLogTableReset(CString event_and_reason) {
  // Log a new connection, plus the version-info
  // (because of all the guys who report "bugs" of outdated versions)
	write_log(k_always_log_basic_information,
		"\n"
		"==============================================\n"
		"%s\n"
    "==============================================\n"
		"%s"    // Version info already contains a newline
		"==============================================\n",
    event_and_reason,
		p_version_info->GetVersionInfo());
}

void CAutoConnector::FailedToConnectBecauseNoWindowInList() {
	p_sharedmem->RememberTimeOfLastFailedAttemptToConnect();
	GoIntoPopupBlockingMode();
}

void CAutoConnector::FailedToConnectProbablyBecauseAllTablesAlreadyServed() {
	write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Attempt to connect did fail\n");
	p_sharedmem->RememberTimeOfLastFailedAttemptToConnect();
	GoIntoPopupBlockingMode();
}

void CAutoConnector::GoIntoPopupBlockingMode() {
	// We have a free instance that has nothing to do.
	// Care about potential popups here, once per auto-connector-heartbeat.
	write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Not connected. Going into popup-blocking mode.\n");
	if (p_sharedmem->AnyWindowAttached())	{
		// Only handle popups if at least one bot is connected to a table.
		// Especially stop popup-handling if the last table got closed
		// to allow "normal" human work again.
		assert(p_popup_handler != NULL);
		p_popup_handler->HandleAllWindows();
	} else {
    write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] No table connected at all. No need for popup-blocking.\n");
  }
}

bool CAutoConnector::Connect(HWND targetHWnd) {
	int					line = 0, ret = 0;
	char				title[MAX_WINDOW_TITLE] = {0};
	int					SelectedItem = kUndefined;
	CString			current_path = "";
	BOOL				bFound = false;
  // Potential race-condition, as some objects
  // (especially GUI objects) get created by another thread.
  // We just skip connection if OH is not yet initialized.
  // http://www.maxinmontreal.com/forums/viewtopic.php?f=156&t=19706
  // 
  // We have to check and return very early, we must not do this
  // after locking the mutex, otherwiese we block other instances forever.
  // http://www.maxinmontreal.com/forums/viewtopic.php?f=110&t=19407&p=140417#p140417
  if (p_table_positioner == NULL) return false;
  if (p_autoplayer == NULL) return false;
  if (p_casino_interface == NULL) return false;
  if (p_engine_container == NULL) return false;
  if (p_flags_toolbar == NULL) return false;
  if (p_scraper == NULL) return false;
  if (p_sharedmem == NULL) return false;
  if (p_tablemap == NULL) return false;
  if (p_tablemap_loader == NULL) return false;
  if (p_table_state == NULL) return false;
  if (p_table_positioner == NULL) return false;
	write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Connect(..)\n");
  ASSERT(_autoconnector_mutex->m_hObject != NULL); 
	write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Locking autoconnector-mutex\n");
	if (!_autoconnector_mutex->Lock(500))	{
		write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Could not grab mutex; early exit\n");
		return false; 
	}
  // Clear global list for holding table candidates
	g_tlist.RemoveAll();
	write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Number of tablemaps loaded: %i\n",
    p_tablemap_loader->NumberOfTableMapsLoaded());
	for (int tablemap_index=0; tablemap_index<p_tablemap_loader->NumberOfTableMapsLoaded(); tablemap_index++) {
		write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Going to check TM nr. %d out of %d\n", 
			tablemap_index, p_tablemap_loader->NumberOfTableMapsLoaded());
		Check_TM_Against_All_Windows_Or_TargetHWND(tablemap_index, targetHWnd);
	}
	// Put global candidate table list in table select dialog variables
	int n_window_candidates = (int) g_tlist.GetSize();
  write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Number of table candidates: %i\n", 
    n_window_candidates);
	if (n_window_candidates == 0) {
		FailedToConnectBecauseNoWindowInList();
	}	else 	{
		SelectedItem = SelectTableMapAndWindowAutomatically();
		if (SelectedItem == kUndefined) {
			FailedToConnectProbablyBecauseAllTablesAlreadyServed();
		}	else {
			write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Window [%d] selected\n", g_tlist[SelectedItem].hwnd);
      // Load correct tablemap, and save hwnd/rect/numchairs of table that we are "attached" to
			set_attached_hwnd(g_tlist[SelectedItem].hwnd);
      CheckIfWindowMatchesMoreThanOneTablemap(attached_hwnd());
			assert(p_tablemap != NULL);
      CString tablemap_to_load = p_tablemap_loader->GetTablemapPathToLoad(g_tlist[SelectedItem].tablemap_index);
			write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Selected tablemap: %s\n", tablemap_to_load);
			p_tablemap_db->LoadTablemapFromDB(tablemap_to_load, p_tablemap);
			write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Tablemap successfully loaded\n");
  		// Create bitmaps
			p_scraper->CreateBitmaps();
			write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Scraper-bitmaps created\n");
      // Clear scraper fields
			p_table_state->Reset();
      p_casino_interface->Reset();
			write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Table state cleared\n");
      // Reset symbols
			p_engine_container->UpdateOnConnection();
      write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] UpdateOnConnection executed (during connection)\n");
			write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Going to continue with scraper output and scraper DLL\n");
      // Reset "ScraperOutput" dialog, if it is live
			if (m_ScraperOutputDlg) {
				m_ScraperOutputDlg->Reset();
			}
			p_flags_toolbar->ResetButtonsOnConnect();
      // The main GUI gets created by another thread.
      // This can be slowed down if there are popups (parse-errors).
      // Handle the race-condition
      WAIT_FOR_CONDITION(PMainframe() != NULL)
      assert(PMainframe() != NULL);
			// Reset display
			PMainframe()->ResetDisplay();
      // log OH title bar text and table reset
      WriteLogTableReset("NEW CONNECTION");
      // Leave the connected window EXACTLY where it is on connect: do NOT resize,
      // reposition, or restore a saved placement (any of which could shove a
      // scrcpy/phone-mirror window onto monitor 1). This holds regardless of the
      // "engage autoplayer on startup" preference, which uses this same connect path.
			p_autoplayer->EngageAutoPlayerUponConnectionIfNeeded();
		}
	}
	write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Unlocking autoconnector-mutex\n");
	_autoconnector_mutex->Unlock();
	return (SelectedItem != kUndefined);
}

// Scarlet Beast server-scrape: window-less connection. We attach to the desktop
// window (always a valid HWND, so IsConnectedToExistingWindow() stays true and the
// heartbeat keeps scraping) and run only the lifecycle reset that EvaluateAll needs.
// No tablemap is loaded; the seat count and table state come from the server payload
// each heartbeat (CScraper::ScrapeFromScarletBeastServer).
bool CAutoConnector::ConnectVirtual() {
  if (p_engine_container == NULL) return false;
  if (p_table_state == NULL) return false;
  if (p_casino_interface == NULL) return false;
  if (p_sharedmem == NULL) return false;
  if (IsConnectedToAnything()) return true;
  if (!_autoconnector_mutex->Lock(500)) return false;
  write_log(k_always_log_basic_information,
    "[CAutoConnector] Scarlet Beast: establishing window-less virtual connection\n");
  _virtual_connection = true;
  // Desktop window: always valid, never "gone", and not a poker window we'd act on.
  set_attached_hwnd(::GetDesktopWindow());
  p_table_state->Reset();
  p_casino_interface->Reset();
  p_engine_container->UpdateOnConnection();
  WAIT_FOR_CONDITION(PMainframe() != NULL)
  if (PMainframe() != NULL) PMainframe()->ResetDisplay();
  WriteLogTableReset("VIRTUAL CONNECTION (Scarlet Beast server-scrape)");
  _autoconnector_mutex->Unlock();
  // Auto-engage the autoplayer if the user enabled that preference, just like a
  // real connection -- so "bot connected" actually means the bot is acting.
  p_autoplayer->EngageAutoPlayerUponConnectionIfNeeded();
  return true;
}

void CAutoConnector::Disconnect(CString reason_for_disconnection) {
	write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Disconnect()\n");
  bool was_virtual = _virtual_connection;
  _virtual_connection = false;
  if (!IsConnectedToAnything()) {
    // Be extra safe.
    // This stupid error happened, when OnTimer() only checked if the window 
    // still existed, but not if we were connected at all.
    // Then Diconnect() plus Connect() lead to freezing.
    write_log(k_always_log_errors, "[CAutoConnector] ERROR: Disconnect() called while not connected\n");
    return;
  }
  // First close scraper-output-dialog,
  // as an updating dialog without a connected table can crash.
  CDlgScraperOutput::DestroyWindowSafely();
  // Make sure autoplayer is off
  write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Stopping autoplayer\n");
  p_autoplayer->EngageAutoplayer(false);
	// Wait for mutex - "forever" if necessary, as we have to clean up.
	ASSERT(_autoconnector_mutex->m_hObject != NULL); 
	write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Locking autoconnector-mutex\n");
  _autoconnector_mutex->Lock(INFINITE); 
	p_engine_container->UpdateOnDisconnection();
	// Remember where/how big the table window is, so the next connection restores it
	// (shared DB) instead of forcing it onto monitor 1. Skipped for the window-less
	// virtual connection (the "window" is the desktop; there is nothing to save).
	if (!was_virtual) {
		p_table_positioner->SaveCurrentPlacement();
	}
	// Clear "attached" info
	set_attached_hwnd(NULL);
	// Unattach OH.
	p_flags_toolbar->UnattachOHFromPokerWindow();
	p_flags_toolbar->ResetButtonsOnDisconnect();
	// Release mutex as soon as possible, after critical work is done
	write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Unlocking autoconnector-mutex\n");
	_autoconnector_mutex->Unlock();	
	// Delete bitmaps
	p_scraper->DeleteBitmaps();
  // Clear scraper fields
	p_table_state->Reset();
  p_casino_interface->Reset();
	// Reset symbols
	p_engine_container->UpdateOnConnection();
	write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] UpdateOnConnection executed (disconnection)\n");
	write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Going to continue with window title\n");
	// Change window title
	p_openholdem_title->UpdateTitle();
	// Reset Display
	//
	// NULL-GUARDED. PMainframe() is theApp.m_pMainWnd, which is NULL while the app is tearing down (and
	// before the main window exists). Disconnect() runs on the HEARTBEAT thread, so when the table window
	// disappears at the same moment the app is closing, this dereferenced NULL and took the whole process
	// down: 0xC0000005 in CAutoConnector::Disconnect (crash_hiss_6340, 2026-07-13 12:58:03) -- both
	// instances died on a plain table-window-gone disconnect, mid-tournament, with no other symptom.
	// The connect path at SelectTableMapAndWindowAutomatically() already guards the identical call; the
	// disconnect path never did. UpdateTitle() above is guarded internally for the same reason.
	if (PMainframe() != NULL) {
		PMainframe()->ResetDisplay();
	}
	// Reset "ScraperOutput" dialog, if it is live
	if (m_ScraperOutputDlg)	{
		m_ScraperOutputDlg->Reset();
	}
  CString message;
  message.Format("DISCONNECTION -- %s", reason_for_disconnection);
	WriteLogTableReset(message);
	write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Disconnect done\n");
}

int CAutoConnector::SelectTableMapAndWindowAutomatically() {
	write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] SelectTableMapAndWindowAutomatically(..)\n");
  int n_window_candidates = (int)g_tlist.GetSize();
	for (int i=0; i<n_window_candidates; ++i) {
		if (!p_sharedmem->PokerWindowAttached(g_tlist[i].hwnd))	{
			write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] Chosen (table, TM)-pair in list: %d\n", i);
			return i;
		}
	}
	// No appropriate table found
	write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] No appropriate table found.\n");
	return kUndefined;
}

double CAutoConnector::SecondsSinceLastFailedAttemptToConnect() {
	time_t last_failed_attempt_to_connect = p_sharedmem->GetTimeOfLastFailedAttemptToConnect(); 
	time_t CurrentTime;
	time(&CurrentTime);
	double _TimeSincelast_failed_attempt_to_connect = difftime(CurrentTime, last_failed_attempt_to_connect);
	{ static DWORD s_thr_since = 0; DWORD now_s_thr_since = ::GetTickCount(); if (now_s_thr_since - s_thr_since > 5000) { s_thr_since = now_s_thr_since; write_log(Preferences()->debug_autoconnector(), "[CAutoConnector] TimeSincelast_failed_attempt_to_connect %f\n", _TimeSincelast_failed_attempt_to_connect); } }
	return _TimeSincelast_failed_attempt_to_connect;
}
// flood-throttle applied
