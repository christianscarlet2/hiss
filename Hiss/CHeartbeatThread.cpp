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

// ---- NN driver engage/disengage (mutually exclusive with the autoplayer) --------------------
// Engaging launches python nn_driver.py aimed at THIS instance's terminal port (in its own
// console so its decisions stay visible) and disengages the autoplayer; disengaging kills it.
static void *g_nn_driver_proc = NULL;   // HANDLE of the launched nn_driver.py (NULL = none)
static void ApplyNNDriverEngage(bool want_on) {
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
  // Refresh the HUD/PT4 stat cache HERE, on the heartbeat thread, inside the update lock
  // and right after the engine evaluated -- this is the ONLY place PT_DLL_GetStat (which
  // evaluates non-thread-safe PT4 query symbols) may run. The UI display + the HTTP
  // game-state JSON now only READ the cached values, so nothing evaluates symbols off this
  // thread (that race crashed Hiss: BuildTableStateJson -> RefreshIfNeeded -> CFunction::Evaluate).
  if (p_hud_manager != NULL && p_handreset_detector != NULL) {
    p_hud_manager->RefreshIfNeeded(p_handreset_detector->GetHandNumber(), false);
  }
	// Reply-frames no longer here in the heartbeat.
  // we have a "ReplayFrameController for that.
  LeaveCriticalSection(&pParent->cs_update_in_progress);

  // ---- MCP / API control requests (run on this thread, where the autoplayer acts) ----
  // One-time restore of persisted modes (ULTRA / superstition) once the terminal port is bound, so
  // they survive a Hiss restart [Emrald]. Sets the same request flags the toolbar/API would, so the
  // engage handlers below launch the daemons exactly as a manual toggle does.
  static bool s_modes_restored = false;
  if (!s_modes_restored && g_terminal_port > 0) {
    s_modes_restored = true;
    DWORD want_ultra = ReadModeReg("UltraEngaged", 0);
    DWORD want_superstition = ReadModeReg("SuperstitionEngaged", 0);
    if (want_ultra && g_mcp_ultra_request < 0) g_mcp_ultra_request = 1;
    if (want_superstition && g_mcp_superstition_request < 0) g_mcp_superstition_request = 1;
    write_log(k_always_log_basic_information,
      "[restore] persisted modes -> ultra=%lu superstition=%lu\n", want_ultra, want_superstition);
  }
  if (g_mcp_autoplayer_request >= 0 && p_autoplayer != NULL) {
    bool want_on = (g_mcp_autoplayer_request == 1);
    g_mcp_autoplayer_request = -1;
    write_log(k_always_log_basic_information, "[MCP] Autoplayer -> %s (API request)\n", want_on ? "ON" : "OFF");
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
  if (g_mcp_action_request >= 0 && p_casino_interface != NULL) {
    bool my_turn = (p_engine_container->symbol_engine_autoplayer() != NULL
                    && p_engine_container->symbol_engine_autoplayer()->ismyturn());
    bool force = g_mcp_action_force;   // manual learner click: bypass the ismyturn gate
    if (GetTickCount() - g_mcp_action_set_tick > 25000) {
      // Expired before our turn came -- discard so it can't fire a later hand.
      write_log(k_always_log_basic_information, "[MCP] Manual action expired before our turn; discarded.\n");
      g_mcp_action_request = -1;
      g_mcp_action_amount = -1.0;
      g_mcp_action_force = false;
    } else if (!force && !my_turn) {
      // Not our turn yet -- keep the request PENDING and retry next heartbeat. With
      // force (manual click) we skip this wait and try the click below right away.
    } else {
      int code = g_mcp_action_request;
      double amount = g_mcp_action_amount;
      g_mcp_action_request = -1;
      g_mcp_action_amount = -1.0;
      g_mcp_action_force = false;
      // Return the cursor to where the user left it after the WHOLE sequence.
      CCursorRestorer _cursor_restorer;
      // Sized bet/raise: go through the autoplayer's two-successive-clicks + on-screen
      // numpad path (same as auto-play), entering the amount (in big blinds).
      if (code == k_autoplayer_function_raise && amount > 0
          && p_two_successive_clicks != NULL) {
        if (p_two_successive_clicks->HandleCycle(true)) {
          Sleep(p_two_successive_clicks->DelayMs());
          p_casino_interface->EnterBetsizeNumpadRaw(amount);
          write_log(k_always_log_basic_information, "[MCP] Manual sized bet/raise %.2fbb via two-successive-clicks.\n", amount);
        } else {
          CAutoplayerButton *btn = p_casino_interface->LogicalAutoplayerButton(code);
          if (btn != NULL && btn->IsClickable()) btn->Click();
          write_log(k_always_log_basic_information, "[MCP] Manual raise %.2fbb: two-clicks N/A, clicked raise button.\n", amount);
        }
      } else {
        CAutoplayerButton *btn = p_casino_interface->LogicalAutoplayerButton(code);
        if (btn != NULL && btn->IsClickable()) {
          write_log(k_always_log_basic_information, "[MCP] Manual FCKRA action: clicking button code %d\n", code);
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
	if (p_autoplayer->autoplayer_engaged()) {
		write_log(Preferences()->debug_heartbeat(), "[HeartBeatThread] Calling DoAutoplayer.\n");
		p_autoplayer->DoAutoplayer();
	}
	else if (g_nn_driver_engaged) {
		// The NN driver plays via POST /api/action and never clicks screen buttons, so a sat-out bot
		// would blind off. Keep re-seating under the NN driver too: fire the fast Sit-In every
		// heartbeat (it self-guards -- only clicks when the Sit-In button is actually present).
		if (p_autoplayer->HandleSitinFast()) {
			write_log(Preferences()->debug_heartbeat(), "[HeartBeatThread] NN-driver: Fast Sit-In handled\n");
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
	if (Preferences()->autoconnector_when_to_connect() == k_AutoConnector_Connect_Permanent) {
		if (p_autoconnector->SecondsSinceLastFailedAttemptToConnect() > 1 /* seconds */) {
			write_log(Preferences()->debug_autoconnector(), "[CHeartbeatThread] going to call Connect()\n");
			p_autoconnector->Connect(NULL);
		}	else {
			write_log(Preferences()->debug_autoconnector(), "[CHeartbeatThread] Reconnection blocked. Other instance failed previously.\n");
		}
	}
}