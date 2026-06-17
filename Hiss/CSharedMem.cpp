//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Synchronization, mainly info about connected tables
//   for the auto-connector
//
//******************************************************************************

#include "stdafx.h"
#include "CSharedMem.h"


#include "crc32hash.h"
#include "CSessionCounter.h"
#include "COcrWorker.h"   // g_ocr_worker_mode
#include "CSymbolEngineRandom.h"
#include "..\DLLs\WindowFunctions_DLL\window_functions.h"

#define ENT CSLock lock(m_critsec);

// For an introduction about shared memory see:
//   * http://www.programmersheaven.com/mb/Win32API/156366/156366/shared-memory-and-dll/
//   * http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2006/n2044.html
//   * http://www.codeproject.com/KB/threads/SharedMemory_IPC_Threads.aspx
//   * http://www.fstsoft.de/shared.htm (german)
//
// We chose the first solution for its simplicity

///////////////////////////////////////////////////////////////////////////////////
//
// SHARED MEMORY for communication and synchronisation of OH-instances.
//
///////////////////////////////////////////////////////////////////////////////////

// Cross-instance shared state, backed by a NAMED FILE MAPPING (CreateFileMapping) so it is
// genuinely shared across every Hiss process. The legacy #pragma data_seg/.ohshmem shared-
// segment trick was NOT reliably shared across processes on this toolchain/OS (even with ASLR
// off): each instance kept a PRIVATE copy, so PokerWindowAttached() never saw another instance's
// table and two instances both grabbed the same poker window. A file mapping is the robust fix.
struct OHSharedState {
  HWND   attached_poker_windows[MAX_SESSION_IDS];                  // auto-connector: who is on what
  time_t last_failed_attempt_to_connect;
  int    session_ID_of_last_instance_that_failed_to_connect;
  HWND   dense_list_of_attached_poker_windows[MAX_SESSION_IDS];    // table positioner
  int    size_of_dense_list_of_attached_poker_windows;
  int    CRC_of_main_mutexname;
  int    openholdem_PIDs[MAX_SESSION_IDS];                         // watchdog / popup-blocker / session IDs
};

// Returns the process-shared state, lazily creating/opening the mapping on first use. Pagefile-
// backed mappings are zero-initialised by the OS, so a fresh segment starts clean (and a second
// instance that opens the existing mapping sees the first instance's writes). Magic-static init
// is thread-safe.
static OHSharedState *SharedState() {
  static OHSharedState *cached = NULL;
  if (cached != NULL) return cached;
  // "Local\\" namespace = shared within the user session (all Hiss instances run there).
  HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
    0, sizeof(OHSharedState), "Local\\HissAutoConnectorSharedState_v1");
  if (map != NULL) {
    OHSharedState *view = (OHSharedState *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(OHSharedState));
    if (view != NULL) {
      // Keep `map` open for the process lifetime (the mapping persists while any handle is open);
      // freed automatically when the process exits.
      cached = view;
      return cached;
    }
    CloseHandle(map);
  }
  // Fallback (mapping unavailable): a private zeroed copy so the bot still runs (single-instance).
  cached = (OHSharedState *)calloc(1, sizeof(OHSharedState));
  return cached;
}

// Keep the original variable names: the rest of this file references them directly; these macros
// redirect every access into the shared mapping.
#define attached_poker_windows                             (SharedState()->attached_poker_windows)
#define last_failed_attempt_to_connect                     (SharedState()->last_failed_attempt_to_connect)
#define session_ID_of_last_instance_that_failed_to_connect (SharedState()->session_ID_of_last_instance_that_failed_to_connect)
#define dense_list_of_attached_poker_windows               (SharedState()->dense_list_of_attached_poker_windows)
#define size_of_dense_list_of_attached_poker_windows       (SharedState()->size_of_dense_list_of_attached_poker_windows)
#define CRC_of_main_mutexname                              (SharedState()->CRC_of_main_mutexname)
#define openholdem_PIDs                                    (SharedState()->openholdem_PIDs)

///////////////////////////////////////////////////////////////////////////////////
//
// CLASS CSharedMem to access the shared memory
//
///////////////////////////////////////////////////////////////////////////////////

CSharedMem *p_sharedmem = NULL;

CSharedMem::CSharedMem() {
	// We can verify the mutex here,
	// because preferences have already been loaded.
	VerifyMainMutexName();
  AquireOwnProcessID();
}

CSharedMem::~CSharedMem() {
  write_log(Preferences()->debug_sharedmem(), "[CSharedMem] Terminating %d\n", p_sessioncounter->session_id());
}

void CSharedMem::AquireOwnProcessID() {
  // OCR-worker processes must NOT register a PID in the cross-instance shared
  // table: they share a fixed session_id (0) and never heartbeat, so another
  // instance's watchdog would treat them as "frozen" and kill them. Helpers are
  // managed by their own kill-on-close job object, not the watchdog.
  if (g_ocr_worker_mode) {
    write_log(Preferences()->debug_sharedmem(), "[CSharedMem] OCR worker: skipping PID registration\n");
    return;
  }
  write_log(Preferences()->debug_sharedmem(), "[CSharedMem] Aquiring own process IG\n");
  assert(p_sessioncounter != NULL);
  AssertRange(p_sessioncounter->session_id(), 0, MAX_SESSION_IDS - 1);
  int my_PID = GetCurrentProcessId();
  // Share our process ID for autostarter, watchdog and popup-blocker
  openholdem_PIDs[p_sessioncounter->session_id()] = my_PID;
  if (my_PID == 0) {
    // GetCurrentProcessId() can fail on some systems,
    // on startup or in general.
    // http://www.maxinmontreal.com/forums/viewtopic.php?f=156&t=20520
    write_log(k_always_log_errors, "ERROR getting my own process ID.\n");
    write_log(k_always_log_errors, "This might be caused by Windows XP security settings.\n");
    write_log(k_always_log_errors, "Features like the auto-starter will be temporary disabled\n");
  }
}

bool CSharedMem::PokerWindowAttached(HWND Window) {
	ENT;
	for (int i=0; i<MAX_SESSION_IDS; i++)	{
		if (attached_poker_windows[i] == Window) {
			// Window found. Already attached.
			return true;
		}
	}
	// Window not found. Not attached.
	return false;
}

bool CSharedMem::AnyWindowAttached() {
	for (int i=0; i<MAX_SESSION_IDS; i++)	{
		if (attached_poker_windows[i] != NULL) {
			return true;
		}
	}
	return false;
}

void CSharedMem::MarkPokerWindowAsAttached(HWND Window) {
  ENT;
	attached_poker_windows[p_sessioncounter->session_id()] = Window;	
}

void CSharedMem::RememberTimeOfLastFailedAttemptToConnect() {
	ENT;
	time(&last_failed_attempt_to_connect);
	write_log(Preferences()->debug_autoconnector(), "[CSharedMem] Set last_failed_attempt_to_connect %d\n", last_failed_attempt_to_connect);
	session_ID_of_last_instance_that_failed_to_connect = p_sessioncounter->session_id();
	write_log(Preferences()->debug_autoconnector(), "[CSharedMem] Instance %d failed to connect\n", session_ID_of_last_instance_that_failed_to_connect);
}

time_t CSharedMem::GetTimeOfLastFailedAttemptToConnect() {
	ENT;
	write_log(Preferences()->debug_autoconnector(), "[CSharedMem] Get last_failed_attempt_to_connect %d\n", last_failed_attempt_to_connect);
	write_log(Preferences()->debug_autoconnector(), "[CSharedMem] Stored by failed session ID: %d\n", session_ID_of_last_instance_that_failed_to_connect);
	return last_failed_attempt_to_connect;
}

HWND *CSharedMem::GetDenseListOfConnectedPokerWindows() {
	ENT;
	// Some functions like TileWindows() and CascadeWindows()
	// need a dense list of poker-tables without NULLs inbetween,
	// otherwise non-tables would be affected, too.
	//
	// See e.g. TileWindows():
	//   http://msdn.microsoft.com/en-us/library/windows/desktop/ms633554(v=vs.85).aspx
	//   lpKids [in, optional]
	//   Type: const HWND*
  //   An array of handles to the child windows to arrange. 
	//   If a specified child window is a top-level window 
	//   with the style WS_EX_TOPMOST or WS_EX_TOOLWINDOW, 
	//   the child window is not arranged. If this parameter is NULL, 
	//   all child windows of the specified parent window 
	//   (or of the desktop window) are arranged.
	CreateDenseListOfConnectedPokerWindows();
	return dense_list_of_attached_poker_windows;
}

void CSharedMem::CreateDenseListOfConnectedPokerWindows() {
	write_log(Preferences()->debug_table_positioner(), "[CSharedMem] CreateDenseListOfConnectedPokerWindows()\n");
	int size_of_list = 0;
	for (int i=0; i<MAX_SESSION_IDS; i++)	{
		if (attached_poker_windows[i] != NULL) {
			write_log(Preferences()->debug_table_positioner(), "[CSharedMem] First HWND: %i\n", attached_poker_windows[i]);
			dense_list_of_attached_poker_windows[size_of_list] = attached_poker_windows[i];
			size_of_list++;
		}
	}
	write_log(Preferences()->debug_table_positioner(), "[CSharedMem] Total tables %i\n", size_of_list);
	// Only at the very end update the size of the list.
	// This helps to avoid potential race-conditions,
	// if another application uses the list at the same time,
	// although at least the creation of the list is protected.
	// Updating the content of the list doesn't harm much,
	// but working with an incorrect size would.
	size_of_dense_list_of_attached_poker_windows = size_of_list;
}

int CSharedMem::SizeOfDenseListOfAttachedPokerWindows() {
	return size_of_dense_list_of_attached_poker_windows;
}

void CSharedMem::VerifyMainMutexName() {
	// Some people were "smart" enough to use multiple ini-files
	// with differing mutex names:
	//
	// This leads to lots of problems:
	// * autoplayer actions no longer synchronized
	// * no longer unique session ID
	// * all bots try to write to the same log-file
	int CRC = crc32((const unsigned char *) Preferences()->mutex_name());
	if (CRC_of_main_mutexname == 0)	{
		// Set the mutex
		// We have a potential race-condition here, but can't do anything.
		// But chances are very low that all instances interrupt
		// each other at the same time and same point.
		CRC_of_main_mutexname = CRC;
	}	else if (CRC_of_main_mutexname != CRC) {
		// Another instance set another CRC,
		// i.e. has another name for the main mutex
		// ==> failure.
		// We assume that the user is present at the very first
		// test-run, so we throw an interactive message, no matter what.
		MessageBox_Interactive("Incorrect mutex name.\n"
			"It is strictly necessary that all instances\n"
			"of OpenHoldem get the same main mutex name,\n"
			"otherwise very important functionality will fail:\n"
			"  * autoplayer synchronization\n"
			"  * session ID\n"
			"  * unique log-files\n"
			"Going to terminate...",
			"Mutex Error", 0);
		PostQuitMessage(-1);
	}
}

bool CSharedMem::IsAnyOpenHoldemProcess(int PID) {
	for (int i=0; i<MAX_SESSION_IDS; i++)	{
		if (openholdem_PIDs[i] == PID) {
			return true;
		}
	}
	return false;
}

int CSharedMem::LowestConnectedSessionID() {
  Dump();
  for (int i = 0; i<MAX_SESSION_IDS; ++i) {
    if (attached_poker_windows[i] != NULL) {
      return i;
    }
  }
  return kUndefined;
}

int CSharedMem::LowestUnconnectedSessionID() {
  Dump();
  write_log(Preferences()->debug_sharedmem(), "[CSharedMem] LowestUnconnectedSessionID()\n");
  for (int i = 0; i < MAX_SESSION_IDS; ++i) {
    if (openholdem_PIDs[i] == 0) {
      // ID not used (bot probably terminated)
      write_log(Preferences()->debug_sharedmem(), "[CSharedMem] ID %i not runing\n", i);
      continue;
    }
    if (attached_poker_windows[i] == NULL) {
      write_log(Preferences()->debug_sharedmem(), "[CSharedMem] ID %i not connected\n", i);
      return i;
    }
    if (!IsWindow(attached_poker_windows[i])) {
      write_log(Preferences()->debug_sharedmem(), "[CSharedMem] ID %i connected to not a window\n", i);
      return i;
    }
  }
  write_log(Preferences()->debug_sharedmem(), "[CSharedMem] LowestUnconnectedSessionID not found\n");
  return kUndefined;
}

int CSharedMem::NBotsPresent() {
  Dump();
  write_log(Preferences()->debug_sharedmem(), "[CSharedMem] NBotsPresent()\n");
  int result = 0;
  for (int i = 0; i < MAX_SESSION_IDS; ++i) {
    if (openholdem_PIDs[i] != 0) {
      write_log(Preferences()->debug_sharedmem(), "[CSharedMem] ID %i running\n", i);
      ++result;
    }
  }
  write_log(Preferences()->debug_sharedmem(), "[CSharedMem] NBotsPresent: %i\n", result);
  return result;
}

int CSharedMem::NUnoccupiedBots() {
  Dump();
  return (NBotsPresent() - NTablesConnected());
}

int CSharedMem::NTablesConnected() {
  Dump();
  int result = 0;
  for (int i = 0; i < MAX_SESSION_IDS; ++i) {
    if (attached_poker_windows[i] != 0) {
      ++result;
    }
  }
  return result;
}

//http://stackoverflow.com/questions/1238379/detecting-if-a-process-is-still-runnning
bool isProcessRunning(DWORD processID)
{
  if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processID))
  {
    DWORD exitCodeOut;
    // GetExitCodeProcess returns zero on failure
    if (GetExitCodeProcess(process, &exitCodeOut) != 0)
    {
      // Return if the process is still active
      return exitCodeOut == STILL_ACTIVE;
    }
  }
  return false;
}

bool CSharedMem::IsDeadOpenHoldemProcess(int open_holdem_iD) {
  if (openholdem_PIDs[open_holdem_iD] == NULL) {
    return false;
  }
  if (isProcessRunning(openholdem_PIDs[open_holdem_iD])) {
    return false;
  }
  write_log(Preferences()->debug_sharedmem(), "[CSharedMem] Dead process %d %d detected\n",
    open_holdem_iD, openholdem_PIDs[open_holdem_iD]);
  return true;
}

void CSharedMem::CleanUpProcessMemory(int open_holdem_iD) {
  // Clear my process ID
  openholdem_PIDs[open_holdem_iD] = 0;
  // Clear my attached window
  attached_poker_windows[open_holdem_iD] = NULL;
}

void CSharedMem::Dump() {
  if (!Preferences()->debug_sharedmem()) {
    return;
  }
  write_log(Preferences()->debug_sharedmem(), "[CSharedMem] last failed attempt to connect: %d\n", last_failed_attempt_to_connect);
  write_log(Preferences()->debug_sharedmem(), "[CSharedMem] failed session ID %d\n", session_ID_of_last_instance_that_failed_to_connect);
  write_log(Preferences()->debug_sharedmem(), "[CSharedMem] CRC of main mutex name %d\n", CRC_of_main_mutexname);
  write_log(Preferences()->debug_sharedmem(), "[CSharedMem] size of dense list %d\n", size_of_dense_list_of_attached_poker_windows);
  write_log(Preferences()->debug_sharedmem(), "[CSharedMem] ID  OpenHoldem-PID     poker-table\n");
  for (int i = 0; i < MAX_SESSION_IDS; ++i) {
    write_log(Preferences()->debug_sharedmem(), "[CSharedMem] %2d %15d %15d\n", 
      i, openholdem_PIDs[i], attached_poker_windows[i]);
  }
}

int CSharedMem::OpenHoldemProcessID(int session_ID) {
  assert(session_ID >= 0);
  assert(session_ID < MAX_SESSION_IDS);
  int process_ID = openholdem_PIDs[session_ID];
  assert(p_sessioncounter != NULL);
  if ((session_ID == p_sessioncounter->session_id())
    && (process_ID == 0)) {
    // Own process ID not available
    // Try to aquire new one
    AquireOwnProcessID();
    // Continue with new result, no matter what,
    // as some systems might fail completely.
    // Features like the auto-starter might be turned off.
    process_ID = openholdem_PIDs[session_ID];
  }
  return process_ID;
}

int CSharedMem::OpenHoldemProcessID() {
  assert(p_sessioncounter != NULL);
  int my_session_ID = p_sessioncounter->session_id();
  return openholdem_PIDs[my_session_ID];
}