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
  // [Emrald] AUTO-STARTER (scrcpy-window driven): keep ONE Hiss instance per scrcpy mirror window. Count
  // scrcpy windows (class SDL_app) vs Hiss instances (class OpenHoldem); if there are MORE scrcpy windows
  // than Hiss instances, launch one more. Only the lowest-session instance coordinates (so instances
  // don't race to spawn) and it is throttled, so by the next tick the freshly-launched instance's window
  // already counts and we stop once they match.
  // Coordinator: ONLY the base instance (terminal port 27654 -- the first one launched) spawns new
  // instances, so they don't race. Hiss SHARED MEMORY does NOT share across processes in this setup
  // (see memory multi-instance-coordination-postgres), so we must NOT gate on p_sharedmem here; the
  // terminal port is a reliable per-process coordinator. The coordinator must already be ATTACHED (so a
  // freshly-launched, not-yet-connected instance doesn't also try to spawn).
  extern int g_terminal_port;
  if (g_terminal_port != 27654) return;
  if (p_autoconnector == NULL || !p_autoconnector->IsConnectedToAnything()) return;
  if (p_engine_container->symbol_engine_casino()->ConnectedToOHReplay()) return;
  int n_scrcpy = CountVisibleWindowsOfClass("SDL_app");
  int n_hiss   = CountVisibleWindowsOfClass("OpenHoldem");
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
