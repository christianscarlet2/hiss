//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "libpq-fe.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")   // ExtractIconEx / ShellExecute
// Opt into Common-Controls v6 so buttons can show an image list (icon + text).
#pragma comment(linker, "\"/manifestdependency:type='win32' "             \
  "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "            \
  "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Custom icons (snake = Hiss, illuminati eye = Vision, barbell = trainer,
// rubber duck = this toolbar). Loaded from the source res/ folders at runtime.
static const char kDuckIcoPath[]    = "C:\\www\\openholdembot_old\\DeveloperToolbar\\res\\devtoolbar.ico";
static const char kSnakeIcoPath[]   = "C:\\www\\openholdembot_old\\Hiss\\res\\OpenHoldem.ico";
static const char kEyeIcoPath[]     = "C:\\www\\openholdembot_old\\Vision\\res\\OpenScrape.ico";
static const char kBarbellIcoPath[] = "C:\\www\\openholdembot_old\\trainer\\res\\trainer.ico";
static const char kFeatherIcoPath[] = "C:\\www\\openholdembot_old\\learner\\res\\learner.ico";

// Attach an HICON to a button via its image list (icon sits left of the text).
static void SetButtonIconHandle(HWND button, HICON icon) {
  if (button == NULL || icon == NULL) return;
  HIMAGELIST himl = ImageList_Create(18, 18, ILC_COLOR32 | ILC_MASK, 1, 1);
  ImageList_AddIcon(himl, icon);
  BUTTON_IMAGELIST bil = {0};
  bil.himl = himl;
  bil.margin.left = 4; bil.margin.right = 4;
  bil.uAlign = BUTTON_IMAGELIST_ALIGN_LEFT;
  SendMessage(button, BCM_SETIMAGELIST, 0, (LPARAM)&bil);
}

// Put a small icon (kept with the button's text) on a button via its image list.
static void SetButtonIcon(HWND button, const char *ico_path) {
  if (button == NULL) return;
  HICON icon = (HICON)LoadImageA(NULL, ico_path, IMAGE_ICON, 18, 18, LR_LOADFROMFILE);
  if (icon == NULL) return;
  SetButtonIconHandle(button, icon);
  DestroyIcon(icon);
}

// Clever: use a program's OWN embedded icon for its launch button (so the MD Viewer
// button shows the actual MarkdownViewer icon). Falls back to a stock document icon.
static void SetButtonIconFromExe(HWND button, const char *exe_path) {
  if (button == NULL) return;
  HICON small_icon = NULL;
  ExtractIconExA(exe_path, 0, NULL, &small_icon, 1);
  if (small_icon == NULL) small_icon = ExtractIconA(GetModuleHandle(NULL), exe_path, 0);
  if (small_icon == NULL) {
    small_icon = (HICON)LoadImageA(NULL, IDI_INFORMATION, IMAGE_ICON, 18, 18, LR_SHARED);
  }
  if (small_icon == NULL) return;
  SetButtonIconHandle(button, small_icon);
  DestroyIcon(small_icon);
}

#define IDC_WIDTH_EDIT 1001
#define IDC_HEIGHT_EDIT 1002
#define IDC_PICK_BUTTON 1003
#define IDC_STATUS_TEXT 1004
#define IDC_SCALE_CHECKBOX 1005
#define IDC_BUILD_BUTTON 1006
#define IDC_BUILD_PROGRESS 1007
#define IDC_OPEN_OPENSCRAPE_BUTTON 1008
#define IDC_OPEN_OPENHOLDEM_BUTTON 1009
#define IDC_ALERT_TEXT 1010
#define IDC_CLOSE_ALL_BUTTON 1011
#define IDC_OPEN_SCRCPY_BUTTON 1012
#define IDC_OPEN_TRAINER_BUTTON 1013
#define IDC_REC_SCRCPY_BUTTON 1014
#define IDC_OPEN_LEARNER_BUTTON 1015
#define IDC_OPEN_MDVIEWER_BUTTON 1016

#define TIMER_WINDOW_MONITOR 2001

#define WM_APP_STATUS (WM_APP + 1)
#define WM_APP_PROGRESS (WM_APP + 2)
#define WM_APP_BUILD_DONE (WM_APP + 3)

static const char kWindowClassName[] = "HissDeveloperToolbar";
static const char kAppTitle[] = "Developer Toolbar";
static const char kScrcpyPath[] = "C:\\www\\scrcpy-win64-v4.0\\scrcpy.exe";
static const char kMdViewerPath[] = "C:\\www\\mdviewer\\dist\\MarkdownViewer.exe";
static const char kPlansDir[] = "C:\\Users\\scarl\\.claude\\plans";
static HWND g_main_window = NULL;
static HWND g_width_edit = NULL;
static HWND g_height_edit = NULL;
static HWND g_pick_button = NULL;
static HWND g_status_text = NULL;
static HWND g_scale_checkbox = NULL;
static HWND g_build_button = NULL;
static HWND g_open_openscrape_button = NULL;
static HWND g_open_openholdem_button = NULL;
static HWND g_open_scrcpy_button = NULL;
static HWND g_open_trainer_button = NULL;
static HWND g_open_learner_button = NULL;
static HWND g_open_mdviewer_button = NULL;
static HWND g_rec_scrcpy_button = NULL;
static HWND g_close_all_button = NULL;
static HWND g_build_progress = NULL;
static HWND g_alert_text = NULL;
static HBRUSH g_alert_brush = NULL;
static HINSTANCE g_instance = NULL;
static bool g_picking_window = false;
static volatile LONG g_building = 0;
static HWND g_monitored_window = NULL;
static int g_monitored_width = 0;
static int g_monitored_height = 0;

typedef BOOL (WINAPI *SetProcessDpiAwarenessContextFunction)(DPI_AWARENESS_CONTEXT);
typedef UINT (WINAPI *GetDpiForWindowFunction)(HWND);
typedef UINT (WINAPI *GetDpiForSystemFunction)(void);
typedef BOOL (WINAPI *QueryFullProcessImageNameFunction)(HANDLE, DWORD, LPSTR, PDWORD);

static void SetStatusText(const char *message) {
  SetWindowText(g_status_text, message);
}

static void SetAlertText(const char *message) {
  SetWindowText(g_alert_text, message);
  ShowWindow(g_alert_text, SW_SHOW);
  InvalidateRect(g_alert_text, NULL, TRUE);
}

static void ClearAlertText() {
  SetWindowText(g_alert_text, "");
  ShowWindow(g_alert_text, SW_HIDE);
}

static void PostStatusText(const char *message) {
  char *copy = _strdup(message);
  if (copy != NULL) {
    PostMessage(g_main_window, WM_APP_STATUS, 0, (LPARAM)copy);
  }
}

static void PostProgress(int position, int maximum) {
  PostMessage(g_main_window, WM_APP_PROGRESS, (WPARAM)position, (LPARAM)maximum);
}

static bool EndsWithSlash(const std::string &path) {
  return !path.empty() && (path[path.length() - 1] == '\\' || path[path.length() - 1] == '/');
}

static std::string JoinPath(const std::string &left, const std::string &right) {
  if (left.empty() || EndsWithSlash(left)) {
    return left + right;
  }
  return left + "\\" + right;
}

static bool DirectoryExists(const std::string &path) {
  DWORD attributes = GetFileAttributes(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static bool FileExists(const std::string &path) {
  DWORD attributes = GetFileAttributes(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string Quote(const std::string &text) {
  return "\"" + text + "\"";
}

static bool IsTablemapFile(const char *name) {
  const char *dot = strrchr(name, '.');
  return dot != NULL && _stricmp(dot, ".tm") == 0;
}

static bool FileTimeIsNewer(const FILETIME &left, const FILETIME &right) {
  return CompareFileTime(&left, &right) > 0;
}

static bool GetFileWriteTime(const std::string &path, FILETIME *time) {
  WIN32_FILE_ATTRIBUTE_DATA data = {0};
  if (!GetFileAttributesEx(path.c_str(), GetFileExInfoStandard, &data)) {
    return false;
  }
  *time = data.ftLastWriteTime;
  return true;
}

static void FindLatestTablemapInDirectory(const std::string &directory,
    std::string *latest_path, FILETIME *latest_time) {
  if (!DirectoryExists(directory)) {
    return;
  }

  const std::string search_spec = JoinPath(directory, "*");
  WIN32_FIND_DATA find_data = {0};
  HANDLE find = FindFirstFile(search_spec.c_str(), &find_data);
  if (find == INVALID_HANDLE_VALUE) {
    return;
  }

  do {
    if (strcmp(find_data.cFileName, ".") == 0 || strcmp(find_data.cFileName, "..") == 0) {
      continue;
    }

    const std::string child_path = JoinPath(directory, find_data.cFileName);
    if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      FindLatestTablemapInDirectory(child_path, latest_path, latest_time);
    } else if (IsTablemapFile(find_data.cFileName)
        && (latest_path->empty() || FileTimeIsNewer(find_data.ftLastWriteTime, *latest_time))) {
      *latest_path = child_path;
      *latest_time = find_data.ftLastWriteTime;
    }
  } while (FindNextFile(find, &find_data));

  FindClose(find);
}

static void EnableProcessDpiAwareness() {
  HMODULE user32 = GetModuleHandle("user32.dll");
  if (user32 != NULL) {
    SetProcessDpiAwarenessContextFunction set_process_dpi_awareness_context =
      (SetProcessDpiAwarenessContextFunction)GetProcAddress(
        user32, "SetProcessDpiAwarenessContext");
    if ((set_process_dpi_awareness_context != NULL)
        && set_process_dpi_awareness_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
      return;
    }
  }

  SetProcessDPIAware();
}

static UINT DpiForWindow(HWND hwnd) {
  HMODULE user32 = GetModuleHandle("user32.dll");
  if (user32 != NULL) {
    GetDpiForWindowFunction get_dpi_for_window =
      (GetDpiForWindowFunction)GetProcAddress(user32, "GetDpiForWindow");
    if (get_dpi_for_window != NULL) {
      const UINT dpi = get_dpi_for_window(hwnd);
      if (dpi != 0) {
        return dpi;
      }
    }

    GetDpiForSystemFunction get_dpi_for_system =
      (GetDpiForSystemFunction)GetProcAddress(user32, "GetDpiForSystem");
    if (get_dpi_for_system != NULL) {
      const UINT dpi = get_dpi_for_system();
      if (dpi != 0) {
        return dpi;
      }
    }
  }

  HDC screen_dc = GetDC(NULL);
  if (screen_dc == NULL) {
    return 96;
  }
  const UINT dpi = GetDeviceCaps(screen_dc, LOGPIXELSX);
  ReleaseDC(NULL, screen_dc);
  return dpi == 0 ? 96 : dpi;
}

static bool ScaleToMonitorDpi() {
  return SendMessage(g_scale_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static int ScaleDimensionForDpi(int value, UINT dpi) {
  return MulDiv(value, (int)dpi, 96);
}

static bool ReadPositiveInteger(HWND edit, int *value) {
  char text[32] = {0};
  GetWindowText(edit, text, sizeof(text));

  char *end = NULL;
  const long parsed = strtol(text, &end, 10);
  if ((end == text) || (*end != '\0') || (parsed <= 0) || (parsed > 32767)) {
    return false;
  }

  *value = (int)parsed;
  return true;
}

static HWND TopLevelWindowFromPoint(POINT point) {
  HWND target = WindowFromPoint(point);
  if (target == NULL) {
    return NULL;
  }

  HWND root = GetAncestor(target, GA_ROOT);
  if (root != NULL) {
    target = root;
  }

  if (target == g_main_window) {
    return NULL;
  }

  return target;
}

static void StopWindowMonitor() {
  KillTimer(g_main_window, TIMER_WINDOW_MONITOR);
  g_monitored_window = NULL;
  g_monitored_width = 0;
  g_monitored_height = 0;
}

static void StartWindowMonitor(HWND target, int width, int height) {
  g_monitored_window = target;
  g_monitored_width = width;
  g_monitored_height = height;
  ClearAlertText();
  SetTimer(g_main_window, TIMER_WINDOW_MONITOR, 500, NULL);
}

static void PollMonitoredWindowSize() {
  if (g_monitored_window == NULL) {
    return;
  }

  if (!IsWindow(g_monitored_window)) {
    SetAlertText("ALERT: selected window disappeared after resize.");
    StopWindowMonitor();
    return;
  }

  RECT current_rect = {0};
  if (!GetWindowRect(g_monitored_window, &current_rect)) {
    SetAlertText("ALERT: selected window size cannot be read.");
    return;
  }

  const int current_width = current_rect.right - current_rect.left;
  const int current_height = current_rect.bottom - current_rect.top;
  if (current_width != g_monitored_width || current_height != g_monitored_height) {
    char message[192] = {0};
    sprintf_s(message, "ALERT: selected window changed size to %d x %d; expected %d x %d.",
      current_width, current_height, g_monitored_width, g_monitored_height);
    SetAlertText(message);
    return;
  }

  ClearAlertText();
}

static void StopPickingWindow() {
  if (!g_picking_window) {
    return;
  }

  g_picking_window = false;
  ReleaseCapture();
  SetCursor(LoadCursor(NULL, IDC_ARROW));
  EnableWindow(g_pick_button, TRUE);
}

static void StartPickingWindow() {
  int width = 0;
  int height = 0;
  if (!ReadPositiveInteger(g_width_edit, &width)
      || !ReadPositiveInteger(g_height_edit, &height)) {
    MessageBox(g_main_window,
      "Enter a positive width and height first.",
      kAppTitle, MB_OK | MB_ICONWARNING | MB_TOPMOST);
    return;
  }

  g_picking_window = true;
  SetCapture(g_main_window);
  SetCursor(LoadCursor(NULL, IDC_CROSS));
  EnableWindow(g_pick_button, FALSE);
  SetStatusText("Click the window to resize. Press Esc to cancel.");
}

static void ResizeClickedWindow(LPARAM lparam) {
  int width = 0;
  int height = 0;
  if (!ReadPositiveInteger(g_width_edit, &width)
      || !ReadPositiveInteger(g_height_edit, &height)) {
    StopPickingWindow();
    MessageBox(g_main_window,
      "Enter a positive width and height first.",
      kAppTitle, MB_OK | MB_ICONWARNING | MB_TOPMOST);
    return;
  }

  POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
  ClientToScreen(g_main_window, &point);
  HWND target = TopLevelWindowFromPoint(point);
  if (target == NULL) {
    StopPickingWindow();
    SetStatusText("No target selected.");
    return;
  }

  RECT current_rect = {0};
  if (!GetWindowRect(target, &current_rect)) {
    StopPickingWindow();
    SetStatusText("Could not read target window position.");
    return;
  }

  const UINT dpi = DpiForWindow(target);
  const bool scale_to_dpi = ScaleToMonitorDpi();
  const int scaled_width = scale_to_dpi ? ScaleDimensionForDpi(width, dpi) : width;
  const int scaled_height = scale_to_dpi ? ScaleDimensionForDpi(height, dpi) : height;

  const BOOL resized = SetWindowPos(target, NULL,
    current_rect.left, current_rect.top, scaled_width, scaled_height,
    SWP_NOZORDER | SWP_NOACTIVATE);

  StopPickingWindow();
  if (resized) {
    StartWindowMonitor(target, scaled_width, scaled_height);
    char message[192] = {0};
    if (scale_to_dpi) {
      sprintf_s(message, "Resized HWND 0x%p to %d x %d at %u DPI (%d x %d physical).",
        target, width, height, dpi, scaled_width, scaled_height);
    } else {
      sprintf_s(message, "Resized HWND 0x%p to %d x %d physical pixels.",
        target, scaled_width, scaled_height);
    }
    SetStatusText(message);
  } else {
    SetStatusText("Could not resize the selected window.");
  }
}

static std::string ExeDirectory() {
  char path[MAX_PATH] = {0};
  GetModuleFileName(NULL, path, MAX_PATH);
  char *last_slash = strrchr(path, '\\');
  if (last_slash != NULL) {
    *last_slash = '\0';
  }
  return path;
}

static std::string ParentDirectory(const std::string &path) {
  std::string trimmed = path;
  while (trimmed.length() > 3 && EndsWithSlash(trimmed)) {
    trimmed.erase(trimmed.length() - 1);
  }
  const size_t slash = trimmed.find_last_of("\\/");
  if (slash == std::string::npos) {
    return trimmed;
  }
  return trimmed.substr(0, slash);
}

static std::string FileNameOnly(const std::string &path) {
  const size_t slash = path.find_last_of("\\/");
  if (slash == std::string::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

static std::string FindRepoRoot() {
  std::string candidate = ExeDirectory();
  for (int i = 0; i < 5; ++i) {
    if (FileExists(JoinPath(candidate, "Hiss.sln"))) {
      return candidate;
    }
    candidate = ParentDirectory(candidate);
  }
  return ParentDirectory(ExeDirectory());
}

static std::string FindDefaultTablemap(const std::string &repo_root) {
  const char *preferred_directories[] = {
    "Release\\scraper",
    "Debug\\scraper",
    "##_Hiss_Release_Directory_##\\scraper",
    "##_Tablemaps_##\\scraper"
  };

  for (int i = 0; i < (int)(sizeof(preferred_directories) / sizeof(preferred_directories[0])); ++i) {
    std::string latest_path;
    FILETIME latest_time = {0};
    FindLatestTablemapInDirectory(JoinPath(repo_root, preferred_directories[i]),
      &latest_path, &latest_time);
    if (!latest_path.empty()) {
      return latest_path;
    }
  }

  return "";
}

static bool ParseTablemapSizeRecord(const char *line, const char *record_name,
    int *width, int *height) {
  char parsed_name[128] = {0};
  int parsed_width = 0;
  int parsed_height = 0;

  if (sscanf_s(line, " z$%127s %d %d", parsed_name, (unsigned)_countof(parsed_name),
      &parsed_width, &parsed_height) != 3) {
    return false;
  }

  if (_stricmp(parsed_name, record_name) != 0 || parsed_width <= 0 || parsed_height <= 0) {
    return false;
  }

  *width = parsed_width;
  *height = parsed_height;
  return true;
}

static bool ReadTablemapSize(const std::string &tablemap_path,
    int *width, int *height, char *size_record, size_t size_record_length) {
  FILE *file = NULL;
  if (fopen_s(&file, tablemap_path.c_str(), "r") != 0 || file == NULL) {
    return false;
  }

  int first_width = 0;
  int first_height = 0;
  char first_record[32] = {0};
  int clientsizemax_width = 0;
  int clientsizemax_height = 0;
  int clientsizemin_width = 0;
  int clientsizemin_height = 0;

  char line[512] = {0};
  while (fgets(line, sizeof(line), file) != NULL) {
    int parsed_width = 0;
    int parsed_height = 0;
    if (ParseTablemapSizeRecord(line, "targetsize", &parsed_width, &parsed_height)) {
      fclose(file);
      *width = parsed_width;
      *height = parsed_height;
      strcpy_s(size_record, size_record_length, "targetsize");
      return true;
    }
    if (clientsizemax_width == 0
        && ParseTablemapSizeRecord(line, "clientsizemax", &clientsizemax_width, &clientsizemax_height)) {
      continue;
    }
    if (clientsizemin_width == 0
        && ParseTablemapSizeRecord(line, "clientsizemin", &clientsizemin_width, &clientsizemin_height)) {
      continue;
    }
    if (first_width == 0) {
      char parsed_name[128] = {0};
      if (sscanf_s(line, " z$%127s %d %d", parsed_name, (unsigned)_countof(parsed_name),
          &parsed_width, &parsed_height) == 3 && parsed_width > 0 && parsed_height > 0) {
        first_width = parsed_width;
        first_height = parsed_height;
        strcpy_s(first_record, sizeof(first_record), parsed_name);
      }
    }
  }

  fclose(file);

  if (clientsizemax_width > 0) {
    *width = clientsizemax_width;
    *height = clientsizemax_height;
    strcpy_s(size_record, size_record_length, "clientsizemax");
    return true;
  }
  if (clientsizemin_width > 0) {
    *width = clientsizemin_width;
    *height = clientsizemin_height;
    strcpy_s(size_record, size_record_length, "clientsizemin");
    return true;
  }
  if (first_width > 0) {
    *width = first_width;
    *height = first_height;
    strcpy_s(size_record, size_record_length, first_record);
    return true;
  }

  return false;
}

static void LoadDefaultTablemapSize() {
  const std::string repo_root = FindRepoRoot();
  const std::string tablemap_path = FindDefaultTablemap(repo_root);
  if (tablemap_path.empty()) {
    SetStatusText("No tablemap found in the scraper folders.");
    return;
  }

  int width = 0;
  int height = 0;
  char size_record[64] = {0};
  if (!ReadTablemapSize(tablemap_path, &width, &height, size_record, sizeof(size_record))) {
    SetStatusText("Could not read a size record from the tablemap.");
    return;
  }

  char text[32] = {0};
  sprintf_s(text, "%d", width);
  SetWindowText(g_width_edit, text);
  sprintf_s(text, "%d", height);
  SetWindowText(g_height_edit, text);

  char status[512] = {0};
  sprintf_s(status, "Loaded %s from %s.", size_record, tablemap_path.c_str());
  SetStatusText(status);
}

static std::string FindMSBuild() {
  const char *candidates[] = {
    "C:\\Program Files\\Microsoft Visual Studio\\18\\Insiders\\MSBuild\\Current\\Bin\\MSBuild.exe",
    "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\MSBuild\\Current\\Bin\\MSBuild.exe",
    "C:\\Program Files\\Microsoft Visual Studio\\18\\Professional\\MSBuild\\Current\\Bin\\MSBuild.exe",
    "C:\\Program Files\\Microsoft Visual Studio\\18\\Enterprise\\MSBuild\\Current\\Bin\\MSBuild.exe",
    "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\MSBuild\\Current\\Bin\\MSBuild.exe",
    "C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\MSBuild\\Current\\Bin\\MSBuild.exe",
    "C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\MSBuild\\Current\\Bin\\MSBuild.exe",
    "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Community\\MSBuild\\Current\\Bin\\MSBuild.exe",
    "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Professional\\MSBuild\\Current\\Bin\\MSBuild.exe",
    "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Enterprise\\MSBuild\\Current\\Bin\\MSBuild.exe"
  };
  for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); ++i) {
    if (FileExists(candidates[i])) {
      return candidates[i];
    }
  }
  return "MSBuild.exe";
}

static bool RunProcessAndWait(const std::string &command_line, const std::string &working_directory) {
  STARTUPINFO startup_info = {0};
  PROCESS_INFORMATION process_info = {0};
  startup_info.cb = sizeof(startup_info);

  std::vector<char> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back('\0');

  BOOL created = CreateProcess(NULL, &mutable_command[0], NULL, NULL, FALSE,
    CREATE_NO_WINDOW, NULL, working_directory.c_str(), &startup_info, &process_info);
  if (!created) {
    return false;
  }

  WaitForSingleObject(process_info.hProcess, INFINITE);

  DWORD exit_code = 1;
  GetExitCodeProcess(process_info.hProcess, &exit_code);
  CloseHandle(process_info.hThread);
  CloseHandle(process_info.hProcess);
  return exit_code == 0;
}

static bool BuildReleaseOptimized(const std::string &repo_root) {
  const std::string solution = JoinPath(repo_root, "Hiss.sln");
  const std::string command_line = Quote(FindMSBuild())
    + " " + Quote(solution)
    + " /p:Configuration=\"Release - Optimized\" /p:Platform=Win32 /m /v:minimal";
  return RunProcessAndWait(command_line, repo_root);
}

static bool GetProcessImagePath(DWORD process_id, std::string *path) {
  HMODULE kernel32 = GetModuleHandle("kernel32.dll");
  if (kernel32 == NULL) {
    return false;
  }

  QueryFullProcessImageNameFunction query_image_name =
    (QueryFullProcessImageNameFunction)GetProcAddress(kernel32, "QueryFullProcessImageNameA");
  if (query_image_name == NULL) {
    return false;
  }

  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
  if (process == NULL) {
    return false;
  }

  char image_path[MAX_PATH] = {0};
  DWORD size = MAX_PATH;
  const BOOL queried = query_image_name(process, 0, image_path, &size);
  CloseHandle(process);
  if (!queried) {
    return false;
  }

  *path = image_path;
  return true;
}

static bool PathStartsWithNoCase(const std::string &path, const std::string &prefix) {
  if (path.length() < prefix.length()) {
    return false;
  }
  return _strnicmp(path.c_str(), prefix.c_str(), prefix.length()) == 0;
}

static bool LooksLikeRepoOutputProcess(const PROCESSENTRY32 &entry, const std::string &repo_root) {
  std::string path;
  if (GetProcessImagePath(entry.th32ProcessID, &path)) {
    return PathStartsWithNoCase(path, JoinPath(repo_root, "Release"))
      || PathStartsWithNoCase(path, JoinPath(repo_root, "Release - Optimized"))
      || PathStartsWithNoCase(path, JoinPath(repo_root, "Debug"));
  }

  const char *known_names[] = {
    "Hiss.exe", "Vision.exe", "OHReplay.exe", "ManualMode.exe",
    "DeveloperToolbar.exe", "WindowSizer.exe", "OpenReplayShooter.exe"
  };
  for (int i = 0; i < (int)(sizeof(known_names) / sizeof(known_names[0])); ++i) {
    if (_stricmp(entry.szExeFile, known_names[i]) == 0) {
      return true;
    }
  }
  return false;
}

static int KillRepoOutputProcesses(const std::string &repo_root) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return 0;
  }

  int killed = 0;
  PROCESSENTRY32 entry = {0};
  entry.dwSize = sizeof(entry);
  if (Process32First(snapshot, &entry)) {
    do {
      if (entry.th32ProcessID == GetCurrentProcessId()) {
        continue;
      }
      if (!LooksLikeRepoOutputProcess(entry, repo_root)) {
        continue;
      }

      HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
      if (process != NULL) {
        if (TerminateProcess(process, 1)) {
          ++killed;
        }
        CloseHandle(process);
      }
    } while (Process32Next(snapshot, &entry));
  }

  CloseHandle(snapshot);
  return killed;
}

static bool IsOpenHoldemOrOpenScrapeProcess(const PROCESSENTRY32 &entry) {
  return _stricmp(entry.szExeFile, "Hiss.exe") == 0
    || _stricmp(entry.szExeFile, "Vision.exe") == 0
    || _stricmp(entry.szExeFile, "trainer.exe") == 0;
}

struct ProcessWindowSearch {
  DWORD process_id;
  HWND window;
};

static BOOL CALLBACK FindMainWindowForProcessProc(HWND window, LPARAM param) {
  ProcessWindowSearch *search = (ProcessWindowSearch*)param;
  DWORD window_process_id = 0;
  GetWindowThreadProcessId(window, &window_process_id);
  if (window_process_id != search->process_id) {
    return TRUE;
  }
  if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != NULL) {
    return TRUE;
  }

  search->window = window;
  return FALSE;
}

static HWND FindMainWindowForProcess(DWORD process_id) {
  ProcessWindowSearch search = {0};
  search.process_id = process_id;
  EnumWindows(FindMainWindowForProcessProc, (LPARAM)&search);
  return search.window;
}

static bool SendCtrlSToWindow(HWND window) {
  if (!IsWindow(window)) {
    return false;
  }

  ShowWindow(window, SW_RESTORE);
  SetForegroundWindow(window);
  Sleep(150);

  INPUT inputs[4] = {0};
  inputs[0].type = INPUT_KEYBOARD;
  inputs[0].ki.wVk = VK_CONTROL;
  inputs[1].type = INPUT_KEYBOARD;
  inputs[1].ki.wVk = 'S';
  inputs[2].type = INPUT_KEYBOARD;
  inputs[2].ki.wVk = 'S';
  inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
  inputs[3].type = INPUT_KEYBOARD;
  inputs[3].ki.wVk = VK_CONTROL;
  inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

  const UINT sent = SendInput((UINT)(sizeof(inputs) / sizeof(inputs[0])), inputs, sizeof(INPUT));
  Sleep(500);
  return sent == (UINT)(sizeof(inputs) / sizeof(inputs[0]));
}

static int SaveOpenScrapeTablemapsBeforeClose() {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return 0;
  }

  int saved = 0;
  PROCESSENTRY32 entry = {0};
  entry.dwSize = sizeof(entry);
  if (Process32First(snapshot, &entry)) {
    do {
      if (_stricmp(entry.szExeFile, "Vision.exe") != 0) {
        continue;
      }

      HWND window = FindMainWindowForProcess(entry.th32ProcessID);
      if (window != NULL && SendCtrlSToWindow(window)) {
        ++saved;
      }
    } while (Process32Next(snapshot, &entry));
  }

  CloseHandle(snapshot);
  return saved;
}

static int ForceCloseOpenHoldemAndOpenScrape() {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return 0;
  }

  int killed = 0;
  PROCESSENTRY32 entry = {0};
  entry.dwSize = sizeof(entry);
  if (Process32First(snapshot, &entry)) {
    do {
      if (!IsOpenHoldemOrOpenScrapeProcess(entry)) {
        continue;
      }

      HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
      if (process != NULL) {
        if (TerminateProcess(process, 1)) {
          ++killed;
        }
        CloseHandle(process);
      }
    } while (Process32Next(snapshot, &entry));
  }

  CloseHandle(snapshot);
  return killed;
}

static void CloseAllOpenHoldemAndOpenScrape() {
  // No autosave on close: just terminate the processes (Hiss/Vision/Trainer).
  const int killed = ForceCloseOpenHoldemAndOpenScrape();
  char status[160] = {0};
  sprintf_s(status, "Closed %d Hiss/Vision/Trainer process%s.",
    killed, killed == 1 ? "" : "es");
  SetStatusText(status);
}

static bool AskToKillBlockingProcesses(const char *step, const std::string &repo_root) {
  char message[512] = {0};
  sprintf_s(message,
    "%s failed. A running Hiss/Vision/DeveloperToolbar process may be blocking the output files.\r\n\r\nKill blocking repo output processes and retry?",
    step);

  const int answer = MessageBox(g_main_window, message, kAppTitle,
    MB_YESNO | MB_ICONWARNING | MB_TOPMOST);
  if (answer != IDYES) {
    return false;
  }

  const int killed = KillRepoOutputProcesses(repo_root);
  char status[160] = {0};
  sprintf_s(status, "Killed %d blocking process%s. Retrying...", killed, killed == 1 ? "" : "es");
  PostStatusText(status);
  Sleep(750);
  return true;
}

static bool EnsureDirectoryTree(const std::string &path) {
  if (DirectoryExists(path)) {
    return true;
  }

  std::string parent = ParentDirectory(path);
  if (parent != path && !parent.empty() && !DirectoryExists(parent)) {
    if (!EnsureDirectoryTree(parent)) {
      return false;
    }
  }

  return CreateDirectory(path.c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static void CollectFiles(const std::string &root, const std::string &relative, std::vector<std::string> *files) {
  const std::string search_dir = relative.empty() ? root : JoinPath(root, relative);
  const std::string search_spec = JoinPath(search_dir, "*");
  WIN32_FIND_DATA find_data = {0};
  HANDLE find = FindFirstFile(search_spec.c_str(), &find_data);
  if (find == INVALID_HANDLE_VALUE) {
    return;
  }

  do {
    if (strcmp(find_data.cFileName, ".") == 0 || strcmp(find_data.cFileName, "..") == 0) {
      continue;
    }

    std::string child_relative = relative.empty()
      ? find_data.cFileName
      : JoinPath(relative, find_data.cFileName);
    if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      CollectFiles(root, child_relative, files);
    } else {
      files->push_back(child_relative);
    }
  } while (FindNextFile(find, &find_data));

  FindClose(find);
}

static bool CopyReleaseOptimizedToRelease(const std::string &repo_root) {
  const std::string source_root = JoinPath(repo_root, "Release - Optimized");
  const std::string destination_root = JoinPath(repo_root, "Release");
  if (!DirectoryExists(source_root)) {
    PostStatusText("Release - Optimized does not exist.");
    return false;
  }
  if (!EnsureDirectoryTree(destination_root)) {
    PostStatusText("Could not create Release directory.");
    return false;
  }

  std::vector<std::string> files;
  CollectFiles(source_root, "", &files);
  if (files.empty()) {
    PostStatusText("No files found in Release - Optimized.");
    PostProgress(0, 100);
    return true;
  }

  PostProgress(0, (int)files.size());
  for (size_t i = 0; i < files.size(); ++i) {
    // Never copy our own running executable (and its build artifacts) over the
    // copy in Release: DeveloperToolbar.exe is locked while this app is running,
    // so the copy would fail with "could not copy ... already running". The
    // toolbar is also excluded from the build (see Hiss.sln), so this is the
    // single deliverable the refresh intentionally skips.
    const std::string leaf = FileNameOnly(files[i]);
    if (_strnicmp(leaf.c_str(), "DeveloperToolbar.", 17) == 0) {
      PostProgress((int)i + 1, (int)files.size());
      continue;
    }
    const std::string source = JoinPath(source_root, files[i]);
    const std::string destination = JoinPath(destination_root, files[i]);

    // Don't let a stale optimized copy clobber a freshly built file. Some
    // projects (e.g. Vision, trainer) build the "Release - Optimized" solution
    // configuration straight into Release\, leaving the copy in
    // Release - Optimized\ stale. Skip when the destination is newer.
    FILETIME source_time, destination_time;
    if (GetFileWriteTime(source, &source_time) && GetFileWriteTime(destination, &destination_time)
        && FileTimeIsNewer(destination_time, source_time)) {
      PostProgress((int)i + 1, (int)files.size());
      continue;
    }

    const std::string destination_dir = ParentDirectory(destination);
    if (!EnsureDirectoryTree(destination_dir) || !CopyFile(source.c_str(), destination.c_str(), FALSE)) {
      char status[256] = {0};
      sprintf_s(status, "Could not copy %s.", files[i].c_str());
      PostStatusText(status);
      return false;
    }
    PostProgress((int)i + 1, (int)files.size());
  }

  return true;
}

static DWORD WINAPI BuildThreadProc(LPVOID) {
  const std::string repo_root = FindRepoRoot();

  PostStatusText("Building Release - Optimized...");
  PostProgress(-1, 100);
  bool build_ok = BuildReleaseOptimized(repo_root);
  if (!build_ok && AskToKillBlockingProcesses("Build", repo_root)) {
    PostStatusText("Building Release - Optimized again...");
    PostProgress(-1, 100);
    build_ok = BuildReleaseOptimized(repo_root);
  }
  if (!build_ok) {
    PostStatusText("Build failed.");
    PostMessage(g_main_window, WM_APP_BUILD_DONE, 0, 0);
    return 0;
  }

  PostStatusText("Copying Release - Optimized to Release...");
  bool copy_ok = CopyReleaseOptimizedToRelease(repo_root);
  if (!copy_ok && AskToKillBlockingProcesses("Copy", repo_root)) {
    PostStatusText("Copying Release - Optimized to Release again...");
    copy_ok = CopyReleaseOptimizedToRelease(repo_root);
  }

  PostStatusText(copy_ok ? "Build complete. Release has been refreshed." : "Copy failed.");
  PostProgress(copy_ok ? 100 : 0, 100);
  PostMessage(g_main_window, WM_APP_BUILD_DONE, 0, 0);
  return 0;
}

static void StartBuild() {
  if (InterlockedCompareExchange(&g_building, 1, 0) != 0) {
    return;
  }

  EnableWindow(g_build_button, FALSE);
  EnableWindow(g_pick_button, FALSE);
  HANDLE thread = CreateThread(NULL, 0, BuildThreadProc, NULL, 0, NULL);
  if (thread == NULL) {
    InterlockedExchange(&g_building, 0);
    EnableWindow(g_build_button, TRUE);
    EnableWindow(g_pick_button, TRUE);
    SetStatusText("Could not start build thread.");
    return;
  }
  CloseHandle(thread);
}

static std::string BestExecutablePath(const std::string &repo_root, const char *exe_name) {
  const std::string release_path = JoinPath(JoinPath(repo_root, "Release"), exe_name);
  if (FileExists(release_path)) {
    return release_path;
  }

  const std::string optimized_path = JoinPath(JoinPath(repo_root, "Release - Optimized"), exe_name);
  if (FileExists(optimized_path)) {
    return optimized_path;
  }

  const std::string debug_path = JoinPath(JoinPath(repo_root, "Debug"), exe_name);
  if (FileExists(debug_path)) {
    return debug_path;
  }

  return release_path;
}

static void OpenRepoExecutable(const char *exe_name, const char *display_name) {
  const std::string repo_root = FindRepoRoot();
  const std::string exe_path = BestExecutablePath(repo_root, exe_name);
  if (!FileExists(exe_path)) {
    char message[256] = {0};
    sprintf_s(message, "%s was not found. Build the project first.", display_name);
    MessageBox(g_main_window, message, kAppTitle, MB_OK | MB_ICONWARNING | MB_TOPMOST);
    return;
  }

  HINSTANCE result = ShellExecute(g_main_window, "open", exe_path.c_str(), NULL,
    ParentDirectory(exe_path).c_str(), SW_SHOWNORMAL);
  if ((INT_PTR)result <= 32) {
    char message[256] = {0};
    sprintf_s(message, "Could not open %s.", display_name);
    MessageBox(g_main_window, message, kAppTitle, MB_OK | MB_ICONERROR | MB_TOPMOST);
  }
}

static void OpenExternalExecutable(const char *exe_path, const char *display_name) {
  if (!FileExists(exe_path)) {
    char message[512] = {0};
    sprintf_s(message, "%s was not found:\r\n%s", display_name, exe_path);
    MessageBox(g_main_window, message, kAppTitle, MB_OK | MB_ICONWARNING | MB_TOPMOST);
    return;
  }

  HINSTANCE result = ShellExecute(g_main_window, "open", exe_path, NULL,
    ParentDirectory(exe_path).c_str(), SW_SHOWNORMAL);
  if ((INT_PTR)result <= 32) {
    char message[256] = {0};
    sprintf_s(message, "Could not open %s.", display_name);
    MessageBox(g_main_window, message, kAppTitle, MB_OK | MB_ICONERROR | MB_TOPMOST);
  }
}

// Find the most-recently-modified *.md in a directory (non-recursive).
static std::string FindLatestMarkdown(const std::string &directory) {
  std::string latest_path;
  FILETIME latest_time = {0};
  const std::string spec = JoinPath(directory, "*.md");
  WIN32_FIND_DATA fd = {0};
  HANDLE find = FindFirstFile(spec.c_str(), &fd);
  if (find == INVALID_HANDLE_VALUE) return latest_path;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    if (latest_path.empty() || FileTimeIsNewer(fd.ftLastWriteTime, latest_time)) {
      latest_path = JoinPath(directory, fd.cFileName);
      latest_time = fd.ftLastWriteTime;
    }
  } while (FindNextFile(find, &fd));
  FindClose(find);
  return latest_path;
}

// Open the newest plan markdown in MarkdownViewer (bare launch if none / not found).
static void OpenMarkdownViewer() {
  if (!FileExists(kMdViewerPath)) {
    char message[512] = {0};
    sprintf_s(message, "MarkdownViewer was not found:\r\n%s", kMdViewerPath);
    MessageBox(g_main_window, message, kAppTitle, MB_OK | MB_ICONWARNING | MB_TOPMOST);
    return;
  }
  const std::string latest = FindLatestMarkdown(kPlansDir);
  HINSTANCE result = ShellExecute(g_main_window, "open", kMdViewerPath,
    latest.empty() ? NULL : Quote(latest).c_str(),
    ParentDirectory(kMdViewerPath).c_str(), SW_SHOWNORMAL);
  if ((INT_PTR)result <= 32) {
    MessageBox(g_main_window, "Could not open MarkdownViewer.", kAppTitle,
      MB_OK | MB_ICONERROR | MB_TOPMOST);
  } else if (!latest.empty()) {
    char status[512] = {0};
    sprintf_s(status, "Opened %s in MarkdownViewer.", FileNameOnly(latest).c_str());
    SetStatusText(status);
  }
}

// ---- scrcpy window auto-positioning (placement persisted to the Hiss postgres DB) ---
// The scrcpy mirror window is created by scrcpy.exe (its title is the device name); it
// is NOT the console/terminal window scrcpy may also spawn. We locate it as a visible,
// non-owned, captioned top-level window owned by a scrcpy.exe process. Its last
// position/size lives in the `settings` table (key 'devtoolbar_windows', field
// 'scrcpy', value "x,y,w,h") and is restored when scrcpy is opened from this toolbar.

static const char *kScrcpyWindowsKey = "devtoolbar_windows";
static const char *kScrcpyWindowField = "scrcpy";

// Optional connection override at HKCU\Software\Hiss\Trainer "hiss_conn" (shared with
// the trainer, so a single registry value points every tool at the same database).
static std::string DbReadConnOverride() {
  std::string out;
  HKEY key;
  if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Hiss\\Trainer", 0, KEY_READ, &key) == ERROR_SUCCESS) {
    char buf[1024] = {0};
    DWORD size = sizeof(buf), type = 0;
    if (RegQueryValueExA(key, "hiss_conn", NULL, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS && type == REG_SZ) {
      out = buf;
    }
    RegCloseKey(key);
  }
  return out;
}

static PGconn *DbConnect() {
  std::string conn = DbReadConnOverride();
  if (conn.empty()) {
    conn = "host=127.0.0.1 port=5432 user=postgres password='dbpass' dbname='hiss'";
  }
  PGconn *c = PQconnectdb(conn.c_str());
  if (c == NULL || PQstatus(c) != CONNECTION_OK) {
    if (c) PQfinish(c);
    return NULL;
  }
  return c;
}

static std::string SqlEscapeLiteral(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\'') out += "''"; else out += s[i];
  }
  return out;
}

// Upsert the rect as "x,y,w,h" into settings(key='devtoolbar_windows').value->'scrcpy'
// (same JSON-merge shape as TrainerDB_SetSetting).
static bool DbSaveScrcpyRect(const RECT &r) {
  PGconn *conn = DbConnect();
  if (conn == NULL) return false;
  char val[64] = {0};
  sprintf_s(val, "%d,%d,%d,%d", (int)r.left, (int)r.top, (int)(r.right - r.left), (int)(r.bottom - r.top));
  const std::string v = SqlEscapeLiteral(val);
  const std::string field = SqlEscapeLiteral(kScrcpyWindowField);
  const std::string key = SqlEscapeLiteral(kScrcpyWindowsKey);
  const std::string sql =
    "INSERT INTO settings(key,value,updated_at) VALUES ('" + key + "', jsonb_build_object('" + field +
    "', to_jsonb('" + v + "'::text)), now()) ON CONFLICT (key) DO UPDATE SET value = jsonb_set("
    "COALESCE(settings.value,'{}'::jsonb), '{" + field + "}', to_jsonb('" + v + "'::text)), updated_at = now()";
  PGresult *res = PQexec(conn, sql.c_str());
  bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
  if (res) PQclear(res);
  PQfinish(conn);
  return ok;
}

static bool DbLoadScrcpyRect(RECT *out) {
  if (out == NULL) return false;
  PGconn *conn = DbConnect();
  if (conn == NULL) return false;
  const std::string field = SqlEscapeLiteral(kScrcpyWindowField);
  const std::string key = SqlEscapeLiteral(kScrcpyWindowsKey);
  const std::string sql = "SELECT value->>'" + field + "' FROM settings WHERE key='" + key + "'";
  PGresult *res = PQexec(conn, sql.c_str());
  bool ok = false;
  if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0 && !PQgetisnull(res, 0, 0)) {
    int x = 0, y = 0, w = 0, h = 0;
    if (sscanf_s(PQgetvalue(res, 0, 0), "%d,%d,%d,%d", &x, &y, &w, &h) == 4 && w >= 50 && h >= 50) {
      out->left = x; out->top = y; out->right = x + w; out->bottom = y + h;
      ok = true;
    }
  }
  if (res) PQclear(res);
  PQfinish(conn);
  return ok;
}

static void CollectScrcpyPids(std::vector<DWORD> *pids) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return;
  PROCESSENTRY32 entry = {0};
  entry.dwSize = sizeof(entry);
  if (Process32First(snapshot, &entry)) {
    do {
      if (_stricmp(entry.szExeFile, "scrcpy.exe") == 0) pids->push_back(entry.th32ProcessID);
    } while (Process32Next(snapshot, &entry));
  }
  CloseHandle(snapshot);
}

// True if `window` is a scrcpy mirror window (owned by a scrcpy pid, visible, top-level,
// captioned, and not the console). `pids` is the current set of scrcpy.exe process ids.
static bool IsScrcpyMirrorWindow(HWND window, const std::vector<DWORD> &pids) {
  DWORD wpid = 0;
  GetWindowThreadProcessId(window, &wpid);
  bool pid_match = false;
  for (size_t i = 0; i < pids.size(); ++i) {
    if (pids[i] == wpid) { pid_match = true; break; }
  }
  if (!pid_match) return false;
  if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != NULL) return false;
  char cls[64] = {0};
  GetClassNameA(window, cls, sizeof(cls));
  if (_stricmp(cls, "ConsoleWindowClass") == 0) return false;   // skip scrcpy's terminal
  if ((GetWindowLong(window, GWL_STYLE) & WS_CAPTION) != WS_CAPTION) return false;
  return true;
}

struct ScrcpyWindowSearch {
  const std::vector<DWORD> *pids;
  const std::vector<HWND> *exclude;   // windows that existed before launch (skip them)
  HWND window;
};

static BOOL CALLBACK FindScrcpyWindowProc(HWND window, LPARAM param) {
  ScrcpyWindowSearch *s = (ScrcpyWindowSearch*)param;
  if (!IsScrcpyMirrorWindow(window, *s->pids)) return TRUE;
  if (s->exclude) {
    for (size_t i = 0; i < s->exclude->size(); ++i) {
      if ((*s->exclude)[i] == window) return TRUE;
    }
  }
  s->window = window;
  return FALSE;
}

// Find a scrcpy mirror window. When `exclude` is given, only a window NOT in that list
// is returned (so the launcher repositions the newly-opened one, not a pre-existing one).
static HWND FindScrcpyWindow(const std::vector<HWND> *exclude) {
  std::vector<DWORD> pids;
  CollectScrcpyPids(&pids);
  if (pids.empty()) return NULL;
  ScrcpyWindowSearch s = { &pids, exclude, NULL };
  EnumWindows(FindScrcpyWindowProc, (LPARAM)&s);
  return s.window;
}

static BOOL CALLBACK CollectScrcpyWindowsProc(HWND window, LPARAM param) {
  ScrcpyWindowSearch *s = (ScrcpyWindowSearch*)param;
  if (IsScrcpyMirrorWindow(window, *s->pids)) {
    const_cast<std::vector<HWND>*>(s->exclude)->push_back(window);
  }
  return TRUE;
}

static void CollectScrcpyWindows(std::vector<HWND> *out) {
  std::vector<DWORD> pids;
  CollectScrcpyPids(&pids);
  if (pids.empty()) return;
  ScrcpyWindowSearch s = { &pids, out, NULL };   // reuse `exclude` slot as the output list
  EnumWindows(CollectScrcpyWindowsProc, (LPARAM)&s);
}

struct ScrcpyPositionJob {
  RECT rect;                 // saved placement to apply
  std::vector<HWND> before;  // scrcpy windows already open at launch time
};

static DWORD WINAPI ScrcpyPositionThread(LPVOID param) {
  ScrcpyPositionJob *job = (ScrcpyPositionJob*)param;
  // Poll up to ~25s for the NEW mirror window to appear (USB/adb startup can be slow).
  HWND win = NULL;
  for (int i = 0; i < 100 && win == NULL; ++i) {
    win = FindScrcpyWindow(&job->before);
    if (win == NULL) Sleep(250);
  }
  if (win != NULL) {
    RECT r = job->rect;
    // Only move if the saved placement still lands on a monitor (handles a screen that
    // was unplugged since the position was recorded).
    if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != NULL) {
      SetWindowPos(win, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
        SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }
  delete job;
  return 0;
}

// Launch scrcpy, then (if a placement was recorded) move the new mirror window to it on
// a background thread once the window actually appears.
static void OpenScrcpyAndPosition() {
  std::vector<HWND> before;
  CollectScrcpyWindows(&before);   // so we reposition only the window this launch opens
  RECT saved;
  const bool have_saved = DbLoadScrcpyRect(&saved);

  OpenExternalExecutable(kScrcpyPath, "Scrcpy");

  if (!have_saved) {
    SetStatusText("Opened scrcpy. No saved position yet - use \"Rec Scrcpy Pos\" to record one.");
    return;
  }
  ScrcpyPositionJob *job = new ScrcpyPositionJob();
  job->rect = saved;
  job->before = before;
  HANDLE th = CreateThread(NULL, 0, ScrcpyPositionThread, job, 0, NULL);
  if (th != NULL) CloseHandle(th); else delete job;
}

// Record the current scrcpy mirror window's position + size into the Hiss database.
static void RecordScrcpyPosition() {
  HWND win = FindScrcpyWindow(NULL);
  if (win == NULL) {
    SetStatusText("scrcpy mirror window not found. Open scrcpy and connect a device first.");
    return;
  }
  RECT r;
  if (!GetWindowRect(win, &r)) {
    SetStatusText("Could not read the scrcpy window position.");
    return;
  }
  if (DbSaveScrcpyRect(r)) {
    char msg[160] = {0};
    sprintf_s(msg, "Recorded scrcpy position %d,%d  %dx%d to the Hiss database.",
      (int)r.left, (int)r.top, (int)(r.right - r.left), (int)(r.bottom - r.top));
    SetStatusText(msg);
  } else {
    SetStatusText("Failed to write scrcpy position to the Hiss database.");
  }
}

static void CreateChildControls(HWND hwnd) {
  CreateWindow("STATIC", "Width",
    WS_CHILD | WS_VISIBLE,
    16, 18, 60, 20, hwnd, NULL, g_instance, NULL);
  g_width_edit = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "1024",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
    82, 14, 90, 24, hwnd, (HMENU)IDC_WIDTH_EDIT, g_instance, NULL);

  CreateWindow("STATIC", "Height",
    WS_CHILD | WS_VISIBLE,
    190, 18, 60, 20, hwnd, NULL, g_instance, NULL);
  g_height_edit = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "768",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
    256, 14, 90, 24, hwnd, (HMENU)IDC_HEIGHT_EDIT, g_instance, NULL);

  g_scale_checkbox = CreateWindow("BUTTON", "Scale to monitor DPI",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
    16, 50, 180, 24, hwnd, (HMENU)IDC_SCALE_CHECKBOX, g_instance, NULL);
  SendMessage(g_scale_checkbox, BM_SETCHECK, BST_UNCHECKED, 0);

  g_pick_button = CreateWindow("BUTTON", "Pick Window",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
    16, 82, 104, 30, hwnd, (HMENU)IDC_PICK_BUTTON, g_instance, NULL);

  g_build_button = CreateWindow("BUTTON", "Build",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
    126, 82, 104, 30, hwnd, (HMENU)IDC_BUILD_BUTTON, g_instance, NULL);

  g_open_openscrape_button = CreateWindow("BUTTON", "Vision",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
    236, 82, 110, 30, hwnd, (HMENU)IDC_OPEN_OPENSCRAPE_BUTTON, g_instance, NULL);

  g_open_openholdem_button = CreateWindow("BUTTON", "Hiss",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
    16, 118, 104, 28, hwnd, (HMENU)IDC_OPEN_OPENHOLDEM_BUTTON, g_instance, NULL);

  g_close_all_button = CreateWindow("BUTTON", "Close All",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
    126, 118, 104, 28, hwnd, (HMENU)IDC_CLOSE_ALL_BUTTON, g_instance, NULL);

  g_open_scrcpy_button = CreateWindow("BUTTON", "Scrcpy",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
    236, 118, 110, 28, hwnd, (HMENU)IDC_OPEN_SCRCPY_BUTTON, g_instance, NULL);

  g_open_trainer_button = CreateWindow("BUTTON", "Trainer",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
    16, 154, 104, 28, hwnd, (HMENU)IDC_OPEN_TRAINER_BUTTON, g_instance, NULL);

  g_rec_scrcpy_button = CreateWindow("BUTTON", "Rec Scrcpy Pos",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
    126, 154, 220, 28, hwnd, (HMENU)IDC_REC_SCRCPY_BUTTON, g_instance, NULL);

  g_open_learner_button = CreateWindow("BUTTON", "Learner",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
    16, 190, 104, 28, hwnd, (HMENU)IDC_OPEN_LEARNER_BUTTON, g_instance, NULL);

  g_open_mdviewer_button = CreateWindow("BUTTON", "MD Viewer",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
    126, 190, 220, 28, hwnd, (HMENU)IDC_OPEN_MDVIEWER_BUTTON, g_instance, NULL);

  // App icons on the launch buttons.
  SetButtonIcon(g_open_openholdem_button, kSnakeIcoPath);   // Hiss
  SetButtonIcon(g_open_openscrape_button, kEyeIcoPath);     // Vision
  SetButtonIcon(g_open_trainer_button,    kBarbellIcoPath); // trainer
  SetButtonIcon(g_open_learner_button,    kFeatherIcoPath); // learner (feather)
  SetButtonIconFromExe(g_open_mdviewer_button, kMdViewerPath); // MD Viewer's own icon

  g_build_progress = CreateWindowEx(0, PROGRESS_CLASS, "",
    WS_CHILD | WS_VISIBLE,
    16, 230, 330, 18, hwnd, (HMENU)IDC_BUILD_PROGRESS, g_instance, NULL);
  SendMessage(g_build_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
  SendMessage(g_build_progress, PBM_SETPOS, 0, 0);

  g_alert_text = CreateWindow("STATIC", "",
    WS_CHILD | SS_CENTER,
    16, 256, 330, 36, hwnd, (HMENU)IDC_ALERT_TEXT, g_instance, NULL);
  ShowWindow(g_alert_text, SW_HIDE);

  g_status_text = CreateWindow("STATIC", "Enter size, then click Pick Window.",
    WS_CHILD | WS_VISIBLE,
    16, 300, 340, 54, hwnd, (HMENU)IDC_STATUS_TEXT, g_instance, NULL);
  LoadDefaultTablemapSize();
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
  case WM_CREATE:
    g_main_window = hwnd;
    CreateChildControls(hwnd);
    return 0;

  case WM_COMMAND:
    if (LOWORD(wparam) == IDC_PICK_BUTTON) {
      StartPickingWindow();
      return 0;
    }
    if (LOWORD(wparam) == IDC_BUILD_BUTTON) {
      StartBuild();
      return 0;
    }
    if (LOWORD(wparam) == IDC_OPEN_OPENSCRAPE_BUTTON) {
      OpenRepoExecutable("Vision.exe", "Vision");
      return 0;
    }
    if (LOWORD(wparam) == IDC_OPEN_OPENHOLDEM_BUTTON) {
      OpenRepoExecutable("Hiss.exe", "Hiss");
      return 0;
    }
    if (LOWORD(wparam) == IDC_OPEN_SCRCPY_BUTTON) {
      OpenScrcpyAndPosition();
      return 0;
    }
    if (LOWORD(wparam) == IDC_REC_SCRCPY_BUTTON) {
      RecordScrcpyPosition();
      return 0;
    }
    if (LOWORD(wparam) == IDC_OPEN_TRAINER_BUTTON) {
      OpenRepoExecutable("trainer.exe", "Trainer");
      return 0;
    }
    if (LOWORD(wparam) == IDC_OPEN_LEARNER_BUTTON) {
      OpenRepoExecutable("learner.exe", "Learner");
      return 0;
    }
    if (LOWORD(wparam) == IDC_OPEN_MDVIEWER_BUTTON) {
      OpenMarkdownViewer();
      return 0;
    }
    if (LOWORD(wparam) == IDC_CLOSE_ALL_BUTTON) {
      CloseAllOpenHoldemAndOpenScrape();
      return 0;
    }
    break;

  case WM_APP_STATUS:
    SetStatusText((const char *)lparam);
    free((void *)lparam);
    return 0;

  case WM_APP_PROGRESS:
    if ((int)wparam < 0) {
      SendMessage(g_build_progress, PBM_SETMARQUEE, TRUE, 30);
    } else {
      SendMessage(g_build_progress, PBM_SETMARQUEE, FALSE, 0);
      SendMessage(g_build_progress, PBM_SETRANGE, 0, MAKELPARAM(0, (int)lparam));
      SendMessage(g_build_progress, PBM_SETPOS, (int)wparam, 0);
    }
    return 0;

  case WM_APP_BUILD_DONE:
    InterlockedExchange(&g_building, 0);
    EnableWindow(g_build_button, TRUE);
    EnableWindow(g_pick_button, TRUE);
    return 0;

  case WM_TIMER:
    if (wparam == TIMER_WINDOW_MONITOR) {
      PollMonitoredWindowSize();
      return 0;
    }
    break;

  case WM_CTLCOLORSTATIC:
    if ((HWND)lparam == g_alert_text) {
      SetTextColor((HDC)wparam, RGB(255, 255, 255));
      SetBkColor((HDC)wparam, RGB(255, 0, 0));
      return (LRESULT)g_alert_brush;
    }
    break;

  case WM_LBUTTONDOWN:
    if (g_picking_window) {
      ResizeClickedWindow(lparam);
      return 0;
    }
    break;

  case WM_KEYDOWN:
    if ((wparam == VK_ESCAPE) && g_picking_window) {
      StopPickingWindow();
      SetStatusText("Selection canceled.");
      return 0;
    }
    break;

  case WM_SETCURSOR:
    if (g_picking_window) {
      SetCursor(LoadCursor(NULL, IDC_CROSS));
      return TRUE;
    }
    break;

  case WM_DESTROY:
    StopWindowMonitor();
    if (g_alert_brush != NULL) {
      DeleteObject(g_alert_brush);
      g_alert_brush = NULL;
    }
    StopPickingWindow();
    PostQuitMessage(0);
    return 0;
  }

  return DefWindowProc(hwnd, message, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_command) {
  g_instance = instance;
  EnableProcessDpiAwareness();
  g_alert_brush = CreateSolidBrush(RGB(255, 0, 0));

  INITCOMMONCONTROLSEX common_controls = {0};
  common_controls.dwSize = sizeof(common_controls);
  common_controls.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
  InitCommonControlsEx(&common_controls);

  // Rubber-duck app icon (debugging mascot), embedded as resource id 1 so Windows
  // Explorer / taskbar show it on the exe too (falls back to the file if missing).
  HICON duck_big = (HICON)LoadImage(instance, MAKEINTRESOURCE(1), IMAGE_ICON, 32, 32, 0);
  HICON duck_small = (HICON)LoadImage(instance, MAKEINTRESOURCE(1), IMAGE_ICON, 16, 16, 0);
  if (duck_big == NULL)   duck_big = (HICON)LoadImageA(NULL, kDuckIcoPath, IMAGE_ICON, 32, 32, LR_LOADFROMFILE);
  if (duck_small == NULL) duck_small = (HICON)LoadImageA(NULL, kDuckIcoPath, IMAGE_ICON, 16, 16, LR_LOADFROMFILE);

  WNDCLASSEX window_class = {0};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hIcon = duck_big ? duck_big : LoadIcon(NULL, IDI_APPLICATION);
  window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
  window_class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  window_class.lpszClassName = kWindowClassName;
  window_class.hIconSm = duck_small ? duck_small : LoadIcon(NULL, IDI_APPLICATION);

  if (!RegisterClassEx(&window_class)) {
    return 1;
  }

  HWND hwnd = CreateWindowEx(WS_EX_TOPMOST, kWindowClassName, kAppTitle,
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
    CW_USEDEFAULT, CW_USEDEFAULT, 380, 421,
    NULL, NULL, instance, NULL);
  if (hwnd == NULL) {
    return 1;
  }

  SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  ShowWindow(hwnd, show_command);
  UpdateWindow(hwnd);

  MSG message = {0};
  while (GetMessage(&message, NULL, 0, 0)) {
    TranslateMessage(&message);
    DispatchMessage(&message);
  }

  return (int)message.wParam;
}
