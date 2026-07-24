//*******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//*******************************************************************************
//
// Purpose:
//
//*******************************************************************************

// Needs to be defined here, before #include "window_functions.h"
// to generate proper export- and inport-definitions
#define WINDOW_FUNCTIONS_EXPORTS

#include "window_functions.h"
#include <assert.h>
#include <atlstr.h>
#include <windows.h>

const int k_messagebox_standard_flags = MB_OK | MB_TOPMOST;
const int k_messagebox_error_flags = k_messagebox_standard_flags | MB_ICONWARNING;

// ---- NON-BLOCKING ACCUMULATING ERROR WINDOW ---------------------------------------------------
//
// MessageBox() is MODAL and pumps messages on the CALLING thread. Every one of the 44 call sites
// that reaches here can therefore FREEZE THE BOT: an f$icm_prizeX warning raised from the heartbeat
// stopped play mid-hand behind an OK button, and because the condition recurred every evaluation,
// dismissing one immediately queued the next. A warning must never be able to stop the bot playing.
//
// So: errors are appended to a shared buffer and rendered by a DEDICATED THREAD that owns a small
// window with a read-only multi-line edit control. The caller appends and returns immediately.
// Repeats of an identical message bump a counter instead of adding another line, so a per-heartbeat
// warning cannot scroll the useful history away. [Emrald: non-blocking, accumulate into one modal]
static CRITICAL_SECTION g_err_cs;
static bool             g_err_cs_ready = false;
static CString          g_err_text;
static CString          g_err_last_line;
static int              g_err_repeat = 0;
static HWND             g_err_wnd = NULL;
static HWND             g_err_edit = NULL;
static HANDLE           g_err_thread = NULL;
static volatile LONG    g_err_dirty = 0;

static LRESULT CALLBACK ErrWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
  if (msg == WM_SIZE && g_err_edit != NULL) {
    ::MoveWindow(g_err_edit, 0, 0, LOWORD(l), HIWORD(l), TRUE);
    return 0;
  }
  if (msg == WM_CLOSE) { ::ShowWindow(h, SW_HIDE); return 0; }   // hide, never destroy
  return ::DefWindowProc(h, msg, w, l);
}

static DWORD WINAPI ErrWindowThread(LPVOID) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = ErrWndProc;
  wc.hInstance = ::GetModuleHandle(NULL);
  wc.hCursor = ::LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszClassName = "HissErrorLog";
  ::RegisterClassA(&wc);
  g_err_wnd = ::CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, "HissErrorLog",
    "Hiss - warnings (non-blocking)", WS_OVERLAPPEDWINDOW,
    60, 60, 760, 340, NULL, NULL, wc.hInstance, NULL);
  g_err_edit = ::CreateWindowExA(0, "EDIT", "",
    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
    0, 0, 744, 300, g_err_wnd, NULL, wc.hInstance, NULL);
  // (No WM_SETFONT: GetStockObject lives in gdi32, which this DLL does not link, and a monospaced
  // font is not worth adding a library dependency for.)
  for (;;) {
    if (::InterlockedExchange(&g_err_dirty, 0) != 0) {
      CString snapshot;
      ::EnterCriticalSection(&g_err_cs);
      snapshot = g_err_text;
      ::LeaveCriticalSection(&g_err_cs);
      ::SetWindowTextA(g_err_edit, snapshot.GetString());
      int len = ::GetWindowTextLengthA(g_err_edit);
      ::SendMessageA(g_err_edit, EM_SETSEL, len, len);      // keep the newest line in view
      ::SendMessageA(g_err_edit, EM_SCROLLCARET, 0, 0);
      if (!::IsWindowVisible(g_err_wnd)) ::ShowWindow(g_err_wnd, SW_SHOWNA);
    }
    MSG m;
    while (::PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) {
      ::TranslateMessage(&m); ::DispatchMessage(&m);
    }
    ::Sleep(150);
  }
}

void MessageBox_Error_Warning(const char*  Message, const char*  Title /* = "Error" */) {
  if (!g_err_cs_ready) {                      // first caller sets up; races are harmless here
    ::InitializeCriticalSection(&g_err_cs);
    g_err_cs_ready = true;
  }
  SYSTEMTIME st; ::GetLocalTime(&st);
  CString line;
  line.Format("%02d:%02d:%02d  [%s] %s", st.wHour, st.wMinute, st.wSecond,
              (Title != NULL ? Title : "Error"), (Message != NULL ? Message : ""));
  line.Replace("\n", " ");                    // one physical line per warning, so repeats collapse

  ::EnterCriticalSection(&g_err_cs);
  CString body = line.Mid(10);                // compare without the timestamp
  if (body == g_err_last_line) {
    ++g_err_repeat;                           // same warning again -> just count it
    int cut = g_err_text.ReverseFind('\r');
    if (cut >= 0) g_err_text = g_err_text.Left(cut);
    CString repeated;
    repeated.Format("\r\n%s   (x%d)", line.GetString(), g_err_repeat + 1);
    g_err_text += repeated;
  } else {
    g_err_last_line = body;
    g_err_repeat = 0;
    if (!g_err_text.IsEmpty()) g_err_text += "\r\n";
    g_err_text += line;
  }
  if (g_err_text.GetLength() > 60000) {       // bound it; keep the most recent half
    g_err_text = g_err_text.Right(30000);
  }
  ::LeaveCriticalSection(&g_err_cs);
  ::InterlockedExchange(&g_err_dirty, 1);

  if (g_err_thread == NULL) {
    g_err_thread = ::CreateThread(NULL, 0, ErrWindowThread, NULL, 0, NULL);
  }
  // RETURNS IMMEDIATELY -- the caller (often the heartbeat) keeps running.
}

int MessageBox_Interactive(const char* Message, const char* Title, int Flags) {
  return MessageBox(0, Message, Title, (Flags | k_messagebox_standard_flags));
}

// MessageBox for the msgbox$MESSAGE-command of OH-script
// Returns 0 as a result
void MessageBox_OH_Script_Messages(const char* message) {
  // Preprocess message
  CString CS_message(message);
  assert(CS_message.Left(7) == "msgbox$");
  CS_message.Replace("msgbox$", "");
  CS_message.Replace("_B", " ");
  CS_message.Replace("_C", ",");
  CS_message.Replace("_D", ".");
  CS_message.Replace("_N", "\n");
  // At the very last: underscores (to avoid incorrect future replacements)
  CS_message.Replace("_U", "_");
  MessageBox_Error_Warning(CS_message, "OH-Script Message");
}