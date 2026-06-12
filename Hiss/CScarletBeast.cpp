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
      _last_ok(false),
      _last_status(0) {
  LoadFromRegistry();
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
}

void CScarletBeast::SaveToRegistry() {
  RegWriteString(L"ScrapeFromServer", _scrape_from_server ? L"1" : L"0");
  RegWriteString(L"BaseUrl", _base_url);
  RegWriteString(L"ApiKey", _api_key);
  RegWriteString(L"GoogleToken", _google_token);
}

void CScarletBeast::SetScrapeFromServer(bool on) { _scrape_from_server = on; SaveToRegistry(); }
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

// Pull the seat view and flatten the parts the symbol engine cares about into
// tablemap-style symbols. Uses tiny extractors instead of a JSON dependency.
bool CScarletBeast::PopulateSymbols(int table_id, std::map<std::string, std::string>& out) {
  std::string seat = SeatView(table_id);
  if (!_last_ok || seat.empty()) return false;

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
