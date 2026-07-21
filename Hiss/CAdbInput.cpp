//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: adb input injection for scrcpy-mirrored tables. See CAdbInput.h.
//
//******************************************************************************

#include "stdafx.h"
#include "CAdbInput.h"

#include <map>
#include <comdef.h>
#include <Wbemidl.h>

#include "..\DLLs\Debug_DLL\debug.h"
#include "..\DLLs\Preferences_DLL\Preferences.h"

#pragma comment(lib, "wbemuuid.lib")   // WMI, for resolving the adb serial behind a scrcpy window

namespace AdbInput {

namespace {

// All caches are per-process, so multiple Hiss instances each resolve their own window
// independently. Guarded because the autoplayer and the GUI thread can both get here.
CRITICAL_SECTION g_cs;
bool g_cs_ready = false;

class Lock {
 public:
  Lock() {
    if (!g_cs_ready) {            // races only during the first calls; both would init the
      InitializeCriticalSection(&g_cs);   // same section, so do it once under a flag
      g_cs_ready = true;
    }
    EnterCriticalSection(&g_cs);
  }
  ~Lock() { LeaveCriticalSection(&g_cs); }
};

// Empty string = "checked, and this window is not adb-backed" (negative caching matters:
// without it every click on a normal poker window would spawn a WMI query).
// Positive results are cached permanently -- the device behind a window never changes.
// NEGATIVES EXPIRE. Resolving the serial spawns a powershell (WMI) and can time out when the
// box is loaded; caching that "not adb-backed" forever silently demoted a live mirror to
// mouse-only for the whole session. Seen for real while two AVDs booted on 10 of 12 logical
// cores: every EMU/EMU2 mirror created during the boot storm was written off permanently,
// while the phones (resolved earlier, when idle) kept working.
struct CachedSerial { CString serial; DWORD stamp_ms; };
std::map<HWND, CachedSerial> g_serial_by_window;
const DWORD kNegativeSerialTtlMs = 15000;

// Device resolution is cached WITH A TTL, not forever. `wm size` is changed at runtime -- the
// EMU/EMU2 launcher scripts set it on every boot, and it can be re-issued by hand -- so a
// permanent cache silently breaks every tap after a resolution change: coordinates keep being
// mapped onto the OLD device size and land clamped at the screen edge. Seen for real when the
// AVDs went 1080x2340 -> 540x1170: taps computed as dev(954,278) on a 540-wide screen.
// One `adb shell wm size` per minute per device is nothing next to that failure mode.
struct CachedResolution { SIZE size; DWORD stamp_ms; };
std::map<CString, CachedResolution> g_res_by_serial;
const DWORD kResolutionTtlMs = 60000;

bool FileExists(const char *path) {
  DWORD attr = GetFileAttributesA(path);
  return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

CString AdbExePath() {
  const char *env_names[] = { "ANDROID_SDK_ROOT", "ANDROID_HOME" };
  char buf[MAX_PATH] = {0};
  for (int i = 0; i < 2; ++i) {
    DWORD n = GetEnvironmentVariableA(env_names[i], buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
      CString p; p.Format("%s\\platform-tools\\adb.exe", buf);
      if (FileExists(p.GetString())) return p;
    }
  }
  char profile[MAX_PATH] = {0};
  DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", profile, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    CString p; p.Format("%s\\Android\\Sdk\\platform-tools\\adb.exe", profile);
    if (FileExists(p.GetString())) return p;
  }
  return CString("adb");   // fall back to PATH
}

// Run a command line with no console window. If `out` != NULL, wait (<= timeout_ms) and
// capture stdout; otherwise fire-and-forget so the autoplayer is never blocked by a tap.
bool RunHidden(const CString &command_line, CString *out, DWORD timeout_ms) {
  SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
  HANDLE read_pipe = NULL, write_pipe = NULL;
  if (out != NULL) {
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return false;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
  }
  STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  if (out != NULL) {
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError  = write_pipe;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
  }
  PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
  CString mutable_cmd = command_line;
  BOOL ok = CreateProcessA(NULL, mutable_cmd.GetBuffer(), NULL, NULL,
                           (out != NULL) ? TRUE : FALSE, CREATE_NO_WINDOW,
                           NULL, NULL, &si, &pi);
  mutable_cmd.ReleaseBuffer();
  if (out != NULL) CloseHandle(write_pipe);
  if (!ok) { if (read_pipe) CloseHandle(read_pipe); return false; }
  if (out != NULL) {
    CString acc; char rb[512]; DWORD n = 0;
    while (ReadFile(read_pipe, rb, sizeof(rb) - 1, &n, NULL) && n > 0) { rb[n] = 0; acc += rb; }
    CloseHandle(read_pipe);
    WaitForSingleObject(pi.hProcess, timeout_ms);
    *out = acc;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

// Full command line of `pid`, via WMI in-process. The bot is 32-bit and scrcpy is 64-bit, so
// reading the PEB directly would need the Wow64 thunks -- ask WMI instead.
//
// DO NOT go back to spawning `powershell -Command "(Get-CimInstance ...)"` for this. Because
// the bot is 32-bit it gets the SysWOW64 host, which MEASURED 8.3-11.6s just to start and
// answer (the 64-bit host does the same query in 1.2-1.7s). That blew the timeout, and since
// a failed lookup reads as "this window is not a scrcpy mirror", the mirror was demoted to
// mouse clicks. EMU happened to squeak under the limit; EMU2 never did, so EMU2's clicks all
// went out through the mouse DLL. In-process WMI is ~100-300ms and cannot stall on process
// creation at all.
CString CommandLineViaWmi(DWORD pid) {
  CString result;
  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  // RPC_E_CHANGED_MODE = this thread already initialised COM in another apartment model; the
  // query still works, we just must not balance it with a CoUninitialize.
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return result;
  bool need_uninit = SUCCEEDED(hr);

  // Process-wide and only honoured once -- RPC_E_TOO_LATE just means someone got here first.
  CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT,
                       RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);

  IWbemLocator *locator = NULL;
  IWbemServices *services = NULL;
  IEnumWbemClassObject *enumerator = NULL;
  do {
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                                IID_IWbemLocator, (LPVOID *)&locator))) break;
    if (FAILED(locator->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL, 0, NULL, 0, 0,
                                      &services))) break;
    CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    CString query;
    query.Format("SELECT CommandLine FROM Win32_Process WHERE ProcessId=%lu", pid);
    if (FAILED(services->ExecQuery(_bstr_t(L"WQL"), _bstr_t(query.GetString()),
                                   WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                   NULL, &enumerator))) break;
    IWbemClassObject *object = NULL;
    ULONG returned = 0;
    // Bounded wait: this runs on the heartbeat thread, so never block it indefinitely.
    if (enumerator->Next(5000, 1, &object, &returned) == WBEM_S_NO_ERROR && returned == 1) {
      VARIANT value; VariantInit(&value);
      if (SUCCEEDED(object->Get(L"CommandLine", 0, &value, NULL, NULL))
          && value.vt == VT_BSTR && value.bstrVal != NULL) {
        result = CString(value.bstrVal);
      }
      VariantClear(&value);
      object->Release();
    }
  } while (false);

  if (enumerator != NULL) enumerator->Release();
  if (services != NULL) services->Release();
  if (locator != NULL) locator->Release();
  if (need_uninit) CoUninitialize();
  result.Trim();
  return result;
}

// Fallback only, for the case where WMI itself is unavailable. Slow (see above), so it must
// never be the normal path.
CString CommandLineOfProcess(DWORD pid) {
  CString cl = CommandLineViaWmi(pid);
  if (!cl.IsEmpty()) return cl;
  CString cmd;
  cmd.Format("powershell -NoProfile -NonInteractive -Command "
             "\"(Get-CimInstance Win32_Process -Filter 'ProcessId=%lu').CommandLine\"", pid);
  CString out;
  if (!RunHidden(cmd, &out, 20000)) return CString("");
  out.Trim();
  return out;
}

// The single connected device, if there is exactly one (scrcpy launched without -s).
CString SoleAdbDevice() {
  CString cmd; cmd.Format("\"%s\" devices", AdbExePath().GetString());
  CString out;
  if (!RunHidden(cmd, &out, 5000)) return CString("");
  CString found;
  int count = 0, pos = 0;
  while (pos < out.GetLength()) {
    int eol = out.Find('\n', pos);
    if (eol < 0) eol = out.GetLength();
    CString line = out.Mid(pos, eol - pos); line.Trim();
    pos = eol + 1;
    if (line.IsEmpty() || line.Find("List of devices") == 0) continue;
    int tab = line.FindOneOf(" \t");
    if (tab <= 0) continue;
    CString state = line.Mid(tab); state.Trim();
    if (state != "device") continue;          // skip offline / unauthorized
    found = line.Left(tab); ++count;
  }
  return (count == 1) ? found : CString("");
}

// Pull the serial out of a scrcpy command line: -s S, --serial S, or --serial=S.
CString ParseSerial(const CString &command_line) {
  static const char *kFlags[] = { "--serial=", "--serial ", "-s " };
  for (int i = 0; i < 3; ++i) {
    int at = command_line.Find(kFlags[i]);
    if (at < 0) continue;
    CString rest = command_line.Mid(at + (int)strlen(kFlags[i]));
    rest.TrimLeft();
    if (rest.IsEmpty()) continue;
    if (rest[0] == '"') {
      int close = rest.Find('"', 1);
      if (close > 1) return rest.Mid(1, close - 1);
      continue;
    }
    int end = rest.FindOneOf(" \t\r\n");
    return (end < 0) ? rest : rest.Left(end);
  }
  return CString("");
}

// adb serial backing `window`, or "" when it is not an adb-backed scrcpy mirror.
CString SerialForWindow(HWND window) {
  if (window == NULL || !IsWindow(window)) return CString("");
  Lock lock;
  std::map<HWND, CachedSerial>::const_iterator cached = g_serial_by_window.find(window);
  if (cached != g_serial_by_window.end()) {
    if (!cached->second.serial.IsEmpty()) return cached->second.serial;
    if ((GetTickCount() - cached->second.stamp_ms) < kNegativeSerialTtlMs) return CString("");
    // negative and stale -> fall through and re-resolve
  }

  CString serial;
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid != 0) {
    CString cl = CommandLineOfProcess(pid);
    // Only mirrors qualify. Without this guard a normal poker client with a single phone
    // plugged in would fall through to SoleAdbDevice() and get tapped on the wrong screen.
    //
    // Lowercase a COPY for that test. CString::MakeLower() mutates in place and returns a
    // reference, so testing `cl.MakeLower()` and then parsing `cl` parses the LOWERCASED
    // command line -- and adb serials are case-sensitive: `-s r58m50tb3by` dies with
    // "device not found" for R58M50TB3BY. Emulator serials (emulator-5554) are already
    // lowercase and survived it, so only the phones fell through to the mouse DLL.
    CString cl_lower = cl;
    cl_lower.MakeLower();
    if (cl_lower.Find("scrcpy") >= 0) {
      serial = ParseSerial(cl);
      if (serial.IsEmpty()) serial = SoleAdbDevice();
    }
  }
  CachedSerial cs; cs.serial = serial; cs.stamp_ms = GetTickCount();
  g_serial_by_window[window] = cs;
  // Always logged, NOT behind debug_autoplayer. This line and the tap/swipe lines below are
  // the only evidence of whether a click went out over adb or silently fell back to the mouse
  // DLL, and they fire once per window-resolution / per click -- not per heartbeat -- so they
  // cost nothing. They used to be gated on debug_autoplayer, which had to be left on to see
  // them; that same flag also drives a per-heartbeat "0 autoplayer buttons visible" flood, so
  // turning the flood off blinded the adb diagnostics too. Keep these two concerns separate.
  write_log(k_always_log_basic_information,
    "[AdbInput] window %p pid %lu -> %s\n", window, pid,
    serial.IsEmpty() ? "not adb-backed (using mouse DLL)" : serial.GetString());
  return serial;
}

// Live device resolution from `adb shell wm size`, cached per serial.
bool DeviceResolution(const CString &serial, int *dev_w, int *dev_h) {
  Lock lock;
  std::map<CString, CachedResolution>::const_iterator cached = g_res_by_serial.find(serial);
  if (cached != g_res_by_serial.end() &&
      (GetTickCount() - cached->second.stamp_ms) < kResolutionTtlMs) {
    *dev_w = cached->second.size.cx; *dev_h = cached->second.size.cy;
    return (*dev_w > 0 && *dev_h > 0);
  }
  CString cmd;
  cmd.Format("\"%s\" -s %s shell wm size", AdbExePath().GetString(), serial.GetString());
  CString out;
  if (!RunHidden(cmd, &out, 5000)) return false;
  int idx = out.Find("Override size:");          // the live logical size taps are in
  if (idx < 0) idx = out.Find("Physical size:");
  if (idx < 0) return false;
  int colon = out.Find(':', idx);
  if (colon < 0) return false;
  CString rest = out.Mid(colon + 1); rest.Trim();
  int xpos = rest.Find('x');
  if (xpos < 0) return false;
  int w = atoi(CString(rest.Left(xpos)).Trim());
  int h = atoi(CString(rest.Mid(xpos + 1)).Trim());
  if (w <= 0 || h <= 0) return false;
  CachedResolution cr;
  cr.size.cx = w; cr.size.cy = h; cr.stamp_ms = GetTickCount();
  g_res_by_serial[serial] = cr;
  *dev_w = w; *dev_h = h;
  return true;
}

// Map a client-area point of `window` to device pixels.
//
// scrcpy preserves the device aspect ratio and letter/pillar-boxes the video inside the
// client area with black bars, so the mapping must go through the actual VIDEO rect and
// not the whole client. Ignoring the bars drifts every tap: a 638x1000 window mirroring a
// 1080x2280 device has ~82px pillars, which throws a naive client->device scale ~120px
// off horizontally. When the aspects match the bars are 0 and this reduces to a plain scale.
bool ClientToDevice(HWND window, const CString &serial,
                    int client_x, int client_y, int *out_x, int *out_y) {
  RECT cr = {0};
  if (!GetClientRect(window, &cr)) return false;
  int client_w = cr.right - cr.left, client_h = cr.bottom - cr.top;
  if (client_w <= 0 || client_h <= 0) return false;
  int dev_w = 0, dev_h = 0;
  if (!DeviceResolution(serial, &dev_w, &dev_h)) return false;

  double dev_aspect    = (double)dev_w / dev_h;
  double client_aspect = (double)client_w / client_h;
  double video_w, video_h, bar_x, bar_y;
  if (client_aspect > dev_aspect) {          // window wider than device -> pillars left/right
    video_h = client_h;
    video_w = (double)client_h * dev_w / dev_h;
    bar_x = (client_w - video_w) / 2.0;
    bar_y = 0.0;
  } else {                                   // window taller than device -> bars top/bottom
    video_w = client_w;
    video_h = (double)client_w * dev_h / dev_w;
    bar_x = 0.0;
    bar_y = (client_h - video_h) / 2.0;
  }
  if (video_w <= 0 || video_h <= 0) return false;
  int dx = (int)(((double)client_x - bar_x) * dev_w / video_w + 0.5);
  int dy = (int)(((double)client_y - bar_y) * dev_h / video_h + 0.5);
  if (dx < 0) dx = 0; else if (dx > dev_w - 1) dx = dev_w - 1;
  if (dy < 0) dy = 0; else if (dy > dev_h - 1) dy = dev_h - 1;
  *out_x = dx; *out_y = dy;
  return true;
}

}  // namespace

bool IsAdbBackedWindow(HWND window) {
  return !SerialForWindow(window).IsEmpty();
}

bool TapRect(HWND window, RECT rect, int clicks) {
  CString serial = SerialForWindow(window);
  if (serial.IsEmpty()) return false;
  int cx = (rect.left + rect.right) / 2;
  int cy = (rect.top + rect.bottom) / 2;
  int dx = 0, dy = 0;
  if (!ClientToDevice(window, serial, cx, cy, &dx, &dy)) return false;
  if (clicks < 1) clicks = 1;
  CString taps;                       // chained in one shell so a multi-tap stays tight
  for (int i = 0; i < clicks; ++i) {
    if (i) taps += " ; ";
    CString one; one.Format("input tap %d %d", dx, dy);
    taps += one;
  }
  CString cmd;
  cmd.Format("\"%s\" -s %s shell %s",
             AdbExePath().GetString(), serial.GetString(), taps.GetString());
  write_log(k_always_log_basic_information,
    "[AdbInput] tap x%d %s client(%d,%d) -> dev(%d,%d)\n",
    clicks, serial.GetString(), cx, cy, dx, dy);
  return RunHidden(cmd, NULL, 0);
}

bool SwipeRect(HWND window, RECT rect, int duration_ms) {
  CString serial = SerialForWindow(window);
  if (serial.IsEmpty()) return false;
  int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  if (!ClientToDevice(window, serial, rect.left,  rect.top,    &x1, &y1)) return false;
  if (!ClientToDevice(window, serial, rect.right, rect.bottom, &x2, &y2)) return false;
  if (duration_ms <= 0) duration_ms = 250;
  CString cmd;
  cmd.Format("\"%s\" -s %s shell input swipe %d %d %d %d %d",
             AdbExePath().GetString(), serial.GetString(), x1, y1, x2, y2, duration_ms);
  write_log(k_always_log_basic_information,
    "[AdbInput] swipe %s client(%d,%d)->(%d,%d) dev(%d,%d)->(%d,%d) %dms\n",
    serial.GetString(), rect.left, rect.top, rect.right, rect.bottom,
    x1, y1, x2, y2, duration_ms);
  return RunHidden(cmd, NULL, 0);
}

}  // namespace AdbInput
