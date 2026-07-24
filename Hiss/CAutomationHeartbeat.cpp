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

#include "stdafx.h"
#include "CAutomationHeartbeat.h"

#include "CAutoconnector.h"
#include "CAutoOcr.h"
#include "..\CTablemap\CTablemapDB.h"
#include "..\CTransform\CTransform.h"
#include "..\Shared\MagicNumbers\MagicNumbers.h"
#include "..\CTablemap\CTablemap.h"

CAutomationHeartbeat *p_automation_heartbeat = NULL;

CTablemap     CAutomationHeartbeat::_map;
bool          CAutomationHeartbeat::_map_loaded = false;
bool          CAutomationHeartbeat::_dumped = false;
CString       CAutomationHeartbeat::_last_ocr;
volatile LONG CAutomationHeartbeat::_login_running = 0;
DWORD         CAutomationHeartbeat::_last_attempt_tick = 0;

extern int g_terminal_port;   // this instance's identity, as used everywhere else

namespace {

// How often to look. The login screen is not a hurry -- and every tick costs a window
// grab plus one OCR, which has no business competing with the playing heartbeat.
const DWORD kTickMs = 5000;
// Do not fire again while the previous attempt may still be typing, and leave room for
// the client to actually load afterwards. The sequence itself takes ~40s.
const DWORD kRetryCooldownMs = 180000;

const char *kAutomationPrefsKey = "automation_api";
const char *kLoginButtonRegion  = "acr_login_button";



CString RepoPath() {
  char repo[MAX_PATH] = {0};
  if (::GetEnvironmentVariableA("HISS_REPO", repo, sizeof(repo) - 1) == 0)
    strcpy_s(repo, "C:\\www\\openholdembot_old");
  return CString(repo);
}

CString PerInstanceField(const char *field) {
  CString out;
  if (g_terminal_port > 0) out.Format("%s_%d", field, g_terminal_port);
  else                     out = field;
  return out;
}

// The mirror this instance drives, e.g. "A17" -> map "automation_a17".
CString MirrorTitle() {
  HWND h = p_autoconnector != NULL ? p_autoconnector->attached_hwnd() : NULL;
  if (h == NULL) return CString();
  char buf[256] = {0};
  ::GetWindowTextA(h, buf, sizeof(buf) - 1);
  return CString(buf);
}

CString MapNameForMirror() {
  CString title = MirrorTitle();
  if (title.IsEmpty()) return CString();
  title.MakeLower();
  CString name;
  name.Format("automation_%s", title.GetString());
  return name;
}

// Capture the attached window's client area. Its own grab rather than the scraper's:
// that bitmap is sized and refreshed for the PLAYING map, and borrowing it would couple
// this slow login check to the fast path it must never disturb.
bool GrabClientArea(cv::Mat *out) {
  if (out == NULL) return false;
  HWND h = p_autoconnector != NULL ? p_autoconnector->attached_hwnd() : NULL;
  if (h == NULL || !::IsWindow(h)) return false;

  RECT cr = {0};
  if (!::GetClientRect(h, &cr)) return false;
  const int w = cr.right - cr.left, ht = cr.bottom - cr.top;
  if (w <= 0 || ht <= 0) return false;         // minimised: nothing to read

  HDC screen = ::GetDC(NULL);
  HDC mem = ::CreateCompatibleDC(screen);
  BITMAPINFOHEADER bi = { sizeof(BITMAPINFOHEADER), w, -ht, 1, 32, BI_RGB };
  void *bits = NULL;
  HBITMAP dib = ::CreateDIBSection(mem, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
  bool ok = false;
  if (dib != NULL && bits != NULL) {
    HBITMAP old = (HBITMAP)::SelectObject(mem, dib);
    // PW_CLIENTONLY | PW_RENDERFULLCONTENT: the mirror renders through a compositor, and
    // without RENDERFULLCONTENT the grab comes back black.
    if (::PrintWindow(h, mem, 3)) {
      cv::Mat frame(ht, w, CV_8UC4);
      memcpy(frame.data, bits, (size_t)w * ht * 4);
      *out = frame.clone();
      ok = true;
    }
    ::SelectObject(mem, old);
  }
  if (dib != NULL) ::DeleteObject(dib);
  ::DeleteDC(mem);
  ::ReleaseDC(NULL, screen);
  return ok;
}

}  // namespace

CAutomationHeartbeat::CAutomationHeartbeat() {
  _m_stop_thread = CreateEvent(0, TRUE, FALSE, 0);
  _m_wait_thread = CreateEvent(0, TRUE, FALSE, 0);
}

CAutomationHeartbeat::~CAutomationHeartbeat() {
  ::SetEvent(_m_stop_thread);
  ::WaitForSingleObject(_m_wait_thread, k_max_time_to_wait_for_thread_to_shutdown);
  ::CloseHandle(_m_stop_thread);
  ::CloseHandle(_m_wait_thread);
  p_automation_heartbeat = NULL;
}

void CAutomationHeartbeat::StartThread() {
  write_log(k_always_log_basic_information,
            "[AUTOMATION] starting the automation (login) heartbeat\n");
  AfxBeginThread(AutomationThreadFunction, this);
}

void CAutomationHeartbeat::StopThread() {
  ::SetEvent(_m_stop_thread);
}

bool CAutomationHeartbeat::AutomationEnabled() {
  if (p_tablemap_db == NULL) return false;
  CString v = p_tablemap_db->GetSettingString(kAutomationPrefsKey,
                                              PerInstanceField("enabled"));
  v.Trim();
  return (v.CompareNoCase("on") == 0 || v == "1" || v.CompareNoCase("true") == 0);
}

bool CAutomationHeartbeat::EnsureMapLoaded() {
  if (_map_loaded) return true;

  const CString name = MapNameForMirror();
  if (name.IsEmpty()) return false;             // not attached to a mirror yet

  // The automation maps live in their OWN postgres schema, and the schema is fixed at
  // connect time, so this needs its own connection rather than the shared one. Set the
  // static, connect, then put it straight back so nothing else picks it up.
  CTablemapDB::SetSchema("automation");
  CTablemapDB db;
  const bool connected = db.Connect();
  CTablemapDB::SetSchema("");

  if (!connected) {
    write_log(k_always_log_basic_information,
              "[AUTOMATION] cannot reach the automation schema: %s\n",
              db.LastError().GetString());
    return false;
  }
  const int res = db.LoadTablemapFromDB(name, &_map);
  db.Disconnect();
  if (res != SUCCESS) {
    write_log(k_always_log_basic_information,
              "[AUTOMATION] no automation map named %s (err %d) -- nothing to do for this mirror\n",
              name.GetString(), res);
    return false;
  }
  _map_loaded = true;
  write_log(k_always_log_basic_information,
            "[AUTOMATION] loaded map %s with %d regions\n",
            name.GetString(), (int)_map.r$()->size());
  return true;
}

bool CAutomationHeartbeat::Snapshot(cv::Mat *frame) {
  if (p_auto_ocr == NULL) {
    if (_last_ocr != "<no-ocr>") {
      _last_ocr = "<no-ocr>";
      write_log(k_always_log_basic_information, "[AUTOMATION] the OCR engine is not up yet\n");
    }
    return false;
  }
  if (!GrabClientArea(frame)) {
    if (_last_ocr != "<no-grab>") {
      _last_ocr = "<no-grab>";
      write_log(k_always_log_basic_information,
                "[AUTOMATION] could not grab the mirror's client area\n");
    }
    return false;
  }
  // Once per run, keep the exact frame this heartbeat is reading. When its verdict
  // disagrees with what the mirror plainly shows, this is the only thing that says
  // whether it is looking at the wrong window or at the right one the wrong size.
  if (!_dumped) {
    _dumped = true;
    // The bot's own log directory (cwd is Release), NOT repo\logs -- that one does not
    // exist, and imwrite fails by RETURNING FALSE rather than throwing, so the first
    // version of this cheerfully logged a success that never happened.
    char path[MAX_PATH] = {0};
    sprintf_s(path, "logs\\automation_grab_%d.png", g_terminal_port);
    bool written = false;
    try {
      written = cv::imwrite(path, *frame);
    } catch (...) {
      written = false;
    }
    write_log(k_always_log_basic_information,
              "[AUTOMATION] %s what this heartbeat sees (%dx%d) -> %s\n",
              written ? "wrote" : "FAILED to write", frame->cols, frame->rows, path);
  }
  return true;
}

CString CAutomationHeartbeat::OcrRegionIn(const cv::Mat &frame, const char *region_name) {
  const RMap *regions = _map.r$();
  RMapCI it = regions->find(CString(region_name));
  if (it == regions->end()) return CString();

  const int l = it->second.left, t = it->second.top;
  const int w = it->second.right - it->second.left + 1;
  const int h = it->second.bottom - it->second.top + 1;
  if (w <= 0 || h <= 0) return CString();
  if (l < 0 || t < 0 || l + w > frame.cols || t + h > frame.rows) {
    write_log(k_always_log_basic_information,
              "[AUTOMATION] region %s (%d,%d %dx%d) falls outside the %dx%d window -- skipping\n",
              region_name, l, t, w, h, frame.cols, frame.rows);
    return CString();
  }
  cv::Mat crop = frame(cv::Rect(l, t, w, h)).clone();
  return p_auto_ocr->get_ocr_result(crop, it);
}

// Does the whole screen agree that this is the login page?
//
// One region cannot answer it. The login-button rectangle sits over the felt when the
// client is at a table, so "it did not say LOGIN" and "we are looking at the wrong
// window" produce identical silence -- and a single stray OCR hit could just as easily
// fire a login while the hero is in a hand. So read SEVERAL points of the same map and
// require them to corroborate:
//
//   * the button must read "login"
//   * a second field must look like the login form (an email, or an empty/masked box)
//   * nothing may look like a table -- a stack in BB is proof we are NOT logged out
//
// Anything short of that is treated as "not the login screen", which is the safe way to
// be wrong: the worst case is a client that stays logged out until the next tick.
bool CAutomationHeartbeat::LooksLikeLoginScreen(const SScreenReading &r) {
  CString button(r.button), email(r.email), password(r.password);
  button.Remove(' ');  button.MakeLower();
  email.Remove(' ');   email.MakeLower();
  password.Remove(' ');

  const bool button_says_login = (button.Find("login") >= 0) || (button.Find("logln") >= 0);
  const bool email_looks_right = (email.Find('@') >= 0) || email.IsEmpty();
  const bool password_looks_right = password.IsEmpty() || (password.Find('*') >= 0)
                                                       || (password.Find('.') >= 0);

  // A stack reads like "92BB" / "992BBB" -- digits with BB attached. Seeing that anywhere
  // in these three boxes means the felt is under them and the client is logged IN.
  const bool table_evidence = LooksLikeStack(r.button) || LooksLikeStack(r.email)
                                                       || LooksLikeStack(r.password);

  const int votes = (button_says_login ? 1 : 0) + (email_looks_right ? 1 : 0)
                  + (password_looks_right ? 1 : 0);
  return button_says_login && !table_evidence && votes >= 2;
}

bool CAutomationHeartbeat::LooksLikeStack(const CString &text) {
  CString t(text);
  t.Remove(' ');
  t.MakeLower();
  if (t.IsEmpty()) return false;
  const int bb = t.Find("bb");
  if (bb <= 0) return false;
  // digits immediately before the "bb"
  for (int i = 0; i < bb; ++i) {
    if (isdigit((unsigned char)t.GetAt(i))) return true;
  }
  return false;
}

void CAutomationHeartbeat::LaunchLoginSequence() {
  if (InterlockedCompareExchange(&_login_running, 1, 0) != 0) return;  // already going

  // Paths match what launch_hiss.py already uses for every other python helper.
  char py[MAX_PATH] = {0};
  CString repo_s = RepoPath();
  const char *repo = repo_s.GetString();
  if (::GetEnvironmentVariableA("HISS_PYTHONW", py, sizeof(py) - 1) == 0)
    strcpy_s(py, "C:\\Users\\scarl\\AppData\\Local\\Programs\\Python\\Python310\\pythonw.exe");

  CString line;
  line.Format("\"%s\" \"%s\\mcp\\automation_login.py\" --port %d", py, repo, g_terminal_port);

  STARTUPINFOA si = { sizeof(si) };
  PROCESS_INFORMATION pi = {0};
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  CStringA line_a(line);
  std::vector<char> buf(line_a.GetString(), line_a.GetString() + line_a.GetLength() + 1);

  write_log(k_always_log_basic_information, "[AUTOMATION] logging in: %s\n", line.GetString());
  if (::CreateProcessA(NULL, &buf[0], NULL, NULL, FALSE, CREATE_NO_WINDOW,
                       NULL, NULL, &si, &pi)) {
    ::CloseHandle(pi.hThread);
    // Wait on its own thread so the heartbeat keeps ticking; the flag clears when it ends.
    AfxBeginThread(WaitForLoginProcess, (LPVOID)pi.hProcess);
  } else {
    write_log(k_always_log_basic_information, "[AUTOMATION] could not start the login sequence (%d)\n",
              (int)::GetLastError());
    InterlockedExchange(&_login_running, 0);
  }
}

UINT CAutomationHeartbeat::WaitForLoginProcess(LPVOID pParam) {
  HANDLE h = (HANDLE)pParam;
  ::WaitForSingleObject(h, 300000);              // 5 min ceiling; the sequence takes ~40s
  DWORD code = 0;
  ::GetExitCodeProcess(h, &code);
  ::CloseHandle(h);
  write_log(k_always_log_basic_information,
            "[AUTOMATION] login sequence finished (exit %d)\n", (int)code);
  InterlockedExchange(&_login_running, 0);
  return 0;
}

UINT CAutomationHeartbeat::AutomationThreadFunction(LPVOID pParam) {
  CAutomationHeartbeat *parent = static_cast<CAutomationHeartbeat *>(pParam);
  assert(parent != NULL);

  while (::WaitForSingleObject(parent->_m_stop_thread, kTickMs) == WAIT_TIMEOUT) {
    if (!AutomationEnabled()) continue;          // the button is the switch; nothing else
    if (_login_running != 0) continue;
    if (_last_attempt_tick != 0 &&
        (::GetTickCount() - _last_attempt_tick) < kRetryCooldownMs) continue;
    if (!EnsureMapLoaded()) continue;

    cv::Mat frame;
    if (!Snapshot(&frame)) continue;

    SScreenReading r;
    r.button   = OcrRegionIn(frame, kLoginButtonRegion);
    r.email    = OcrRegionIn(frame, "acr_email");
    r.password = OcrRegionIn(frame, "acr_password");

    // Log every CHANGE in what the screen says -- silence here makes "the grab failed",
    // "OCR read nothing" and "it read something else entirely" look identical.
    CString summary;
    summary.Format("button='%s' email='%s' password='%s'",
                   r.button.GetString(), r.email.GetString(), r.password.GetString());
    if (summary != _last_ocr) {
      _last_ocr = summary;
      write_log(k_always_log_basic_information, "[AUTOMATION] screen: %s\n", summary.GetString());
    }
    if (!LooksLikeLoginScreen(r)) continue;

    write_log(k_always_log_basic_information,
              "[AUTOMATION] every read agrees this is the login screen (%s) -- logging in\n",
              summary.GetString());
    _last_attempt_tick = ::GetTickCount();
    LaunchLoginSequence();
  }

  ::SetEvent(parent->_m_wait_thread);
  return 0;
}
