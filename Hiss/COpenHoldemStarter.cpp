//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: starting and terminating bots automatically
//
//******************************************************************************

#include "stdafx.h"
#include "COpenHoldemStarter.h"

#include "CAutoConnector.h"
#include "CEngineContainer.h"

#include "CSharedMem.h"
#include "CSessionCounter.h"
#include "CSymbolEngineCasino.h"
#include "CSymbolEngineTime.h"
#include "OpenHoldem.h"

// For connection and popup handling
const int kMinNumberOfUnoccupiedBotsNeeded =   1;
const int kSecondsToWaitBeforeTermination  = 120;
const int kSecondsToWaitBeforeNextStart    =   5;

COpenHoldemStarter::COpenHoldemStarter() {
  time(&_starting_time_of_last_instance);
}

COpenHoldemStarter::~COpenHoldemStarter() {
}

// Count visible top-level windows of a given class. scrcpy mirror windows are class "SDL_app"
// (titled by device, e.g. "S10"/"A17"); Hiss main windows are class "OpenHoldem". [Emrald]
static int s_count_class_n = 0;
static const char *s_count_class_name = NULL;
static BOOL CALLBACK CountClassEnumProc(HWND h, LPARAM) {
  if (!::IsWindowVisible(h)) return TRUE;
  char cls[64]; cls[0] = '\0';
  ::GetClassNameA(h, cls, sizeof(cls));
  if (s_count_class_name != NULL && strcmp(cls, s_count_class_name) == 0) s_count_class_n++;
  return TRUE;
}
static int CountVisibleWindowsOfClass(const char *cls) {
  s_count_class_n = 0; s_count_class_name = cls;
  ::EnumWindows(CountClassEnumProc, 0);
  s_count_class_name = NULL;
  return s_count_class_n;
}

void COpenHoldemStarter::StartNewInstanceIfNeeded() {
  // [Emrald] AUTO-STARTER (scrcpy-window driven): keep ONE Hiss instance per scrcpy mirror window.
  // Count scrcpy windows (class SDL_app) vs live Hiss instances; if there are MORE mirrors than
  // instances, launch one more. Only the base instance (terminal port 27654) coordinates, so
  // instances don't race, and it is throttled.
  //
  // COUNT INSTANCES FROM THE SHARED PID TABLE, NOT FROM WINDOW CLASSES. This used to be
  // CountVisibleWindowsOfClass("OpenHoldem") and that was doubly wrong:
  //   * Hiss's own main frame is a DEFAULT MFC class ("Afx:00400000:b:..."), NOT "OpenHoldem",
  //     so our own instances were never counted -- closing one could not lower the count, and
  //     the spawn condition stayed true forever. Symptom: a closed instance instantly reopens,
  //     and Hiss mains pile up (4 were live when this was found).
  //   * "OpenHoldem" IS the class of the sibling brass_serpent app's windows, so we were really
  //     counting how many brass_serpent bots were open and spawning Hiss to match.
  // Counting windows cannot be salvaged anyway: one instance owns several visible windows (the
  // main frame plus HissHUD), so it would over-count.
  // openholdem_PIDs lives in CSharedMem's named file mapping, which IS genuinely shared across
  // processes and is namespaced per app (Local\HissAutoConnectorSharedState_v2), so brass_serpent
  // cannot pollute it. OCR workers deliberately never register a PID, so they are excluded, and
  // the watchdog reaps dead entries every heartbeat. (The old comment here claimed the shared
  // memory does not share across processes -- that was true only of the legacy .ohshmem data_seg,
  // not of the file mapping this uses.)
  extern int g_terminal_port;
  // HISS_SINGLE_INSTANCE=1 suppresses the fleet entirely: this instance runs alone no matter how
  // many scrcpy mirrors are open. For debugging a single table, where the auto-starter otherwise
  // races a fresh instance onto every other mirror and muddies the logs. Read from the environment
  // rather than a stored preference on purpose -- it must not survive into a normal launch and
  // silently leave the operator with one bot when they expect the whole fleet.
  {
    char single[8] = { 0 };
    if (::GetEnvironmentVariableA("HISS_SINGLE_INSTANCE", single, sizeof(single)) > 0
        && single[0] == '1') {
      return;
    }
  }
  if (g_terminal_port != 27654) return;
  if (p_autoconnector == NULL || !p_autoconnector->IsConnectedToAnything()) return;
  if (p_engine_container->symbol_engine_casino()->ConnectedToOHReplay()) return;
  if (p_sharedmem == NULL) return;
  int n_scrcpy = CountVisibleWindowsOfClass("SDL_app");
  int n_hiss   = p_sharedmem->NBotsPresent();
  if (n_scrcpy <= 0 || n_hiss >= n_scrcpy) {
    write_log(Preferences()->debug_autostarter(),
      "[COpenHoldemStarter] %d scrcpy window(s), %d Hiss instance(s) -- no new instance needed.\n", n_scrcpy, n_hiss);
    return;
  }
  time_t current_time; time(&current_time);
  if (current_time - _starting_time_of_last_instance < kSecondsToWaitBeforeNextStart) {
    return;   // gave the last-launched instance time to show its window; don't flood
  }
  time(&_starting_time_of_last_instance);
  write_log(Preferences()->debug_autostarter(),
    "[COpenHoldemStarter] %d scrcpy window(s) > %d Hiss instance(s) -> launching one more [%s].\n",
    n_scrcpy, n_hiss, ExecutableFilename());
  ShellExecute(NULL, "open", ExecutableFilename(), NULL, "", SW_SHOWNORMAL);
}

void COpenHoldemStarter::CloseThisInstanceIfNoLongerNeeded() {
  // [Emrald] AUTO-SHUTDOWN (scrcpy-window driven): if THIS instance was attached to a scrcpy window that
  // has since CLOSED, terminate this instance (the auto-starter will spawn a fresh one if a new scrcpy
  // window appears). We only shut down once we HAVE attached (so a freshly-launched, not-yet-attached
  // instance keeps waiting for a window). Tabbing tables within scrcpy does NOT destroy the SDL window,
  // so it does not fire; only physically closing the scrcpy window does. If we re-attach to another live
  // window, we keep running.
  static bool ever_attached = false;
  static HWND last_hwnd = NULL;
  HWND att = (p_autoconnector != NULL) ? p_autoconnector->attached_hwnd() : NULL;
  if (att != NULL && ::IsWindow(att)) {
    ever_attached = true;
    last_hwnd = att;
    return;   // still attached to a live scrcpy window
  }
  if (!ever_attached) return;                              // never attached yet -> keep waiting
  if (last_hwnd != NULL && ::IsWindow(last_hwnd)) return;  // our window still exists (transient detach)
  write_log(Preferences()->debug_autostarter(),
    "[COpenHoldemStarter] Our scrcpy window closed -> shutting down this instance.\n");
  if (theApp.m_pMainWnd != NULL) {
    PostMessage(theApp.m_pMainWnd->GetSafeHwnd(), WM_QUIT, NULL, NULL);
  }
}
