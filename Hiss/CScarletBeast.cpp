#include "stdafx.h"  // OpenHoldem/Hiss precompiled header (must be first)

#include "CScarletBeast.h"

#include <windows.h>
#include <winhttp.h>
#include <vector>

#pragma comment(lib, "winhttp.lib")

CScarletBeast* p_scarlet_beast = nullptr;

static const wchar_t* kRegPath = L"Software\\ScarletBeast";

CScarletBeast::CScarletBeast()
    : _scrape_from_server(false),
      _base_url(L"poker.scarletbeast.com"),
      _table_id(1),
      _is_child(false),
      _cmdline_table(0),
      _last_manage_tick(0),
      _last_ok(false),
      _last_status(0),
      _seat_json_tick(0),
      _hud_tick(0) {
  LoadFromRegistry();
  ParseCommandLine();
}

// Detect child mode: "--sb-table=N" on the command line binds this instance to
// one table and suppresses further spawning.
void CScarletBeast::ParseCommandLine() {
  std::string cmd = ::GetCommandLineA() ? ::GetCommandLineA() : "";
  const std::string flag = "--sb-table=";
  size_t p = cmd.find(flag);
  if (p != std::string::npos) {
    _cmdline_table = atoi(cmd.c_str() + p + flag.size());
    if (_cmdline_table > 0) _is_child = true;
  }
}

// Spawn another Hiss bound to a specific table.
void CScarletBeast::SpawnChild(int table_id) {
  char exe[MAX_PATH] = {0};
  if (!::GetModuleFileNameA(NULL, exe, MAX_PATH)) return;
  std::string cmd = std::string("\"") + exe + "\" --sb-table=" + std::to_string(table_id);
  std::vector<char> buf(cmd.begin(), cmd.end());
  buf.push_back(0);
  STARTUPINFOA si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));
  if (::CreateProcessA(NULL, buf.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
    _children[table_id] = std::make_pair((void*)pi.hProcess, (void*)pi.hThread);
  }
}

// Poll the seated-tables endpoint; spawn/kill children to match. Throttled to
// once every ~5s. Master only.
void CScarletBeast::ManageInstances() {
  if (_is_child || !_scrape_from_server) return;
  unsigned long now = ::GetTickCount();
  if (_last_manage_tick != 0 && (now - _last_manage_tick) < 5000) return;
  _last_manage_tick = now;

  std::string body = Request(L"GET", L"/api/v1/me/tables", "", true);
  if (!_last_ok) return;

  // Parse {"tables":[1,2,3]} into a set of ids.
  std::map<int, bool> seated;
  size_t p = body.find("\"tables\"");
  if (p != std::string::npos) {
    p = body.find('[', p);
    size_t end = (p == std::string::npos) ? std::string::npos : body.find(']', p);
    while (p != std::string::npos && end != std::string::npos && p < end) {
      while (p < end && (body[p] < '0' || body[p] > '9')) ++p;
      if (p >= end) break;
      int id = 0;
      while (p < end && body[p] >= '0' && body[p] <= '9') { id = id * 10 + (body[p] - '0'); ++p; }
      if (id > 0) seated[id] = true;
    }
  }

  int mine = TableId();
  // Spawn children for seated tables other than the master's own table.
  for (std::map<int, bool>::iterator it = seated.begin(); it != seated.end(); ++it) {
    int id = it->first;
    if (id == mine) continue;
    if (_children.find(id) == _children.end()) SpawnChild(id);
  }
  // Close children whose table is no longer seated.
  for (std::map<int, std::pair<void*, void*> >::iterator it = _children.begin(); it != _children.end();) {
    if (seated.find(it->first) == seated.end()) {
      if (it->second.first) { ::TerminateProcess((HANDLE)it->second.first, 0); ::CloseHandle((HANDLE)it->second.first); }
      if (it->second.second) ::CloseHandle((HANDLE)it->second.second);
      _children.erase(it++);
    } else {
      ++it;
    }
  }
}

CScarletBeast::~CScarletBeast() {}

// ----------------------------------------------------------- registry I/O ----

static std::wstring RegReadString(const wchar_t* name, const std::wstring& def) {
  HKEY h;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &h) != ERROR_SUCCESS)
    return def;
  wchar_t buf[2048];
  DWORD len = sizeof(buf);
  DWORD type = 0;
  std::wstring out = def;
  if (RegQueryValueExW(h, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &len) == ERROR_SUCCESS &&
      type == REG_SZ) {
    out.assign(buf, (len / sizeof(wchar_t)) ? (len / sizeof(wchar_t)) - 1 : 0);
  }
  RegCloseKey(h);
  return out;
}

static void RegWriteString(const wchar_t* name, const std::wstring& val) {
  HKEY h;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr) !=
      ERROR_SUCCESS)
    return;
  RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(val.c_str()),
                 static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(h);
}

void CScarletBeast::LoadFromRegistry() {
  _scrape_from_server = RegReadString(L"ScrapeFromServer", L"0") == L"1";
  _base_url = RegReadString(L"BaseUrl", L"poker.scarletbeast.com");
  _api_key = RegReadString(L"ApiKey", L"");
  _google_token = RegReadString(L"GoogleToken", L"");
  _table_id = _wtoi(RegReadString(L"TableId", L"1").c_str());
  if (_table_id <= 0) _table_id = 1;
}

void CScarletBeast::SaveToRegistry() {
  RegWriteString(L"ScrapeFromServer", _scrape_from_server ? L"1" : L"0");
  RegWriteString(L"BaseUrl", _base_url);
  RegWriteString(L"ApiKey", _api_key);
  RegWriteString(L"GoogleToken", _google_token);
  RegWriteString(L"TableId", std::to_wstring(_table_id));
}

void CScarletBeast::SetScrapeFromServer(bool on) { _scrape_from_server = on; SaveToRegistry(); }
void CScarletBeast::SetTableId(int id) { _table_id = (id > 0 ? id : 1); SaveToRegistry(); }
void CScarletBeast::SetBaseUrl(const std::wstring& url) { _base_url = url; SaveToRegistry(); }
void CScarletBeast::SetApiKey(const std::wstring& key) { _api_key = key; SaveToRegistry(); }
void CScarletBeast::SetGoogleToken(const std::wstring& token) { _google_token = token; SaveToRegistry(); }

// --------------------------------------------------------------- WinHTTP ----

std::string CScarletBeast::Request(const std::wstring& method, const std::wstring& path,
                                   const std::string& body, bool with_auth) {
  _last_ok = false;
  _last_status = 0;
  _last_error.clear();
  std::string response;

  HINTERNET hSession = WinHttpOpen(L"Hiss-ScarletBeast/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) { _last_error = L"WinHttpOpen failed"; return response; }

  HINTERNET hConnect = WinHttpConnect(hSession, _base_url.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!hConnect) { _last_error = L"WinHttpConnect failed"; WinHttpCloseHandle(hSession); return response; }

  HINTERNET hRequest = WinHttpOpenRequest(hConnect, method.c_str(), path.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          WINHTTP_FLAG_SECURE);
  if (!hRequest) {
    _last_error = L"WinHttpOpenRequest failed";
    WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    return response;
  }

  std::wstring headers = L"Accept: application/json\r\nContent-Type: application/json\r\n";
  if (with_auth && !_api_key.empty()) headers += L"Authorization: Bearer " + _api_key + L"\r\n";

  BOOL ok = WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(-1),
                               body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                               static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
  if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);

  if (ok) {
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    _last_status = static_cast<int>(status);
    _last_ok = (status >= 200 && status < 300);

    DWORD avail = 0;
    do {
      avail = 0;
      if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
      std::vector<char> buf(avail + 1, 0);
      DWORD read = 0;
      if (WinHttpReadData(hRequest, buf.data(), avail, &read) && read) response.append(buf.data(), read);
    } while (avail > 0);
  } else {
    _last_error = L"request/receive failed";
  }

  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
  return response;
}

// ------------------------------------------------------------------ REST ----

std::string CScarletBeast::SeatView(int table_id) {
  return Request(L"GET", L"/api/v1/tables/" + std::to_wstring(table_id), "", true);
}
std::string CScarletBeast::Observe(int table_id) {
  return Request(L"GET", L"/api/v1/tables/" + std::to_wstring(table_id) + L"/observe", "", false);
}
std::string CScarletBeast::Act(int table_id, const std::string& json_body) {
  return Request(L"POST", L"/api/v1/tables/" + std::to_wstring(table_id) + L"/act", json_body, true);
}

// --------------------------------------------------------------- GraphQL ----

std::string CScarletBeast::GraphQL(const std::string& query, const std::string& variables_json) {
  // Build {"query":"...","variables":{...}} with minimal escaping of the query.
  std::string esc;
  esc.reserve(query.size() + 16);
  for (char c : query) {
    if (c == '"') esc += "\\\"";
    else if (c == '\\') esc += "\\\\";
    else if (c == '\n') esc += "\\n";
    else esc += c;
  }
  std::string body = "{\"query\":\"" + esc + "\",\"variables\":" + variables_json + "}";
  return Request(L"POST", L"/console/graphql", body, true);
}

// Fetch the seat view at most ~once per 120 ms; cache the raw JSON so the scraper
// (table-state) and the symbol engine (sb_* symbols) share one network round-trip
// per heartbeat instead of hitting the API twice.
bool CScarletBeast::RefreshSeatView() {
  unsigned long now = ::GetTickCount();
  if (!_seat_json_cache.empty() && _seat_json_tick != 0 && (now - _seat_json_tick) < 120) {
    return true;  // reuse the very recent fetch (same heartbeat)
  }
  std::string seat = SeatView(TableId());
  if (!_last_ok || seat.empty()) return false;
  _seat_json_cache = seat;
  _seat_json_tick = now;
  return true;
}

// Pull the seat view and flatten the parts the symbol engine cares about into
// tablemap-style symbols. Uses tiny extractors instead of a JSON dependency.
bool CScarletBeast::PopulateSymbols(int table_id, std::map<std::string, std::string>& out) {
  if (!RefreshSeatView()) return false;
  const std::string& seat = _seat_json_cache;

  // Headline fields the symbol engine can map onto OpenHoldem symbols.
  out["sb_table_id"] = std::to_string(table_id);
  out["sb_pot"] = std::to_string(ExtractJsonNumber(seat, "pot", 0));
  out["sb_to_call"] = std::to_string(ExtractJsonNumber(seat, "to_call", 0));
  out["sb_to_act"] = std::to_string(ExtractJsonNumber(seat, "to_act", -1));
  out["sb_my_seat"] = std::to_string(ExtractJsonNumber(seat, "seat_no", -1));
  out["sb_street"] = ExtractJsonString(seat, "street");
  out["sb_board"] = ExtractJsonString(seat, "board");
  out["sb_hole"] = ExtractJsonString(seat, "hole");
  out["sb_raw"] = seat;  // full payload for advanced parsing in the symbol engine
  return true;
}

// ----------------------------------------------------- HUD (PT stats) -------
// Balanced { ... } block (inclusive) starting at brace_pos; respects strings.
static std::string HUD_BracedBlock(const std::string& s, size_t brace_pos) {
  if (brace_pos >= s.size() || s[brace_pos] != '{') return "";
  int depth = 0;
  bool in_str = false;
  for (size_t i = brace_pos; i < s.size(); ++i) {
    char c = s[i];
    if (in_str) {
      if (c == '\\') { ++i; continue; }
      if (c == '"') in_str = false;
    } else if (c == '"') { in_str = true; }
    else if (c == '{') { ++depth; }
    else if (c == '}') { if (--depth == 0) return s.substr(brace_pos, i - brace_pos + 1); }
  }
  return "";
}

// Value object for "key": { ... }.
static std::string HUD_Object(const std::string& s, const std::string& key) {
  std::string needle = "\"" + key + "\"";
  size_t p = s.find(needle);
  while (p != std::string::npos) {
    size_t c = s.find(':', p + needle.size());
    if (c != std::string::npos) {
      size_t q = c + 1;
      while (q < s.size() && (s[q] == ' ' || s[q] == '\t' || s[q] == '\n' || s[q] == '\r')) ++q;
      if (q < s.size() && s[q] == '{') return HUD_BracedBlock(s, q);
    }
    p = s.find(needle, p + needle.size());
  }
  return "";
}

// Floating-point value of "key": N (handles negatives and decimals).
static bool HUD_Number(const std::string& s, const std::string& key, double* out) {
  std::string needle = "\"" + key + "\"";
  size_t p = s.find(needle);
  if (p == std::string::npos) return false;
  p = s.find(':', p + needle.size());
  if (p == std::string::npos) return false;
  ++p;
  while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
  size_t start = p;
  if (p < s.size() && (s[p] == '-' || s[p] == '+')) ++p;
  bool any = false;
  while (p < s.size() && ((s[p] >= '0' && s[p] <= '9') || s[p] == '.')) { ++p; any = true; }
  if (!any) return false;
  *out = atof(s.substr(start, p - start).c_str());
  return true;
}

// Map a PT-symbol basic name (no "pt_" prefix / chair suffix) to the API HUD key.
// Normalises by lower-casing and dropping underscores, then matches known aliases.
static std::string MapPtStatToApiKey(const char* basic_stat) {
  std::string s;
  for (const char* p = basic_stat; *p; ++p) {
    char c = *p;
    if (c == '_') continue;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    s += c;
  }
  struct Pair { const char* alias; const char* key; };
  static const Pair kMap[] = {
    {"vpip","vpip"}, {"pfr","pfr"}, {"hands","hands"}, {"wtsd","wtsd"},
    {"wsd","wsd"}, {"wonsd","wsd"}, {"wwsf","wwsf"}, {"wonwhensawflop","wwsf"},
    {"bb100","bb100"}, {"winrate","bb100"}, {"bbper100","bb100"},
    {"af","af"}, {"totalaf","af"}, {"aggression","af"}, {"aggressionfactor","af"},
    {"flopaf","af"}, {"turnaf","af"}, {"riveraf","af"},  // street AF -> total AF proxy (API has no per-street AF)
    {"3bet","threebet"}, {"threebet","threebet"}, {"pf3bet","threebet"}, {"3betpf","threebet"},
    {"fold3bet","fold3bet"}, {"foldto3bet","fold3bet"}, {"f3bet","fold3bet"}, {"foldpf3bet","fold3bet"},
    {"4bet","fourbet"}, {"fourbet","fourbet"},
    {"fold4bet","fold4bet"}, {"foldto4bet","fold4bet"}, {"f4bet","fold4bet"},
    {"cbet","cbetf"}, {"cbetf","cbetf"}, {"flopcbet","cbetf"}, {"cbetflop","cbetf"},
    {"cbett","cbett"}, {"turncbet","cbett"}, {"cbetturn","cbett"},
    {"cbetr","cbetr"}, {"rivercbet","cbetr"}, {"cbetriver","cbetr"},
    {"foldcbet","foldcbetf"}, {"foldcbetf","foldcbetf"}, {"foldflopcbet","foldcbetf"}, {"foldtocbet","foldcbetf"},
    {"foldcbett","foldcbett"}, {"foldturncbet","foldcbett"},
    {"foldcbetr","foldcbetr"}, {"foldrivercbet","foldcbetr"},
  };
  for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); ++i) {
    if (s == kMap[i].alias) return kMap[i].key;
  }
  return "";
}

bool CScarletBeast::RefreshHud() {
  unsigned long now = ::GetTickCount();
  // HUD stats move slowly; refresh at most ~once every 3 s.
  if (!_hud_json_cache.empty() && _hud_tick != 0 && (now - _hud_tick) < 3000) return true;
  std::wstring path = L"/api/v1/tables/" + std::to_wstring((long)TableId()) + L"/hud";
  std::string body = Request(L"GET", path, "", true);
  if (!_last_ok || body.empty()) return false;
  _hud_json_cache = body;
  _hud_tick = now;
  return true;
}

bool CScarletBeast::HudStatForChair(int chair, const char* basic_stat, double* out_value) {
  if (chair < 0 || out_value == NULL) return false;
  if (!RefreshHud()) return false;
  std::string key = MapPtStatToApiKey(basic_stat);
  if (key.empty()) return false;  // stat not provided by the API
  std::string seats = HUD_Object(_hud_json_cache, "seats");
  if (seats.empty()) return false;
  char seat_no[8];
  sprintf_s(seat_no, sizeof(seat_no), "%d", chair + 1);  // API seats are 1-based
  std::string seat = HUD_Object(seats, seat_no);
  if (seat.empty()) return false;
  return HUD_Number(seat, key, out_value);
}

// --------------------------------------------- server acting context --------
// These read top-level/hand scalars from the cached seat view. Each of these keys
// occurs exactly once in the payload, so the first-match extractor is safe.

long CScarletBeast::ServerToAct() {
  if (_seat_json_cache.empty()) return -1;
  return ExtractJsonNumber(_seat_json_cache, "to_act", -1);
}
long CScarletBeast::ServerCurrentBet() {
  if (_seat_json_cache.empty()) return 0;
  return ExtractJsonNumber(_seat_json_cache, "current_bet", 0);
}
long CScarletBeast::ServerMinRaise(long def) {
  if (_seat_json_cache.empty()) return def;
  return ExtractJsonNumber(_seat_json_cache, "min_raise", def);
}
long CScarletBeast::ServerStateVersion() {
  if (_seat_json_cache.empty()) return 0;
  return ExtractJsonNumber(_seat_json_cache, "version", 0);
}

// --------------------------------------------------------- tiny JSON utils ----
// Deliberately minimal: find "key": then read the string or number after it.

std::string CScarletBeast::ExtractJsonString(const std::string& json, const std::string& key) {
  std::string needle = "\"" + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) return "";
  p = json.find(':', p + needle.size());
  if (p == std::string::npos) return "";
  // skip ws
  ++p;
  while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
  if (p >= json.size() || json[p] != '"') return "";
  ++p;
  std::string out;
  while (p < json.size() && json[p] != '"') {
    if (json[p] == '\\' && p + 1 < json.size()) { out += json[p + 1]; p += 2; }
    else out += json[p++];
  }
  return out;
}

long CScarletBeast::ExtractJsonNumber(const std::string& json, const std::string& key, long def) {
  std::string needle = "\"" + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) return def;
  p = json.find(':', p + needle.size());
  if (p == std::string::npos) return def;
  ++p;
  while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
  bool neg = false;
  if (p < json.size() && json[p] == '-') { neg = true; ++p; }
  if (p >= json.size() || json[p] < '0' || json[p] > '9') return def;
  long v = 0;
  while (p < json.size() && json[p] >= '0' && json[p] <= '9') { v = v * 10 + (json[p] - '0'); ++p; }
  return neg ? -v : v;
}

std::wstring CScarletBeast::Widen(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}
std::string CScarletBeast::Narrow(const std::wstring& s) {
  if (s.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
  std::string o(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &o[0], n, nullptr, nullptr);
  return o;
}
