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
#include "CAutoplayerButton.h"
#include "CAutoplayerFunctions.h"
#include "CCasinoInterface.h"
#include "..\CTablemap\CTablemap.h"
#include "CTwoSuccessiveClicks.h"
#include "CSymbolEngineAutoplayer.h"
#include "CursorRestore.h"
#include "CBetroundCalculator.h"
#include "CHeartbeatDelay.h"
#include "CEngineContainer.h"
#include "ChatTerminalServer.h"   // RefreshSymbolSnapshot() -- /api/symbols is served from a
                                  // heartbeat-published snapshot, never evaluated on the HTTP thread
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
#include "CSharedMem.h"   // PokerWindowAttached(), to explain a failed manual pin
#include "CHandresetDetector.h"
#include "HudManager.h"
#include "HudOverlayWindow.h"
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

// ---- Managed child-process launch -----------------------------------------------------------
// Drivers (nn_driver.py / ultra_mode.py) are launched into a kill-on-close JOB OBJECT so that if
// Hiss exits or is killed they die WITH it instead of orphaning. Orphaned drivers from prior runs
// kept hitting /api/action and caused multiple competing bet-drivers after restarts. (Same idea as
// the OCR-worker job.)
static HANDLE EnsureDriverJob() {
  static HANDLE job = NULL;
  if (job == NULL) {
    job = CreateJobObject(NULL, NULL);
    if (job != NULL) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
      ZeroMemory(&jeli, sizeof(jeli));
      jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }
  }
  return job;
}
// Launch a new-console child assigned to the kill-on-close job. Returns its process HANDLE (NULL on
// failure). Created suspended so it is inside the job before it runs and can spawn anything.
// Mirror a launched daemon's stdout/stderr into the Terminal CHAT window. [Emrald: route the
// superstition/666-omen, nn-driver and ULTRA output to the Terminal chat window.]
extern void ChatTerminalAppendToScreen(CString screen, int section, CString text);
struct DaemonPipeCtx { HANDLE rd; };
static DWORD WINAPI DaemonPipeReaderThread(LPVOID param) {
  DaemonPipeCtx *ctx = (DaemonPipeCtx *)param;
  char buf[1024];
  DWORD n = 0;
  CString pending;
  while (ReadFile(ctx->rd, buf, sizeof(buf) - 1, &n, NULL) && n > 0) {
    buf[n] = '\0';
    pending += CString(buf);
    int nl;
    while ((nl = pending.Find('\n')) >= 0) {
      CString line = pending.Left(nl);
      line.TrimRight('\r');
      pending = pending.Mid(nl + 1);
      if (!line.IsEmpty()) ChatTerminalAppendToScreen("main", 3 /*kChatTerminalChat*/, line + "\r\n");
    }
  }
  if (!pending.IsEmpty()) ChatTerminalAppendToScreen("main", 3, pending + "\r\n");
  CloseHandle(ctx->rd);
  delete ctx;
  return 0;
}

static void *LaunchManagedConsole(const char *command, const char *cwd) {
  char buf[512];
  strncpy_s(buf, sizeof(buf), command, _TRUNCATE);
  // Capture the child's stdout+stderr through a pipe so a reader thread can mirror it into the Terminal
  // CHAT window, and launch with CREATE_NO_WINDOW so a console NEVER pops up (not even briefly). [Emrald]
  SECURITY_ATTRIBUTES sa; ZeroMemory(&sa, sizeof(sa));
  sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE; sa.lpSecurityDescriptor = NULL;
  HANDLE rd = NULL, wr = NULL;
  if (!CreatePipe(&rd, &wr, &sa, 0)) { rd = NULL; wr = NULL; }
  if (rd != NULL) SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);  // keep our read end private
  STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
  if (wr != NULL) {
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = NULL;
  }
  PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
  BOOL ok = CreateProcessA(NULL, buf, NULL, NULL, (wr != NULL),  // inherit handles only if piping
                           CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, cwd, &si, &pi);
  if (wr != NULL) CloseHandle(wr);   // the child holds the write end now
  if (!ok) { if (rd != NULL) CloseHandle(rd); return NULL; }
  if (rd != NULL) {
    DaemonPipeCtx *ctx = new DaemonPipeCtx(); ctx->rd = rd;
    HANDLE th = CreateThread(NULL, 0, DaemonPipeReaderThread, ctx, 0, NULL);
    if (th != NULL) CloseHandle(th); else { CloseHandle(rd); delete ctx; }
  }
  HANDLE job = EnsureDriverJob();
  if (job != NULL) AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  return (void *)pi.hProcess;
}

// ---- persisted mode-state across restarts (HKCU\Software\ScarletBeast) -----------------------
// ULTRA + superstition engaged-state survives a Hiss restart [Emrald: "make ultra mode and the
// superstition mode persist on restart"], re-applied once the terminal port is bound (see the
// one-time restore in ScrapeEvaluateAct).
static DWORD ReadModeReg(const char *value, DWORD def) {
  HKEY k; DWORD out = def, sz = sizeof(DWORD), type = 0;
  if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\ScarletBeast", 0, KEY_READ, &k) == ERROR_SUCCESS) {
    RegQueryValueExA(k, value, NULL, &type, (LPBYTE)&out, &sz);
    RegCloseKey(k);
  }
  return out;
}
static void WriteModeReg(const char *value, DWORD data) {
  HKEY k;
  if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\ScarletBeast", 0, NULL, 0,
                      KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
    RegSetValueExA(k, value, 0, REG_DWORD, (const BYTE*)&data, sizeof(data));
    RegCloseKey(k);
  }
}

// Persist the user's AUTOPLAYER intent so a restart comes back the way they left it.
//
// Deliberately NOT called from CAutoplayer::EngageAutoplayer(). Most callers of that are automatic
// SAFETY disengages -- the autoconnector losing the window, a formula error, a validator trip, the
// panic hotkey, opening the formula editor. Persisting from there would record "off" every time the
// bot protected itself and the user would never get their setting back. Only a deliberate toggle
// (the toolbar button, or /api/autoplayer) states an intent worth remembering.
void PersistAutoplayerIntent(bool on) {
  WriteModeReg("AutoplayerEngaged", on ? 1 : 0);
}

// ---- NN driver engage/disengage (mutually exclusive with the autoplayer) --------------------
// Engaging launches python nn_driver.py aimed at THIS instance's terminal port (in its own
// console so its decisions stay visible) and disengages the autoplayer; disengaging kills it.
static void *g_nn_driver_proc = NULL;   // HANDLE of the launched nn_driver.py (NULL = none)
static void ApplyNNDriverEngage(bool want_on) {
  WriteModeReg("NNDriverEngaged", want_on ? 1 : 0);   // persist intent across restarts [Emrald]
  if (want_on) {
    if (g_nn_driver_engaged) return;
    // NEW DRIVER MODEL [Emrald]: NN and the autoplayer are NOT mutually exclusive any more. The
    // autoplayer stays ENGAGED as the always-on executor; on NLH the NN bypasses the OHF read
    // (DoAutoplayer defers the primary action to the NN), and the autoplayer still runs the OHF on
    // PLO/PLO8 and whenever the NN is off. So do NOT disengage the autoplayer when the NN engages.
    int port = (g_terminal_port > 0) ? g_terminal_port : 27654;
    char cmd[512];
    sprintf_s(cmd, sizeof(cmd),
      "\"C:\\Users\\scarl\\AppData\\Local\\Programs\\Python\\Python310\\python.exe\" "
      "\"C:\\www\\openholdembot_old\\mcp\\nn_driver.py\" --bot-url http://127.0.0.1:%d", port);
    g_nn_driver_proc = LaunchManagedConsole(cmd, "C:\\www\\openholdembot_old\\mcp");
    if (g_nn_driver_proc != NULL) {
      g_nn_driver_engaged = true;
      write_log(k_always_log_basic_information, "[NN] driver engaged on port %d\n", port);
    } else {
      g_nn_driver_engaged = false;
      write_log(k_always_log_basic_information, "[NN] driver launch FAILED (err %lu)\n", GetLastError());
    }
  } else {
    if (g_nn_driver_proc != NULL) {
      TerminateProcess((HANDLE)g_nn_driver_proc, 0);
      CloseHandle((HANDLE)g_nn_driver_proc);
      g_nn_driver_proc = NULL;
    }
    g_nn_driver_engaged = false;
    write_log(k_always_log_basic_information, "[NN] driver disengaged\n");
  }
}

// ---- ULTRA mode engage/disengage ------------------------------------------------------------
// ULTRA launches ultra_mode.py aimed at THIS instance's terminal port. That daemon samples the
// system-audio average and randomly flips the bot between OHF (autoplayer) and NN (nn_driver)
// via /api/autoplayer + /api/nn-driver -- so ULTRA does not touch the mode itself, it delegates.
// Disengaging kills the daemon, leaving whatever mode was last selected.
static void *g_ultra_proc = NULL;
static void ApplyUltraEngage(bool want_on) {
  WriteModeReg("UltraEngaged", want_on ? 1 : 0);   // persist intent across restarts [Emrald]
  if (want_on) {
    if (g_ultra_engaged) return;
    int port = (g_terminal_port > 0) ? g_terminal_port : 27654;
    char cmd[512];
    sprintf_s(cmd, sizeof(cmd),
      "\"C:\\Users\\scarl\\AppData\\Local\\Programs\\Python\\Python310\\python.exe\" "
      "\"C:\\www\\openholdembot_old\\mcp\\ultra_mode.py\" --bot-url http://127.0.0.1:%d", port);
    g_ultra_proc = LaunchManagedConsole(cmd, "C:\\www\\openholdembot_old\\mcp");
    if (g_ultra_proc != NULL) {
      g_ultra_engaged = true;
      write_log(k_always_log_basic_information, "[ULTRA] mode engaged on port %d\n", port);
    } else {
      g_ultra_engaged = false;
      write_log(k_always_log_basic_information, "[ULTRA] launch FAILED (err %lu)\n", GetLastError());
    }
  } else {
    if (g_ultra_proc != NULL) {
      TerminateProcess((HANDLE)g_ultra_proc, 0);
      CloseHandle((HANDLE)g_ultra_proc);
      g_ultra_proc = NULL;
    }
    g_ultra_engaged = false;
    write_log(k_always_log_basic_information, "[ULTRA] mode disengaged\n");
  }
}

// ---- Superstition / Omen mode engage/disengage ----------------------------------------------
// Launches the 666 Card Oracle (card_oracle.py --superstition --speak) aimed at THIS instance's
// terminal port. In --superstition it feeds the omen resonance to /api/beast -> the sb_beastfavor
// symbol, so the OHF chases draws when the Beast favors; --speak voices strong omens (feedback).
// The toolbar/React button toggles it; disengaging kills the daemon (kill switch) and sb_beastfavor
// auto-stales to 0 within ~15s, so superstition cleanly self-disables.
static void *g_superstition_proc = NULL;
static void ApplySuperstitionEngage(bool want_on) {
  WriteModeReg("SuperstitionEngaged", want_on ? 1 : 0);   // persist intent across restarts [Emrald]
  if (want_on) {
    if (g_superstition_engaged) return;
    int port = (g_terminal_port > 0) ? g_terminal_port : 27654;
    char cmd[512];
    sprintf_s(cmd, sizeof(cmd),
      "\"C:\\Users\\scarl\\AppData\\Local\\Programs\\Python\\Python310\\python.exe\" "
      "\"C:\\www\\openholdembot_old\\mcp\\card_oracle.py\" --bot-url http://127.0.0.1:%d "
      "--superstition --speak", port);
    g_superstition_proc = LaunchManagedConsole(cmd, "C:\\www\\openholdembot_old\\mcp");
    if (g_superstition_proc != NULL) {
      g_superstition_engaged = true;
      write_log(k_always_log_basic_information, "[SUPERSTITION] engaged on port %d (666 Card Oracle feeding OHF)\n", port);
    } else {
      g_superstition_engaged = false;
      write_log(k_always_log_basic_information, "[SUPERSTITION] launch FAILED (err %lu)\n", GetLastError());
    }
  } else {
    if (g_superstition_proc != NULL) {
      TerminateProcess((HANDLE)g_superstition_proc, 0);
      CloseHandle((HANDLE)g_superstition_proc);
      g_superstition_proc = NULL;
    }
    g_superstition_engaged = false;
    write_log(k_always_log_basic_information, "[SUPERSTITION] disengaged (kill switch)\n");
  }
}

// Keep g_superstition_engaged HONEST: if the 666 Card Oracle process exited on its own (crash / closed /
// out of audio), clear the engaged flag so /api/superstition + the React table-view + Synapse-tab indicators
// reflect the REAL state instead of staying stuck ON. Called every heartbeat. [Emrald: superstition
// indicator must poll/reflect its actual state]
static void SyncSuperstitionLiveness() {
  if (!g_superstition_engaged || g_superstition_proc == NULL) return;
  if (WaitForSingleObject((HANDLE)g_superstition_proc, 0) == WAIT_OBJECT_0) {
    CloseHandle((HANDLE)g_superstition_proc);
    g_superstition_proc = NULL;
    g_superstition_engaged = false;
    WriteModeReg("SuperstitionEngaged", 0);
    write_log(k_always_log_basic_information, "[SUPERSTITION] oracle process exited -> engaged=false (indicator synced)\n");
  }
}

CHeartbeatThread	 *p_heartbeat_thread = NULL;
CRITICAL_SECTION	 CHeartbeatThread::cs_update_in_progress;
volatile LONG      CHeartbeatThread::cs_update_ready = 0;   // see CHeartbeatThread.h
volatile LONG      CHeartbeatThread::cs_update_users = 0;   // in-flight holders; dtor drains before delete
long int			     CHeartbeatThread::_heartbeat_counter = 0;
CHeartbeatThread   *CHeartbeatThread::pParent = NULL;
CHeartbeatDelay    CHeartbeatThread::_heartbeat_delay;
COpenHoldemStarter CHeartbeatThread::_openholdem_starter;

CHeartbeatThread::CHeartbeatThread() {
	InitializeCriticalSectionAndSpinCount(&cs_update_in_progress, 4000);
	InterlockedExchange(&cs_update_ready, 1);   // only NOW may off-thread users (e.g. /api/symbols) take it
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

	// Close the gate BEFORE destroying the lock, so an in-flight /api/symbols on the HTTP thread
	// can't enter a critical section that is about to be (or has been) deleted.
	InterlockedExchange(&cs_update_ready, 0);
	// ...then WAIT for anyone already past the gate to finish. The WaitForSingleObject above is
	// bounded at 5s, so it returns while a stalled heartbeat is still mid-cycle; deleting the lock
	// under it is what crashed the process (crash_hiss_17224). Closing the gate alone does not help
	// a thread that tested it before the close. Drain, then destroy.
	for (int spins = 0; spins < 300; ++spins) {
		if (InterlockedCompareExchange(&cs_update_users, 0, 0) == 0) break;
		Sleep(10);                                  // up to 3s; then fall through rather than hang exit
	}
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
          // PLO auto-detect: hot-swap to the "<name>_omaha" tablemap when the scraped text says
          // Omaha (and back to the Hold'em map otherwise). No-op unless an _omaha variant exists.
          p_tablemap_loader->SwitchTablemapForGameTypeIfNeeded();
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

// ---- Seat status ------------------------------------------------------------------------------
// Decide whether the hero is genuinely SEATED at a table, merely OBSERVING one, or NOT AT A TABLE
// at all (lobby, home screen, a crashed app, a phone that wandered into another app).
//
// The consumer restarts the poker client when this reads not_at_table, so a false negative is
// expensive and a false positive is worse. Two defences:
//   1. SEVERAL INDEPENDENT SIGNALS. No single one is trusted. In the lobby the scraper still emits
//      a table string -- observed: "antCliedog|EE.1." -- so the identity check demands a numeric
//      tourney id or a substantial table name, and a verdict needs corroborating seats and blinds.
//   2. DEBOUNCE. The raw verdict is recomputed every heartbeat, but g_seat_since_tick records when
//      the CURRENT verdict first appeared. Callers wait for it to have held a while, so a single
//      bad scrape (or the gap between hands) can never trigger a restart.
static bool SeatIdentityLooksReal(const CString &identity) {
  // "35534662|50GTDFreerollPLO6MaxTable1." -> real. "antCliedog|EE.1." -> lobby noise.
  int bar = identity.Find('|');
  if (bar > 0) {
    CString tourney = identity.Left(bar);
    bool all_digits = (tourney.GetLength() >= 4);
    for (int i = 0; i < tourney.GetLength() && all_digits; ++i) {
      if (!isdigit((unsigned char)tourney[i])) all_digits = false;
    }
    if (all_digits) return true;              // a numeric tourney id is the strongest signal
  }
  // No tourney id (cash game, or the id region misread): fall back to demanding a table name with
  // real substance, which lobby noise like "EE.1." cannot fake.
  CString tail = (bar >= 0) ? identity.Mid(bar + 1) : identity;
  int alnum = 0;
  for (int i = 0; i < tail.GetLength(); ++i) {
    if (isalnum((unsigned char)tail[i])) ++alnum;
  }
  return (alnum >= 12);
}

void CHeartbeatThread::UpdateSeatStatus() {
  long ev = 0;
  if (SeatIdentityLooksReal(g_table_identity)) ev |= kSeatEvIdentity;

  if (p_engine_container != NULL
      && p_engine_container->symbol_engine_tablelimits()->bblind() > 0) {
    ev |= kSeatEvBlinds;
  }

  int nchairs = (p_tablemap == NULL) ? 0 : p_tablemap->nchairs();
  int named_seats = 0;
  for (int i = 0; i < nchairs && p_table_state != NULL; ++i) {
    CPlayer *pl = p_table_state->Player(i);
    if (pl != NULL && pl->seated() && !pl->name().IsEmpty()) ++named_seats;
  }
  if (named_seats >= 2) ev |= kSeatEvSeats;

  int userchair = kUndefined;
  if (p_engine_container != NULL
      && p_engine_container->symbol_engine_userchair()->userchair_confirmed()) {
    userchair = p_engine_container->symbol_engine_userchair()->userchair();
    ev |= kSeatEvHeroChair;      // reported as evidence only -- deliberately NOT decisive, see below
  }
  // WHOSE seat is it? By NAME, not by userchair. userchair is set by CalculateUserChair from the
  // first seat showing known cards, which while RAILING a table points at an opponent: observed live
  // with userchair=3 on "CrowdStanding" and the observer flag false. Deciding from that would report
  // "seated" at a table we are only watching -- and the automation would then never restart a client
  // that had wandered off. The user's own username list is the real discriminator.
  int my_chair = HeroChairByName();
  if (my_chair >= 0 && my_chair < nchairs && p_table_state != NULL) {
    ev |= kSeatEvHeroNamed;
    CPlayer *hero = p_table_state->Player(my_chair);
    if (hero != NULL) {
      if (hero->_balance.GetValue() > 0)  ev |= kSeatEvHeroStack;
      if (hero->HasKnownCards())          ev |= kSeatEvHeroCards;
    }
  }

  if (p_handreset_detector != NULL && !p_handreset_detector->GetHandNumber().IsEmpty()) {
    ev |= kSeatEvHand;
  }
  if (p_casino_interface != NULL
      && p_casino_interface->NumberOfVisibleAutoplayerButtons() > 0) {
    ev |= kSeatEvButtons;
  }
  if (p_scraper != NULL && p_scraper->ObserverActive()) ev |= kSeatEvObserver;

  // A table is REAL only with all three of: a believable identity, sane blinds, and populated
  // seats. Every "am I somewhere" question is gated on that, so nothing in the lobby can qualify.
  const bool at_a_real_table =
      (ev & kSeatEvIdentity) && (ev & kSeatEvBlinds) && (ev & kSeatEvSeats);

  // SEATED needs OUR name at a seat, and that seat to look occupied -- chips or cards. Chips alone
  // suffice because card templates can mismatch and read as backs; cards alone suffice because a
  // stack can scrape as 0 mid-allin. The observer flag, when the scraper does raise it, vetoes.
  const bool hero_present =
      (ev & kSeatEvHeroNamed) && ((ev & kSeatEvHeroStack) || (ev & kSeatEvHeroCards));

  long state = kSeatNotAtTable;
  if (at_a_real_table && hero_present && !(ev & kSeatEvObserver)) {
    state = kSeatSeated;
  } else if (at_a_real_table) {
    // A real table is on screen but no seat carries our name: we are RAILING it. This is an
    // "at a table" state -- the automation must not restart the client for it. It is also the
    // safe landing spot if our own name ever OCRs too badly to match, which is deliberate: a
    // missed name costs a mislabel, never a restart that drops us out of a live seat.
    state = kSeatObserving;
  }

  // Stamp the clock on the very first evaluation as well as on every change. Without this the
  // opening state -- not_at_table, which is also the zero the global initialises to -- never looks
  // like a "change", g_seat_since_tick stays 0, and stable_ms silently becomes the machine's uptime.
  // A just-started instance would then present a 13-hour-stable "no table" and the automation would
  // restart its poker client immediately, before the client had even finished loading.
  static bool s_seeded = false;
  if (state != g_seat_state || !s_seeded) {
    s_seeded = true;
    g_seat_state = state;
    g_seat_since_tick = (long)GetTickCount();
    NoteSeatStateChanged();   // feeds the flicker counter behind SeatIsStableForActing()
    write_log(k_always_log_basic_information,
      "[SeatStatus] state -> %s (evidence 0x%03x, %d named seats, my_chair %d, userchair %d)\n",
      state == kSeatSeated ? "seated" : state == kSeatObserving ? "observing" : "not_at_table",
      ev, named_seats, my_chair, userchair);
  }
  g_seat_evidence = ev;
}

void CHeartbeatThread::ScrapeEvaluateAct() {
	// No window to keep in position for the Scarlet Beast virtual connection.
	if (!p_autoconnector->IsVirtualConnection()) {
		p_table_positioner->AlwaysKeepPositionIfEnabled();
	}
	// This critical section lets other threads know that the internal state is being updated.
	//
	// The gate is checked HERE TOO, not just by off-thread users. ~CHeartbeatThread waits only
	// k_max_time_to_wait_for_thread_to_shutdown (5s) for this thread and then deletes the lock
	// regardless -- and a stalled scrape overruns 5s easily (the suspect-capture storm used to stall
	// it far longer). The heartbeat then entered a DESTROYED critical section and faulted inside
	// RtlEnterCriticalSection: crash_hiss_17224, ScrapeEvaluateAct+0x46.
	//
	// Registering as an in-flight user before the gate test, and having the dtor drain that count
	// before DeleteCriticalSection, closes the check-then-use window rather than just narrowing it.
	if (pParent == NULL) return;
	InterlockedIncrement(&cs_update_users);
	if (InterlockedCompareExchange(&cs_update_ready, 1, 1) == 0) {
		InterlockedDecrement(&cs_update_users);   // shutting down: end the cycle, touch nothing
		return;
	}
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
  // Publish the symbols the HTTP callers are reading (/api/symbols). We are already inside
  // cs_update_in_progress and the engines were just evaluated, so this is the cheapest possible
  // place to do it -- and it means the HTTP thread NEVER evaluates and never takes this lock.
  // Before this, a read of the bot's state could stall the bot: a large or heavy /api/symbols query
  // held the update lock for its whole duration, and the endpoint wedged outright (reproduced: a few
  // back-to-back 110-symbol pulls, and afterwards even a single cheap symbol timed out). The
  // nn_driver cannot decide a hand without this endpoint.
  RefreshSymbolSnapshot();
  DWORD t_eval_ms = GetTickCount() - t_eval0;
  // Refresh the HUD/PT4 stat cache HERE, on the heartbeat thread, inside the update lock
  // and right after the engine evaluated -- this is the ONLY place PT_DLL_GetStat (which
  // evaluates non-thread-safe PT4 query symbols) may run. The UI display + the HTTP
  // game-state JSON now only READ the cached values, so nothing evaluates symbols off this
  // thread (that race crashed Hiss: BuildTableStateJson -> RefreshIfNeeded -> CFunction::Evaluate).
  if (p_hud_manager != NULL && p_handreset_detector != NULL) {
    // Force the HUD to re-evaluate every frame for a short window after a table switch, instead of
    // honouring its 2500ms throttle. The seat names were just flushed, so StableName() is empty until
    // the majority-vote window refills; forcing (together with the post-switch scrape burst) lets the
    // new table's stats appear as soon as the names stabilise rather than up to 2.5s later.
    extern volatile unsigned long g_table_switch_tick;
    bool just_switched = (g_table_switch_tick != 0 && (GetTickCount() - g_table_switch_tick) < 1500);
    p_hud_manager->RefreshIfNeeded(p_handreset_detector->GetHandNumber(), just_switched);
  }
	// Reply-frames no longer here in the heartbeat.
  // we have a "ReplayFrameController for that.
  LeaveCriticalSection(&pParent->cs_update_in_progress);
  InterlockedDecrement(&cs_update_users);   // paired with the increment above; single exit path

  // ---- MCP / API control requests (run on this thread, where the autoplayer acts) ----
  // One-time restore of persisted modes (ULTRA / superstition) once the terminal port is bound, so
  // they survive a Hiss restart [Emrald]. Sets the same request flags the toolbar/API would, so the
  // engage handlers below launch the daemons exactly as a manual toggle does.
  static bool s_modes_restored = false;
  if (!s_modes_restored && g_terminal_port > 0) {
    s_modes_restored = true;
    DWORD want_ultra = ReadModeReg("UltraEngaged", 0);
    DWORD want_superstition = ReadModeReg("SuperstitionEngaged", 0);
    // Autoplayer + NN driver restore with the same mechanism. Default 0 (OFF) so a FIRST-EVER run,
    // or one after the registry is cleared, never starts playing on its own -- only a state the
    // user actually selected is restored.
    DWORD want_autoplayer = ReadModeReg("AutoplayerEngaged", 0);
    DWORD want_nn_driver  = ReadModeReg("NNDriverEngaged", 0);
    if (want_ultra && g_mcp_ultra_request < 0) g_mcp_ultra_request = 1;
    if (want_superstition && g_mcp_superstition_request < 0) g_mcp_superstition_request = 1;
    if (want_autoplayer && g_mcp_autoplayer_request < 0) g_mcp_autoplayer_request = 1;
    if (want_nn_driver && g_mcp_nn_driver_request < 0) g_mcp_nn_driver_request = 1;
    write_log(k_always_log_basic_information,
      "[restore] persisted modes -> ultra=%lu superstition=%lu autoplayer=%lu nn_driver=%lu\n",
      want_ultra, want_superstition, want_autoplayer, want_nn_driver);
    // Reclaim the window this instance was last on. Done HERE, in the same one-shot block, because
    // it needs g_terminal_port to be bound -- the port is what identifies the instance, and the
    // memory is keyed by it. If the window is gone this leaves the override off and the
    // autoconnector picks a table normally. See RestoreRememberedWindow in CAutoConnector.cpp.
    extern void RestoreRememberedWindow();
    RestoreRememberedWindow();
  }
  // Keep the window memory current. Called every heartbeat rather than only from
  // set_attached_hwnd because that fires BEFORE the terminal port is bound, and the memory is
  // keyed by port -- an attach-time-only save recorded everything under port 0. Cheap: it
  // returns immediately unless the port+title actually changed.
  if (g_terminal_port > 0 && p_autoconnector != NULL) {
    extern void RememberAttachedWindow(HWND window);
    HWND att = p_autoconnector->attached_hwnd();
    if (att != NULL && ::IsWindow(att)) {
      RememberAttachedWindow(att);
    }
  }
  if (g_mcp_autoplayer_request >= 0 && p_autoplayer != NULL) {
    bool want_on = (g_mcp_autoplayer_request == 1);
    g_mcp_autoplayer_request = -1;
    write_log(k_always_log_basic_information, "[MCP] Autoplayer -> %s (API request)\n", want_on ? "ON" : "OFF");
    // Persist the intent, like ULTRA/superstition already do. A restarted instance used to come up
    // with the autoplayer DISENGAGED while looking perfectly healthy -- attached, scraping, frames
    // logging -- so it silently sat out every hand. That cost a live session on 2026-07-19 after a
    // rebuild. Written HERE (on change) rather than at exit, because instances are routinely
    // force-killed for rebuilds and an exit-only save would never run. [Emrald]
    WriteModeReg("AutoplayerEngaged", want_on ? 1 : 0);
    p_autoplayer->EngageAutoplayer(want_on);
  }
  if (g_mcp_nn_driver_request >= 0) {
    bool nn_on = (g_mcp_nn_driver_request == 1);
    g_mcp_nn_driver_request = -1;
    ApplyNNDriverEngage(nn_on);
  }
  if (g_mcp_ultra_request >= 0) {
    bool ultra_on = (g_mcp_ultra_request == 1);
    g_mcp_ultra_request = -1;
    ApplyUltraEngage(ultra_on);
  }
  if (g_mcp_superstition_request >= 0) {
    bool superstition_on = (g_mcp_superstition_request == 1);
    g_mcp_superstition_request = -1;
    ApplySuperstitionEngage(superstition_on);
  }
  // MANUAL WINDOW OVERRIDE, requested by /api/connect-window (the toolbar button). Applied here
  // rather than on the HTTP thread because Connect() loads a tablemap and resets the whole
  // connection lifecycle -- that belongs on the thread that already owns it.
  if (g_manual_connect_request >= 0) {
    int req = g_manual_connect_request;
    g_manual_connect_request = -1;
    if (req == 0) {
      g_manual_connect_hwnd = 0;
      g_manual_connect_status = "automatic selection restored";
      write_log(k_always_log_basic_information, "[MCP] Manual window override CLEARED -> automatic selection\n");
    } else {
      HWND want = (HWND)(intptr_t)g_manual_connect_hwnd;
      // Move off the current table first -- the point of the override is to go somewhere else,
      // and Connect() is only reached from the not-connected branch.
      if (p_autoconnector->IsConnectedToAnything() && p_autoconnector->attached_hwnd() != want) {
        p_autoconnector->Disconnect("manual window override");
      }
      if (p_autoconnector->IsConnectedToAnything()) {
        g_manual_connect_status = "connected";   // already on the requested window
      } else if (p_autoconnector->Connect(want)) {
        g_manual_connect_status = "connected";
      } else {
        // Connect() returns a bare false for several distinct reasons. Work out which, so the
        // toolbar can say something useful instead of just failing.
        if (!::IsWindow(want)) {
          g_manual_connect_status = "that window no longer exists";
        } else if (!::IsWindowVisible(want)) {
          g_manual_connect_status = "that window is hidden (only visible windows can be attached)";
        } else if (p_sharedmem != NULL && p_sharedmem->PokerWindowAttached(want)) {
          g_manual_connect_status = "already served by another Hiss instance";
        } else {
          g_manual_connect_status = "no tablemap matches that window (size / title / tablepoints)";
        }
        // A failed pin must not silently strand the instance with no table: drop back to
        // automatic so the bot keeps working while the user picks again.
        g_manual_connect_hwnd = 0;
      }
      write_log(k_always_log_basic_information, "[MCP] Manual window override -> hwnd=0x%p: %s\n",
                want, g_manual_connect_status.GetString());
    }
  }
  SyncSuperstitionLiveness();   // clear the engaged flag if the oracle died, so the indicator stays honest [Emrald]
  if (g_mcp_action_request >= 0 && p_casino_interface != NULL) {
    const char *seat_why = "";
    static DWORD s_last_seat_hold_log = 0;
    bool my_turn = (p_engine_container->symbol_engine_autoplayer() != NULL
                    && p_engine_container->symbol_engine_autoplayer()->ismyturn());
    bool force = g_mcp_action_force;   // bypass the ismyturn gate (the NN driver sets this on every action)
    bool human = g_mcp_action_human;   // a REAL person clicked: the only thing that waives the trust gate
    // THE SPOT MOVED ON -> DROP IT.
    //
    // The 25 s expiry alone is not enough: hands finish in seconds, so a decision made for hand N
    // (or for the flop) could still be sitting in the queue when hand N+1 deals -- or when the turn
    // card lands -- and fire there, into a spot nobody decided anything about, the moment a matching
    // button appeared. force=1 makes that worse, because it bypasses the ismyturn gate entirely.
    // An action is only valid for the exact hand and street it was decided for.
    CString cur_hand = (p_handreset_detector != NULL) ? p_handreset_detector->GetHandNumber() : CString("");
    int cur_betround = (p_betround_calculator != NULL) ? p_betround_calculator->betround() : -1;
    // A spot is (table, hand, betround) -- NOT just (hand, betround). Table identity is the field that
    // was missing, and it is the one that matters when several tables share a name: the two tables in
    // hand 2783851433 were both "GadsdenNoLimi" and differed only by id, so a hand/betround comparison
    // saw nothing wrong while the decision crossed from one table to the other.
    bool table_moved =
        (!g_mcp_action_table.IsEmpty() && !g_table_identity.IsEmpty()
         && g_mcp_action_table != g_table_identity);
    bool stale_spot =
        (!g_mcp_action_hand.IsEmpty() && !cur_hand.IsEmpty() && g_mcp_action_hand != cur_hand)
     || (g_mcp_action_betround >= 0 && cur_betround >= 0 && g_mcp_action_betround != cur_betround)
     || table_moved;

    if (stale_spot) {
      write_log(k_always_log_basic_information,
        "[MCP] Manual action DISCARDED: decided for table %s hand %s round %d, but the table is now "
        "%s hand %s round %d. A decision is only valid for the spot it was made in.%s\n",
        g_mcp_action_table.GetString(), g_mcp_action_hand.GetString(), g_mcp_action_betround,
        g_table_identity.GetString(), cur_hand.GetString(), cur_betround,
        table_moved ? " [TABLE SWITCHED MID-HAND]" : "");
      g_mcp_action_request = -1;
      g_mcp_action_amount = -1.0;
      g_mcp_action_force = false;
      g_mcp_action_human = false;
      g_mcp_action_hand = "";
      g_mcp_action_betround = -1;
      g_mcp_action_table = "";
    } else if (GetTickCount() - g_mcp_action_set_tick > 25000) {
      // Expired before our turn came -- discard so it can't fire a later hand.
      write_log(k_always_log_basic_information, "[MCP] Manual action expired before our turn; discarded.\n");
      g_mcp_action_request = -1;
      g_mcp_action_amount = -1.0;
      g_mcp_action_force = false;
      g_mcp_action_human = false;
      g_mcp_action_hand = "";
      g_mcp_action_betround = -1;
      g_mcp_action_table = "";
    } else if (!force && !my_turn) {
      // Not our turn yet -- keep the request PENDING and retry next heartbeat. With
      // force (manual click) we skip this wait and try the click below right away.
    } else if (!human && !ScrapeIsTrustworthyForActing(&seat_why)) {
      // SCRAPE-TRUST GATE for the NN path (the OHF equivalent lives in CAutoplayer.cpp).
      //
      // NOTE ON `force`: it does NOT mean "a human clicked this". nn_driver.py sends force=1 on EVERY
      // action (mcp/nn_driver.py: q = {"do": action, "force": "1"}) purely to bypass the ismyturn
      // wait, which is why the log calls autonomous NN actions "[MCP] Manual ...". Exempting force
      // here would leave the entire NN path ungated -- exactly how hand 2783842881 triple-barrelled
      // and then fired a 65.5bb donk while the validator was reporting "duplicate card among
      // hands/board" on every evaluation. Only an explicit human=1 (the learner UI) bypasses.
      //
      // Held PENDING, not discarded: the hand/betround staleness check above already prevents it
      // firing on a later hand, so if the scrape settles inside the 25s window the action still lands
      // on the correct spot. [Emrald: gate acting on seat-status + validator]
      if (GetTickCount() - s_last_seat_hold_log > 3000) {
        s_last_seat_hold_log = GetTickCount();
        write_log(k_always_log_errors,
          "[MCP] HOLDING action (%s) -- scrape not trustworthy: %s\n",
          ActionConstantNames(g_mcp_action_request), seat_why);
      }
      NoteSeatUnstableWhileActing();
    } else {
      int code = g_mcp_action_request;
      double amount = g_mcp_action_amount;
      g_mcp_action_request = -1;
      g_mcp_action_amount = -1.0;
      g_mcp_action_force = false;
      g_mcp_action_human = false;
      g_mcp_action_hand = "";
      g_mcp_action_betround = -1;
      g_mcp_action_table = "";
      // Publish the action to the on-table RED decision overlay (HudOverlayWindow).
      //
      // The OHF autoplayer publishes its own decision in CAutoplayer.cpp -- but that path is
      // deliberately BYPASSED whenever the NN driver is engaged on NLH (CAutoplayer::DoAutoplayer
      // defers the primary decision to the NN to avoid double-acting), and the NN acts through
      // /api/action, i.e. right here. So with the NN driving, the overlay never updated: it showed
      // nothing, or worse, kept trailing whatever the OHF last decided. Publish from whichever
      // path actually ACTS, so the red decision reflects the bot that is really playing. [Emrald]
      //
      // Amounts on this path are in BIG BLINDS (that is what /api/action takes and what the
      // two-successive-clicks/numpad path types), so label them bb rather than implying dollars.
      {
        CStringA decision;
        if      (code == k_autoplayer_function_fold)  decision = "FOLD";
        else if (code == k_autoplayer_function_check) decision = "CHECK";
        else if (code == k_autoplayer_function_call)  decision = "CALL";
        else if (code == k_autoplayer_function_allin) decision = "ALL-IN";
        else if (code == k_autoplayer_function_raise) {
          if (amount > 0) decision.Format("RAISE %.2fbb", amount);
          else            decision = "RAISE";
        }
        if (!decision.IsEmpty()) {
          strcpy_s(g_hero_decision_text, sizeof(g_hero_decision_text), decision.GetString());
          strcpy_s(g_hero_decision_source, sizeof(g_hero_decision_source), "NN DRIVER");
          g_hero_decision_tick = GetTickCount();
        }
      }
      // Return the cursor to where the user left it after the WHOLE sequence.
      CCursorRestorer _cursor_restorer;
      // Sized bet/raise AND ALL-IN: go through the autoplayer's two-successive-clicks +
      // on-screen numpad path (same as auto-play), entering the amount (in big blinds).
      //
      // ALL-IN used to be EXCLUDED here (the gate was `code == ..._raise && amount > 0`), so a
      // jam fell through to the plain-click branch below and did ONE click. On these phone maps
      // that single click only pops the "Raise Options" panel OPEN -- the amount is never typed
      // and the raise is never confirmed. That is the NN driver "raises with one click and
      // nothing happens" bug: the driver sends do=allin with NO &amount (its <=12bb rule jams
      // instead of raising), so it never met the old gate. The autoplayer never had this bug --
      // its own two-successive-clicks path handles wants_allin (CAutoplayer.cpp) -- but the
      // /api/action path that the NN driver / learner use was never given the same treatment.
      //
      // An ALL-IN carries no numeric RaiseTo size, so type the FULL stack (posted bet +
      // remaining balance) PLUS kAllinOvershoot, exactly as CAutoplayer's two-successive-clicks
      // path does. The table accepts an over-bet and caps it to our real stack, so overshooting
      // means the shove no longer depends on our stack scrape being exact -- only on it being
      // close enough to be too big. A stack that scraped a hair high used to type an amount the
      // table refused, which left the raise panel open and hung the bot mid-hand.
      double keypad_amount = amount;
      if (code == k_autoplayer_function_allin && keypad_amount <= 0
          && p_table_state != NULL && p_table_state->User() != NULL) {
        keypad_amount = p_table_state->User()->_bet.GetValue()
                      + p_table_state->User()->_balance.GetValue() + kAllinOvershoot;
      }
      // HandleCycle() is self-gating: it only fires when the configured label regions
      // ("BetOptions" / "RaiseOptions") actually match on screen. So on a short-stack table that
      // shows a real Fold|All-In bar instead, it returns false and we correctly fall back to a
      // single click of the genuine All-In button.
      if ((code == k_autoplayer_function_raise || code == k_autoplayer_function_allin)
          && keypad_amount > 0 && p_two_successive_clicks != NULL
          && p_two_successive_clicks->HandleCycle(true)) {
        Sleep(p_two_successive_clicks->DelayMs());
        p_casino_interface->EnterBetsizeNumpadRaw(keypad_amount);
        write_log(k_always_log_basic_information,
          "[MCP] Manual %s %.2fbb via two-successive-clicks%s.\n",
          (code == k_autoplayer_function_allin) ? "ALL-IN" : "sized bet/raise",
          keypad_amount,
          (code == k_autoplayer_function_allin && amount <= 0) ? " (full stack)" : "");
      } else if (code == k_autoplayer_function_raise && amount > 0) {
        // SIZED RAISE REQUESTED, NO RAISE CONTROL -> CALL (or CHECK).
        //
        // The third member of the family below: all-in with no all-in button calls, check/call
        // with no passive button folds, and this one -- a sized raise the table will not offer.
        //
        // Facing a shove that our stack covers, these phone maps show Fold | Call | All-In with
        // no Bet/Raise Options panel, so the two-successive-clicks path above self-gates off (its
        // label region reads e.g. "104.5BBAllIn" instead of "BetOptions"). We then landed here,
        // found no clickable raise button, clicked NOTHING -- and still logged "clicked raise
        // button", so the log looked like action while the hand rotted. The driver re-decided
        // every heartbeat until the table timed us out. Observed on hand 2783568571 (3c 3h on the
        // button): the NN asked to raise 2.50bb nine times over 51 s and never acted; ACR
        // auto-folded and sat us out.
        //
        // CALL is the right downgrade, not All-In. Asking for 2.5bb means we wanted a SMALL raise;
        // the only raise this bar offers is the whole 104bb stack, and turning a min-raise into a
        // stack-off is the worst way to be wrong. Calling keeps the hand on the terms the decision
        // actually contemplated. If there is nothing to call we take the free CHECK instead.
        //
        // GUARDED, for the same reason the passive->fold case is: a transient mis-scrape must not
        // silently convert an intended raise into a flat, so the table has to say it for
        // kBeatsBeforeDowngrade consecutive heartbeats first.
        static int s_no_raise_beats = 0;
        static const int kBeatsBeforeDowngrade = 3;
        CAutoplayerButton *btn = p_casino_interface->LogicalAutoplayerButton(code);
        if (btn != NULL && btn->IsClickable()) {
          s_no_raise_beats = 0;
          btn->Click();
          write_log(k_always_log_basic_information,
            "[MCP] Manual raise %.2fbb: two-clicks N/A, clicked raise button.\n", amount);
        } else {
          CAutoplayerButton *call_btn =
            p_casino_interface->LogicalAutoplayerButton(k_autoplayer_function_call);
          CAutoplayerButton *check_btn =
            p_casino_interface->LogicalAutoplayerButton(k_autoplayer_function_check);
          bool call_live  = (call_btn  != NULL && call_btn->IsClickable());
          bool check_live = (check_btn != NULL && check_btn->IsClickable());
          if (call_live || check_live) {
            if (++s_no_raise_beats >= kBeatsBeforeDowngrade) {
              CAutoplayerButton *use = call_live ? call_btn : check_btn;
              write_log(k_always_log_basic_information,
                "[MCP] Manual raise %.2fbb: no raise control for %d heartbeats (the bar offers no "
                "sized raise) -- downgrading to %s. A small raise never meant commit the stack.\n",
                amount, s_no_raise_beats, call_live ? "CALL" : "CHECK");
              use->Click();
              s_no_raise_beats = 0;
            } else {
              write_log(k_always_log_basic_information,
                "[MCP] Manual raise %.2fbb: no raise control (beat %d/%d) -- nothing clicked yet.\n",
                amount, s_no_raise_beats, kBeatsBeforeDowngrade);
            }
          } else {
            // Nothing to downgrade to either: say so plainly instead of claiming a click.
            write_log(k_always_log_basic_information,
              "[MCP] Manual raise %.2fbb: no raise control and no live Call/Check -- NOTHING "
              "clicked this heartbeat.\n", amount);
          }
        }
      } else {
        CAutoplayerButton *btn = p_casino_interface->LogicalAutoplayerButton(code);
        // ALL-IN REQUESTED, NO ALL-IN BUTTON -> PRESS CALL.
        //
        // Facing a shove these phone maps show only Fold | Call: there is no dedicated All-In
        // button, and no Bet/Raise Options panel either, so HandleCycle() above returns false and
        // we land here. The old code then hunted for an all-in button that does not exist and
        // logged "[MCP] Manual FCKRA code 1: button not clickable yet; keeping pending." on every
        // heartbeat for the whole turn -- while CSymbolEngineChipAmounts was simultaneously
        // logging "CALL BUTTON LIVE" -- and the bot sat the hand out. Observed on hand
        // 2782511829: the NN asked for all-in three times against a shove and never acted.
        //
        // Calling a bet that covers our stack IS the all-in, so this is the intended action, not
        // an approximation. GUARDED so it cannot turn an intended OPEN-JAM into a limp/flat: we
        // only do it when there is no way to raise at all (no clickable raise button and the
        // two-clicks panel did not match). If a raise is possible we never reach here -- the
        // sized/all-in numpad path above handles it.
        if (code == k_autoplayer_function_allin && (btn == NULL || !btn->IsClickable())) {
          CAutoplayerButton *raise_btn =
            p_casino_interface->LogicalAutoplayerButton(k_autoplayer_function_raise);
          CAutoplayerButton *call_btn =
            p_casino_interface->LogicalAutoplayerButton(k_autoplayer_function_call);
          bool can_raise = (raise_btn != NULL && raise_btn->IsClickable());
          if (!can_raise && call_btn != NULL && call_btn->IsClickable()) {
            write_log(k_always_log_basic_information,
              "[MCP] ALL-IN requested but no clickable all-in button and no raise available; "
              "the CALL button is live -- calling instead (facing a shove, call IS the all-in).\n");
            btn = call_btn;
          }
        }
        // PASSIVE ACTION REQUESTED, NO PASSIVE BUTTON -> FOLD.
        //
        // The mirror image of the all-in case above. Facing a shove that our stack covers, these
        // phone maps sometimes show a bare Fold | All-In bar -- no Check, no Call, no Raise Options.
        // A driver that asked for check/call then had no button to press, so we re-armed the request
        // and logged "button not clickable yet; keeping pending" every heartbeat until the 25 s
        // expiry, the driver re-decided, and the loop repeated until the table timed us out. Observed
        // on hand 2782903775 (5d Jd on the button): the NN asked to call twelve times over 90 s
        // against MrFalkom805's 15.1bb jam and never acted; ACR auto-folded us on the flop.
        //
        // FOLD is the right answer, not All-In. Wanting to check or flat means we did NOT want to
        // commit the stack; the only two things the table offers are giving up and jamming, so
        // giving up is what the decision actually meant. Clicking the All-In next to it would turn
        // a fold-equity-free flat into a stack-off -- the single worst way to be wrong here.
        //
        // GUARDED THREE WAYS, because folding a hand we could have checked for free is a real cost:
        //   1. the FOLD button must itself be clickable -- that proves the bar is rendered and
        //      scraped, so we are reading a genuine two-button spot and not a half-drawn frame;
        //   2. neither Check nor Call may be clickable -- if either is live the normal path takes it;
        //   3. it must hold for kMsBeforeGivingUp MILLISECONDS. The all-in->call case
        //      can fire on the first frame because being wrong there still jams, which is what was
        //      asked for. Here a transient mis-scrape would throw away a free check, so we make the
        //      table say it twice more before believing it.
        // SHORT-STACK VETO on every fold-downgrade below.
        //
        // The two downgrades that follow reason "a passive decision never meant commit the stack" --
        // true at normal depth, and false once the stack is a blind or two. Below
        // kDesperationStackBB the blinds take it next orbit whether we fold or not, so folding is the
        // one line that cannot win; CAutoplayer::ExecuteDesperationShoveOrCall already refuses to fold
        // there, and this path must not quietly do what that rule forbids.
        //
        // Hand 2783687719: hero held top pair with ~1 BB behind, the NN asked to CHECK, the Check
        // button was momentarily not clickable, and this code pressed FOLD -- throwing away a stack
        // that had nothing to protect. Measured from the same values the desperation rule uses.
        double sv_stack_bb = 0.0;
        {
          double sv_bb = (p_engine_container != NULL)
                       ? p_engine_container->symbol_engine_tablelimits()->bblind() : 0.0;
          if (sv_bb > 0.0 && p_table_state != NULL && p_table_state->User() != NULL) {
            sv_stack_bb = (p_table_state->User()->_balance.GetValue()
                         + p_table_state->User()->_bet.GetValue()) / sv_bb;
          }
        }
        const bool sv_desperate = (sv_stack_bb > 0.0 && sv_stack_bb < kDesperationStackBB);

        // The give-up wait is measured in TIME, not heartbeats.
        //
        // It was 3 heartbeats. At a 25ms scrape delay that is under a second -- the log shows beats
        // 1/3 and 2/3 landing in the SAME second -- while the driver polls every 0.6s and then has to
        // round-trip the model. So the "give the driver a chance to re-decide" window this comment
        // promises never actually existed, and a stale request was converted to a fold before the
        // fresh decision could replace it.
        //
        // Hand 2783719772: the NN asked to CHECK, villain bet, the NN re-decided to RAISE 12 with a
        // KING-HIGH STRAIGHT (Kc4c on Js6hTcQh9c) -- and the stale check folded it in the same second
        // the raise was decided. A whole made straight, thrown away by a race.
        //
        // 2500ms comfortably covers a poll plus a decision, and costs nothing when the driver does not
        // re-decide: we simply act on the original intent a beat later. [Emrald]
        static DWORD s_no_passive_since = 0;              // 0 = condition not currently seen
        const DWORD kMsBeforeGivingUp = 2500;
        if (s_no_passive_since == 0) s_no_passive_since = GetTickCount();
        DWORD s_no_passive_beats = GetTickCount() - s_no_passive_since;   // ms waited, for the logs
        const DWORD kBeatsBeforeGivingUp = kMsBeforeGivingUp;
        if ((code == k_autoplayer_function_check || code == k_autoplayer_function_call)
            && (btn == NULL || !btn->IsClickable())) {
          CAutoplayerButton *check_btn =
            p_casino_interface->LogicalAutoplayerButton(k_autoplayer_function_check);
          CAutoplayerButton *call_btn =
            p_casino_interface->LogicalAutoplayerButton(k_autoplayer_function_call);
          CAutoplayerButton *fold_btn =
            p_casino_interface->LogicalAutoplayerButton(k_autoplayer_function_fold);
          bool check_live = (check_btn != NULL && check_btn->IsClickable());
          bool call_live  = (call_btn  != NULL && call_btn->IsClickable());
          bool passive_live = check_live || call_live;
          bool can_fold = (fold_btn != NULL && fold_btn->IsClickable());
          if (!passive_live && can_fold) {
            if (s_no_passive_beats >= kBeatsBeforeGivingUp) {
              CAutoplayerButton *allin_btn =
                p_casino_interface->LogicalAutoplayerButton(k_autoplayer_function_allin);
              bool allin_live = (allin_btn != NULL && allin_btn->IsClickable());
              if (sv_desperate && allin_live) {
                // Fold|All-In bar with ~nothing behind: jam. See the short-stack veto above.
                write_log(k_always_log_basic_information,
                  "[MCP] %s requested, bar offers only Fold and All-In, but we hold %.2f BB (< %.1f) "
                  "-- JAMMING instead of folding: at this depth folding is the only line that cannot "
                  "win.\n", (code == k_autoplayer_function_check) ? "CHECK" : "CALL",
                  sv_stack_bb, kDesperationStackBB);
                btn = allin_btn;
              } else if (sv_desperate) {
                // No all-in control either. Still refuse to fold; keep the request pending so a
                // usable button next heartbeat can act on it.
                write_log(k_always_log_basic_information,
                  "[MCP] %s requested and no passive button, but we hold %.2f BB (< %.1f) -- NOT "
                  "folding. Waiting for a usable button.\n",
                  (code == k_autoplayer_function_check) ? "CHECK" : "CALL",
                  sv_stack_bb, kDesperationStackBB);
                s_no_passive_since = 0;
              } else {
                write_log(k_always_log_basic_information,
                  "[MCP] %s requested but the bar has no Check and no Call for %lu ms -- only "
                  "Fold and All-In. FOLDING: a passive decision never meant commit the stack.\n",
                  (code == k_autoplayer_function_check) ? "CHECK" : "CALL",
                  (unsigned long)s_no_passive_beats);
                btn = fold_btn;
              }
              if (!sv_desperate) s_no_passive_since = 0;
            }
          } else if (code == k_autoplayer_function_call && !call_live && check_live) {
            // CALL REQUESTED, NOTHING TO CALL -> CHECK, straight away.
            //
            // No Call button but a live Check means nobody has bet: checking IS the call that was
            // asked for, it costs nothing, and there is no way for a mis-scrape to make it
            // expensive -- so unlike the fold cases below this needs no waiting period.
            write_log(k_always_log_basic_information,
              "[MCP] CALL requested but there is nothing to call (no Call button, Check is live) "
              "-- CHECKING instead: with no bet in front of us, a check IS the call.\n");
            btn = check_btn;
            s_no_passive_since = 0;
          } else if (code == k_autoplayer_function_check && !check_live && call_live) {
            // CHECK REQUESTED WHILE FACING A BET -> wait, then FOLD.
            //
            // Check is not offered, so somebody HAS bet and the driver decided from a stale view
            // of the table. Without this the request simply stayed pending, nothing was clicked,
            // and the driver re-decided every heartbeat until ACR timed us out -- the same stall
            // that cost hand 2783568571 on the raise branch above.
            //
            // FOLD, never call. Asking to check means the decision declined to put money in;
            // quietly turning that into a call is how a bot pays off every bet it did not see.
            // The wait first gives the driver a few heartbeats to notice the bet and re-decide
            // for itself -- a fresh CALL or RAISE lands on its own path and never reaches here.
            if (s_no_passive_beats >= kBeatsBeforeGivingUp) {
              if (sv_desperate) {
                // Facing a bet with ~nothing behind. "Check never meant pay to continue" stops being
                // true when continuing costs a blind we lose anyway next orbit -- so CALL rather than
                // fold. This is the branch that folded top pair on hand 2783687719 at ~1 BB.
                write_log(k_always_log_basic_information,
                  "[MCP] CHECK requested facing a bet, but we hold %.2f BB (< %.1f) -- CALLING "
                  "instead of folding: at this depth the chips are gone either way.\n",
                  sv_stack_bb, kDesperationStackBB);
                btn = call_btn;
                s_no_passive_since = 0;
              } else if (can_fold) {
                write_log(k_always_log_basic_information,
                  "[MCP] CHECK requested but we are facing a bet (no Check, Call is live) for %d "
                  "heartbeats -- FOLDING: a check never meant pay to continue.\n",
                  s_no_passive_beats);
                btn = fold_btn;
              } else {
                write_log(k_always_log_basic_information,
                  "[MCP] CHECK requested facing a bet and no Fold button either -- nothing "
                  "clicked this heartbeat.\n");
              }
              s_no_passive_since = 0;
            } else {
              write_log(k_always_log_basic_information,
                "[MCP] CHECK requested but we are facing a bet (beat %d/%d) -- giving the driver a "
                "chance to re-decide before folding.\n", s_no_passive_beats, kBeatsBeforeGivingUp);
            }
          } else {
            s_no_passive_since = 0;   // buttons still settling, or the asked-for option really is live
          }
        } else {
          s_no_passive_since = 0;
        }
        if (btn != NULL && btn->IsClickable()) {
          // Log WHICH region we are about to click, its OCR'd label, and its rect -- not just the
          // action code. Buttons are matched to actions purely by their scraped LABEL, and the phone
          // stacks check/call/raise-options on overlapping rects, so a mislabelled region makes the
          // bot press a DIFFERENT physical button than the one it asked for (reported: "pressed the
          // raise button and nothing followed" on a hand where the bot only ever asked for
          // call/check -- hand 2777172499). Without the rect there is no way to tell which. The
          // pre-existing ButtonDebugLog claims to be "always-on" but is gated behind
          // debug_autoplayer(), so it was silent. This line is genuinely always on and fires only on
          // an actual click (a few per hand), so it cannot flood the log.
          RECT br = {0};
          p_tablemap->GetTMRegion(btn->TechnicalName(), &br);
          write_log(k_always_log_basic_information,
            "[MCP] Manual FCKRA action: code %d -> region \"%s\" label=\"%s\" rect=(%d,%d)-(%d,%d) center=(%d,%d)\n",
            code, btn->TechnicalName().GetString(), btn->Label().GetString(),
            br.left, br.top, br.right, br.bottom,
            (br.left + br.right) / 2, (br.top + br.bottom) / 2);
          btn->Click();
        } else {
          write_log(k_always_log_basic_information, "[MCP] Manual FCKRA code %d: button not clickable yet; keeping pending.\n", code);
          // Re-arm so it retries (e.g. buttons still appearing this turn). Preserve force
          // so a manual click keeps bypassing the ismyturn gate until it lands or expires.
          g_mcp_action_request = code;
          g_mcp_action_amount = amount;
          g_mcp_action_force = force;
        }
      }
      // ACTED ON THIS TABLE -> ask for a hop to the next open one.
      //
      // This is the path the NN DRIVER acts through (/api/action -> queued here), which is what runs
      // when nn-driver is engaged and the OHF autoplayer is not. Hooking only CAutoplayer::DoAutoplayer
      // meant the hop could never fire for an NN-driven bot -- the normal configuration here.
      //
      // Requested only when the request was CONSUMED: the not-clickable branch above re-arms it, so a
      // still-pending request means nothing was pressed and we must stay put and keep trying.
      if (g_mcp_action_request < 0) {
        extern volatile unsigned long g_pill_switch_request_tick;
        g_pill_switch_request_tick = GetTickCount();
      }
    }
  }
  // ---- MCP / API: reload the OHF strategy folder (safe here: outside the update
  // critical section, between evaluations). Re-parses bot_logic/Strategy + the master. ----
  if (g_mcp_reload_ohf_request && p_formula_parser != NULL) {
    g_mcp_reload_ohf_request = false;
    if (!p_formula_parser->IsParsing()) {
      write_log(k_always_log_basic_information, "[MCP] Reloading OHF strategy (API request)\n");
      p_formula_parser->ReloadStrategy();
    }
  }
  // ---- MCP / API: click an arbitrary tablemap region (lobby navigation, etc.).
  // Looks up the named region's rect and clicks its center via the mouse DLL. ----
  if (!g_mcp_click_region.IsEmpty() && p_tablemap != NULL && p_casino_interface != NULL) {
    CString rgn = g_mcp_click_region;
    g_mcp_click_region = "";
    RMapCI it = p_tablemap->r$()->find(rgn.GetString());
    if (it != p_tablemap->r$()->end()) {
      CCursorRestorer _cursor_restorer;
      RECT r;
      r.left = it->second.left; r.top = it->second.top;
      r.right = it->second.right; r.bottom = it->second.bottom;
      write_log(k_always_log_basic_information, "[MCP] Clicking region '%s' (%d,%d,%d,%d)\n",
                rgn.GetString(), r.left, r.top, r.right, r.bottom);
      p_casino_interface->ClickRect(r);
    } else {
      write_log(k_always_log_basic_information, "[MCP] Click region '%s' not found in tablemap\n", rgn.GetString());
    }
  }

  // ---- ACR table-tab hop: after acting here, move to the other open table ----------------------
  // ACR shows one pill per open table at the top of the felt. When the autoplayer acts we set a
  // request; once the click has settled we classify the pills and click an OCCUPIED (open but not
  // foreground) one. Doing nothing is always the safe outcome: a map with no pill regions, a single
  // table (the second pill reads as the empty "+"), or a screen that is not a table all classify as
  // EMPTY and leave us where we are.
  {
    extern volatile unsigned long g_pill_switch_request_tick;
    const unsigned long kSettleMs = 900;      // let the action's own clicks land first
    const unsigned long kGiveUpMs = 6000;     // stale request -> drop it rather than hop late
    static unsigned long s_last_pill_hop = 0;
    const unsigned long kHopCooldownMs = 2500;
    unsigned long req = g_pill_switch_request_tick;
    if (req != 0 && p_scraper != NULL && p_casino_interface != NULL) {
      unsigned long age = GetTickCount() - req;
      if (age > kGiveUpMs) {
        g_pill_switch_request_tick = 0;
      } else if (age >= kSettleMs && (GetTickCount() - s_last_pill_hop) >= kHopCooldownMs) {
        // Never hop while it is our turn again here -- acting beats tidying up.
        bool my_turn = (p_engine_container != NULL
                        && p_engine_container->symbol_engine_autoplayer()->ismyturn());
        int active = 0, occupied = 0, active_idx = -1;
        CScraper::TablePillState st[CScraper::kMaxTablePills];
        for (int i = 0; i < CScraper::kMaxTablePills; ++i) {
          st[i] = p_scraper->ClassifyTablePill(i);
          if (st[i] == CScraper::kPillActive) { ++active; active_idx = i; }
          else if (st[i] == CScraper::kPillOccupied) ++occupied;
        }
        // ROUND-ROBIN: take the next occupied slot AFTER the active one, wrapping. Always picking the
        // lowest-numbered one would ping-pong between two tables and starve the third once three are
        // open -- and three (plus the empty "+") is a normal layout.
        int target = -1;
        if (active_idx >= 0) {
          for (int step = 1; step <= CScraper::kMaxTablePills; ++step) {
            int cand = (active_idx + step) % CScraper::kMaxTablePills;
            if (st[cand] == CScraper::kPillOccupied) { target = cand; break; }
          }
        }
        // Require a coherent picture: exactly one foreground table AND at least one other open one.
        if (!my_turn && active == 1 && occupied >= 1 && target >= 0) {
          CString rgn;
          rgn.Format("table_pill%d", target);
          RMapCI it = p_tablemap->r$()->find(rgn.GetString());
          if (it != p_tablemap->r$()->end()) {
            CCursorRestorer _cursor_restorer;
            RECT r;
            r.left = it->second.left; r.top = it->second.top;
            r.right = it->second.right; r.bottom = it->second.bottom;
            write_log(k_always_log_basic_information,
              "[TablePills] acted here; hopping to %s (%d active, %d occupied)\n",
              rgn.GetString(), active, occupied);
            p_casino_interface->ClickRect(r);
            s_last_pill_hop = GetTickCount();
          }
          g_pill_switch_request_tick = 0;
        } else if (active != 1 || occupied < 1) {
          // Single table, or nothing recognisable on screen -> nothing to hop to.
          g_pill_switch_request_tick = 0;
        }
      }
    }
  }

  // ---- Seat status for /api/seat-status: am I really sat at (or railing) a table? ----
  UpdateSeatStatus();

  // ---- MCP / API: apply HUD overlay positions posted by Claude (/api/hud-positions).
  // Hand the JSON to the overlay on the UI thread (DB write + repaint happen there). ----
  if (g_hud_positions_apply) {
    g_hud_positions_apply = false;
    if (p_hud_overlay_window != NULL && ::IsWindow(p_hud_overlay_window->GetSafeHwnd())) {
      p_hud_overlay_window->PostMessage(WM_HUD_APPLY_POSITIONS, 0, 0);
      write_log(k_always_log_basic_information, "[MCP] Applied HUD positions from API.\n");
    }
  }

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
	// Refresh the FCKRA / TIOLP clickable-button indicators EVERY heartbeat -- engaged or not. This
	// used to happen inside DoAutoplayer(), which only runs when the autoplayer is engaged, so under
	// the NN driver fckra stayed EMPTY and /api/table-state could not tell the driver which buttons
	// were on screen. The driver then clicked "check" blind into a spot that had no Check button and
	// hit RAISE OPTIONS instead (hand 2777062344). Cheap: reads already-scraped button state.
	p_autoplayer->CacheButtonIndicators();
	if (p_autoplayer->autoplayer_engaged()) {
		write_log(Preferences()->debug_heartbeat(), "[HeartBeatThread] Calling DoAutoplayer.\n");
		p_autoplayer->DoAutoplayer();   // reaches HandleSitinFast() internally
	}
	else {
		// ALWAYS RE-SEAT. Sitting out bleeds blinds every orbit, so the Sit-In button gets clicked no
		// matter what is (or isn't) driving the bot.
		//
		// This used to be `else if (g_nn_driver_engaged)`, so fast Sit-In only ran when the autoplayer
		// OR the NN driver was engaged. With BOTH off -- which is the state after every single restart,
		// since the autoplayer comes up disengaged -- nothing clicked "I Am Back" and the bot just sat
		// there blinding off. Re-seating is not a strategy decision and must not depend on who is
		// playing: an unseated bot cannot act at all.
		//
		// Safe to run every heartbeat: HandleSitinFast() self-guards -- it only clicks when a Sit-In
		// button is genuinely on screen (f$sitin + a clickable button) and holds a 1.2s cooldown, and
		// the "I Am Back" label disappears once the click lands, so it cannot toggle us back out.
		if (p_autoplayer->HandleSitinFast()) {
			write_log(k_always_log_basic_information,
				"[HeartBeatThread] Fast Sit-In: clicked (autoplayer=%s, nn_driver=%s) -- a sat-out bot "
				"blinds off, so we re-seat regardless of who is driving.\n",
				p_autoplayer->autoplayer_engaged() ? "on" : "off",
				g_nn_driver_engaged ? "on" : "off");
		}
	}
	// Even when the autoplayer is DISABLED (manual / NN driver / off), PUBLISH the would-be OHF decision
	// so the RED DECISION overlay still shows on scrcpy WHAT IT WOULD HAVE DONE if it were enabled. This
	// self-gates on ismyturn && isfinalanswer and dedups, so the engaged path (DoAutoplayer already emits
	// it) is unaffected. [Emrald: show the red decision even when autoplayer is off]
	p_autoplayer->EmitDecisionTrace();
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
	// MANUAL WINDOW OVERRIDE: while pinned, offer the autoconnector that ONE window and nothing
	// else, so a reconnect (table briefly gone, instance restarted) can only ever land back on the
	// user's chosen window -- never on whatever the automatic first-match rule would have grabbed.
	// The cross-instance failure throttle is deliberately still honoured: it exists to stop several
	// instances fighting over the same table, and a manual pin is not a reason to opt out of that.
	if (g_manual_connect_hwnd != 0) {
		HWND pinned = (HWND)(intptr_t)g_manual_connect_hwnd;
		if (!::IsWindow(pinned)) {
			static DWORD s_gone_log = 0; DWORD now_gone = ::GetTickCount();
			if (now_gone - s_gone_log > 5000) {
				s_gone_log = now_gone;
				write_log(Preferences()->debug_autoconnector(),
					"[CHeartbeatThread] pinned window 0x%p is gone; holding the pin (not auto-selecting)\n", pinned);
			}
			return;
		}
		if (p_autoconnector->SecondsSinceLastFailedAttemptToConnect() > 1 /* seconds */) {
			p_autoconnector->Connect(pinned);
		}
		return;
	}
	if (Preferences()->autoconnector_when_to_connect() == k_AutoConnector_Connect_Permanent) {
		if (p_autoconnector->SecondsSinceLastFailedAttemptToConnect() > 1 /* seconds */) {
			write_log(Preferences()->debug_autoconnector(), "[CHeartbeatThread] going to call Connect()\n");
			p_autoconnector->Connect(NULL);
		}	else {
			{ static DWORD s_thr_block = 0; DWORD now_s_thr_block = ::GetTickCount(); if (now_s_thr_block - s_thr_block > 5000) { s_thr_block = now_s_thr_block; write_log(Preferences()->debug_autoconnector(), "[CHeartbeatThread] Reconnection blocked. Other instance failed previously.\n"); } }
		}
	}
}
// flood-throttle applied
