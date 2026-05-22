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

#pragma comment(lib, "comctl32.lib")

#define IDC_WIDTH_EDIT 1001
#define IDC_HEIGHT_EDIT 1002
#define IDC_PICK_BUTTON 1003
#define IDC_STATUS_TEXT 1004
#define IDC_SCALE_CHECKBOX 1005
#define IDC_BUILD_BUTTON 1006
#define IDC_BUILD_PROGRESS 1007
#define IDC_OPEN_OPENSCRAPE_BUTTON 1008
#define IDC_OPEN_OPENHOLDEM_BUTTON 1009

#define WM_APP_STATUS (WM_APP + 1)
#define WM_APP_PROGRESS (WM_APP + 2)
#define WM_APP_BUILD_DONE (WM_APP + 3)

static const char kWindowClassName[] = "OpenHoldemDeveloperToolbar";
static const char kAppTitle[] = "Developer Toolbar";
static HWND g_main_window = NULL;
static HWND g_width_edit = NULL;
static HWND g_height_edit = NULL;
static HWND g_pick_button = NULL;
static HWND g_status_text = NULL;
static HWND g_scale_checkbox = NULL;
static HWND g_build_button = NULL;
static HWND g_open_openscrape_button = NULL;
static HWND g_open_openholdem_button = NULL;
static HWND g_build_progress = NULL;
static HINSTANCE g_instance = NULL;
static bool g_picking_window = false;
static volatile LONG g_building = 0;

typedef BOOL (WINAPI *SetProcessDpiAwarenessContextFunction)(DPI_AWARENESS_CONTEXT);
typedef UINT (WINAPI *GetDpiForWindowFunction)(HWND);
typedef UINT (WINAPI *GetDpiForSystemFunction)(void);
typedef BOOL (WINAPI *QueryFullProcessImageNameFunction)(HANDLE, DWORD, LPSTR, PDWORD);

static void SetStatusText(const char *message) {
  SetWindowText(g_status_text, message);
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

static std::string FindRepoRoot() {
  std::string candidate = ExeDirectory();
  for (int i = 0; i < 5; ++i) {
    if (FileExists(JoinPath(candidate, "OpenHoldem.sln"))) {
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
    "##_OpenHoldem_Release_Directory_##\\scraper",
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
  const std::string solution = JoinPath(repo_root, "OpenHoldem.sln");
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
    "OpenHoldem.exe", "OpenScrape.exe", "OHReplay.exe", "ManualMode.exe",
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

static bool AskToKillBlockingProcesses(const char *step, const std::string &repo_root) {
  char message[512] = {0};
  sprintf_s(message,
    "%s failed. A running OpenHoldem/OpenScrape/DeveloperToolbar process may be blocking the output files.\r\n\r\nKill blocking repo output processes and retry?",
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
    const std::string source = JoinPath(source_root, files[i]);
    const std::string destination = JoinPath(destination_root, files[i]);
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

  g_open_openscrape_button = CreateWindow("BUTTON", "OpenScrape",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
    236, 82, 110, 30, hwnd, (HMENU)IDC_OPEN_OPENSCRAPE_BUTTON, g_instance, NULL);

  g_open_openholdem_button = CreateWindow("BUTTON", "OpenHoldem",
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
    16, 118, 330, 28, hwnd, (HMENU)IDC_OPEN_OPENHOLDEM_BUTTON, g_instance, NULL);

  g_build_progress = CreateWindowEx(0, PROGRESS_CLASS, "",
    WS_CHILD | WS_VISIBLE,
    16, 158, 330, 18, hwnd, (HMENU)IDC_BUILD_PROGRESS, g_instance, NULL);
  SendMessage(g_build_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
  SendMessage(g_build_progress, PBM_SETPOS, 0, 0);

  g_status_text = CreateWindow("STATIC", "Enter size, then click Pick Window.",
    WS_CHILD | WS_VISIBLE,
    16, 186, 340, 54, hwnd, (HMENU)IDC_STATUS_TEXT, g_instance, NULL);
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
      OpenRepoExecutable("OpenScrape.exe", "OpenScrape");
      return 0;
    }
    if (LOWORD(wparam) == IDC_OPEN_OPENHOLDEM_BUTTON) {
      OpenRepoExecutable("OpenHoldem.exe", "OpenHoldem");
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
    StopPickingWindow();
    PostQuitMessage(0);
    return 0;
  }

  return DefWindowProc(hwnd, message, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_command) {
  g_instance = instance;
  EnableProcessDpiAwareness();

  INITCOMMONCONTROLSEX common_controls = {0};
  common_controls.dwSize = sizeof(common_controls);
  common_controls.dwICC = ICC_PROGRESS_CLASS;
  InitCommonControlsEx(&common_controls);

  WNDCLASSEX window_class = {0};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hIcon = LoadIcon(NULL, IDI_APPLICATION);
  window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
  window_class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  window_class.lpszClassName = kWindowClassName;
  window_class.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

  if (!RegisterClassEx(&window_class)) {
    return 1;
  }

  HWND hwnd = CreateWindowEx(WS_EX_TOPMOST, kWindowClassName, kAppTitle,
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
    CW_USEDEFAULT, CW_USEDEFAULT, 380, 300,
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
