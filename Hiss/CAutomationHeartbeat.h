//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: AUTOMATION heartbeat -- log the poker client back in when it has
//   dropped to its login screen.
//
//******************************************************************************

#ifndef INC_CAUTOMATIONHEARTBEAT_H
#define INC_CAUTOMATIONHEARTBEAT_H

#include "../CTablemap/CTablemap.h"
#include <opencv2/core.hpp>

// A SECOND heartbeat, deliberately separate from CHeartbeatThread.
//
// The playing heartbeat scrapes the felt with the playing tablemap and must stay fast and
// undisturbed; this one runs on its own slow tick against a different map entirely (the
// automation.* schema), and only while the AUTOMATION button is on for this instance. Two
// concerns, two threads: a login check has no business inside the loop that decides bets.
//
// What it does, once per tick:
//    * automation off for this instance?         -> do nothing
//    * OCR the acr_login_button region (autoocr0)
//    * does it read "login"?                     -> the client is logged OUT, so run the
//                                                   login sequence for this instance
//
// The typing itself is done by mcp\automation_login.py rather than from here. That
// sequence has to clear the fields, survive Chrome's "Use saved password?" sheet (which
// SWALLOWS the keystrokes, so the password must be typed again after dismissing it), and
// read each field back to confirm what landed -- all of which is adb work that is far
// safer to own, and to have already proven, outside an MFC GUI thread.
class CAutomationHeartbeat {
 public:
  CAutomationHeartbeat();
  ~CAutomationHeartbeat();

 public:
  void StartThread();
  void StopThread();
  // Last thing this heartbeat saw in the login-button region, for /api/automation-status.
  static CString last_ocr() { return _last_ocr; }
  static bool login_in_progress() { return _login_running != 0; }

 private:
  static UINT AutomationThreadFunction(LPVOID pParam);
  // Is the AUTOMATION button on for THIS instance? Reads the same
  // automation_api.enabled_<port> setting the toolbar tile writes.
  static bool AutomationEnabled();
  // Loads (once) the automation map for the mirror this instance is attached to.
  static bool EnsureMapLoaded();
  // Grabs the attached window's client area and OCRs one region of the automation map.
  // One grab per tick, then several regions read out of that same frame -- a screen
  // verdict has to come from points that agree, not from one rectangle.
  struct SScreenReading { CString button, email, password; };
  static bool Snapshot(cv::Mat *frame);
  static CString OcrRegionIn(const cv::Mat &frame, const char *region_name);
  static bool LooksLikeLoginScreen(const SScreenReading &r);
  static bool LooksLikeStack(const CString &text);
  static void LaunchLoginSequence();
  static UINT WaitForLoginProcess(LPVOID pParam);

 private:
  static CTablemap _map;             // the automation map (automation_<mirror>)
  static bool      _map_loaded;
  static bool      _dumped;          // wrote the debug frame once this run
  static CString   _last_ocr;
  static volatile LONG _login_running;
  static DWORD     _last_attempt_tick;

 private:
  HANDLE _m_stop_thread;
  HANDLE _m_wait_thread;
};

extern CAutomationHeartbeat *p_automation_heartbeat;

#endif  // INC_CAUTOMATIONHEARTBEAT_H
