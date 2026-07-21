//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Watching for crashed processes and cleaning up shared memory
//   * so that another instance can connect to this table
//   * because the OpenHoldem starter needs the number of running processes
//
//******************************************************************************

#include "stdafx.h"
#include "CWatchdog.h"

#include "CSessionCounter.h"
#include "CSharedMem.h"

CWatchdog *p_watchdog = NULL;

// The heartbeats live in CSharedMem's named file mapping, reached through
// p_sharedmem->AliveTimestamp()/SetAliveTimestamp(). They used to be declared here in
// the .ohshmem data_seg, but that segment is NOT shared across processes on this
// toolchain -- each instance got a private copy. Since openholdem_PIDs IS genuinely
// shared, every instance could see its siblings but never their heartbeats, so every
// sibling looked permanently frozen to every other one. That is what used to make
// WatchForFrozenProcesses() below TerminateProcess() healthy bots; the kill is gone
// now, but without this the freeze verdict itself was still always wrong.

const int kSecondsToconsiderAProcessAsFrozen = 15;

CWatchdog::CWatchdog() {
  write_log(Preferences()->debug_watchdog(), "[CWatchdog] CWatchdog()\n");
  MarkThisInstanceAsAlive();
}

CWatchdog::~CWatchdog() {
  write_log(Preferences()->debug_watchdog(), "[CWatchdog] ~CWatchdog()\n");
  MarkThisInstanceAsDead();
}

void CWatchdog::HandleCrashedAndFrozenProcesses() {
  assert(p_sharedmem != NULL);
  if (p_sharedmem->OpenHoldemProcessID() == 0) {
    write_log(k_always_log_errors, "WARNING! Watch-dog turned off, unavailable process ID\n");
    return;
  }
  MarkThisInstanceAsAlive();
  WatchForCrashedProcesses();
  WatchForFrozenProcesses();
}

void CWatchdog::MarkInstanceAsAlive(int session_ID) {
  write_log(Preferences()->debug_watchdog(), "[CWatchdog] Marking instance %d alive\n", session_ID);
  assert(session_ID >=  0);
  assert(session_ID < MAX_SESSION_IDS);
  // p_watchdog is constructed BEFORE p_sharedmem (and destroyed after it), so the
  // heartbeat store is legitimately absent at both ends of the process lifetime.
  // Nothing is lost by skipping: the next heartbeat marks us alive again.
  if (p_sharedmem == NULL) return;
  time_t current_time;
  time(&current_time);
  p_sharedmem->SetAliveTimestamp(session_ID, current_time);
}

void CWatchdog::MarkThisInstanceAsAlive() {
  write_log(Preferences()->debug_watchdog(), "[CWatchdog] Marking this instance alive\n");
  assert(p_sessioncounter != NULL);
  MarkInstanceAsAlive(p_sessioncounter->session_id());
}

void CWatchdog::MarkInstanceAsDead(int session_ID) {
  write_log(Preferences()->debug_watchdog(), "[CWatchdog] Marking instance %d dead\n", session_ID);
  assert(session_ID >= 0);
  assert(session_ID < MAX_SESSION_IDS);
  // See MarkInstanceAsAlive(): p_sharedmem is already gone during our own teardown.
  // A crashed/exited instance is reaped anyway by WatchForCrashedProcesses(), which
  // tests real PID liveness rather than the heartbeat.
  if (p_sharedmem == NULL) return;
  p_sharedmem->SetAliveTimestamp(session_ID, kUndefinedZero);
}

void CWatchdog::MarkThisInstanceAsDead() {
  write_log(Preferences()->debug_watchdog(), "[CWatchdog] Marking this instance dead\n");
  assert(p_sessioncounter != NULL);
  MarkInstanceAsDead(p_sessioncounter->session_id());
}

void CWatchdog::WatchForCrashedProcesses() {
  write_log(Preferences()->debug_watchdog(), "[CWatchdog] Watching for crashed processes\n");
  for (int i = 0; i < MAX_SESSION_IDS; ++i) {
    if (p_sharedmem->IsDeadOpenHoldemProcess(i)) {
      write_log(Preferences()->debug_watchdog(), "[CWatchdog] Found crashed process and cleaning up\n");
      MarkInstanceAsDead(i);;
      p_sharedmem->CleanUpProcessMemory(i);
    }
  }
}

// !! move to dll
BOOL KillProcess(DWORD dwProcessId) {
  // http://stackoverflow.com/questions/2443738/c-terminateprocess-function
  DWORD dwDesiredAccess = PROCESS_TERMINATE;
  BOOL  bInheritHandle = FALSE;
  INT   uExitCode = -1;
  HANDLE hProcess = OpenProcess(dwDesiredAccess, bInheritHandle, dwProcessId);
  if (hProcess == NULL) {
    return FALSE;
  }
  BOOL result = TerminateProcess(hProcess, uExitCode);
  CloseHandle(hProcess);
  return result;
}

void CWatchdog::WatchForFrozenProcesses() {
  write_log(Preferences()->debug_watchdog(), "[CWatchdog] Watching for frozen processes\n");
  time_t current_time;
  time(&current_time);
  for (int i = 0; i < MAX_SESSION_IDS; ++i) {
    if (p_sharedmem->OpenHoldemProcessID(i) == 0) {
      // Not a process
      continue;
    }
    time_t last_process_timestamp = p_sharedmem->AliveTimestamp(i);
    int seconds_elapsed = current_time - last_process_timestamp;
    assert(seconds_elapsed >= 0);
    if (seconds_elapsed > kSecondsToconsiderAProcessAsFrozen) {
      if (seconds_elapsed > 2 * kSecondsToconsiderAProcessAsFrozen) {
        // Differences between time-stamp and currernt time way too large,
        // we should already have killed it.
        // Probably a new process which does not yet proper heartbeating,
        // fix its time-stamp and grant it some time to continue.
        write_log(Preferences()->debug_watchdog(), "[CWatchdog] Deep freeze detected %i, PID: %i\n",
          i, p_sharedmem->OpenHoldemProcessID(i));
        write_log(Preferences()->debug_watchdog(), "[CWatchdog] Might be stale data\n");
        write_log(Preferences()->debug_watchdog(), "[CWatchdog] Granting more time\n");
        p_sharedmem->OpenHoldemProcessID(i);
        MarkInstanceAsAlive(i);
        continue;
      }
#ifndef _DEBUG
      // DO NOT kill the other instance. This watchdog runs in EVERY instance and inspects
      // EVERY session's heartbeat, so a bot that merely stalls -- an adb tap that blocks, an
      // OCR worker round-trip, a burst of validator capture-dumps -- gets TerminateProcess'd
      // by a sibling. That is external termination: no ExitInstance, no shutdown.log line, no
      // WER event, which is exactly the "Hiss keeps closing for no reason" signature. Sysmon
      // caught it as Hiss.exe -> Hiss.exe with GrantedAccess 0x1 (PROCESS_TERMINATE), and 0x1
      // is requested in only one place: KillProcess() below.
      // A stall is not a crash. Genuinely dead processes are already reaped by
      // WatchForCrashedProcesses()/IsDeadOpenHoldemProcess(), which checks the process is gone
      // rather than merely quiet. So record the freeze and leave the other bot alone.
      {
        extern void ShutdownLog(const char *what);
        extern const char *g_shutdown_reason;
        const char *saved_reason = g_shutdown_reason;
        g_shutdown_reason = "observed-frozen-sibling-NOT-killed";
        ShutdownLog("WatchdogFreezeObserved");
        g_shutdown_reason = saved_reason;
      }
      write_log(k_always_log_errors,
        "[CWatchdog] Session %i (PID %i) has not heartbeat for >%i s. NOT killing it: a stall "
        "is not a crash, and killing a live sibling loses a hand mid-play.\n",
        i, p_sharedmem->OpenHoldemProcessID(i), kSecondsToconsiderAProcessAsFrozen);
#else
      // Don't kill any processes in debug.mode
      // It is extremely annoying if we hit a breakpoint
      // and another instance kills the paused program.
#endif _DEBUG
      write_log(Preferences()->debug_watchdog(), 
        "[CWatchdog] Skipped killing frozen process %i, PID: %i because of debug-mode\n",
        i, p_sharedmem->OpenHoldemProcessID(i));
    }
  }
}