#ifndef INC_CSCARLETBEAST_H
#define INC_CSCARLETBEAST_H

// CScarletBeast — the Scarlet Beast online bridge for Hiss.
//
// A self-contained WinHTTP client (no external libraries) that lets Hiss scrape
// from poker.scarletbeast.com instead of (or alongside) screen-scraping:
//   * REST   — the documented machine gate at /api/v1 for seat/table state.
//   * GraphQL — /console/graphql to populate tablemap symbols from the server.
//
// Configuration (scrape source on/off, base URL, API key, linked Google token)
// is persisted in the registry under HKCU\Software\ScarletBeast so it survives
// across runs and is shared by every spawned Hiss instance.

#include <string>
#include <map>

class CScarletBeast {
 public:
  CScarletBeast();
  ~CScarletBeast();

  // ---- configuration (registry-backed) ----
  bool   ScrapeFromServer() const { return _scrape_from_server; }
  void   SetScrapeFromServer(bool on);
  std::wstring BaseUrl() const { return _base_url; }
  void   SetBaseUrl(const std::wstring& url);
  std::wstring ApiKey() const { return _api_key; }
  void   SetApiKey(const std::wstring& key);
  std::wstring GoogleToken() const { return _google_token; }
  void   SetGoogleToken(const std::wstring& token);
  void   LoadFromRegistry();
  void   SaveToRegistry();

  // ---- REST (the felt loop) ----
  // GET <base>/api/v1/tables/{id} — your seat view (hole cards + legal actions).
  std::string SeatView(int table_id);
  // GET <base>/api/v1/tables/{id}/observe — public table snapshot.
  std::string Observe(int table_id);
  // POST act.  body is raw JSON, e.g. {"action":"call"}.
  std::string Act(int table_id, const std::string& json_body);

  // ---- GraphQL (symbol population) ----
  // Runs a GraphQL query against <base>/console/graphql and returns raw JSON.
  std::string GraphQL(const std::string& query, const std::string& variables_json = "{}");

  // Pull table/seat state from the server and flatten it into tablemap-style
  // symbols (key -> string value) the symbol engine can consume.
  // Returns true on success; fills `out_symbols`.
  bool PopulateSymbols(int table_id, std::map<std::string, std::string>& out_symbols);

  // True once a request has produced a 2xx; useful for the settings "Test" button.
  bool LastOk() const { return _last_ok; }
  int  LastStatus() const { return _last_status; }
  std::wstring LastError() const { return _last_error; }

 private:
  // Core WinHTTP request.  method = L"GET"/L"POST"; path may be absolute or
  // relative to the base host.  Returns the response body (UTF-8).
  std::string Request(const std::wstring& method, const std::wstring& path,
                      const std::string& body, bool with_auth);

  // Tiny helpers (avoid pulling in a JSON lib for the client itself).
  static std::string  ExtractJsonString(const std::string& json, const std::string& key);
  static long         ExtractJsonNumber(const std::string& json, const std::string& key, long def);
  static std::wstring Widen(const std::string& s);
  static std::string  Narrow(const std::wstring& s);

  bool         _scrape_from_server;
  std::wstring _base_url;       // e.g. L"poker.scarletbeast.com"
  std::wstring _api_key;        // sbp_...
  std::wstring _google_token;   // linked identity token

  bool         _last_ok;
  int          _last_status;
  std::wstring _last_error;
};

// Single shared instance for the process (declared in the .cpp).
extern CScarletBeast* p_scarlet_beast;

#endif  // INC_CSCARLETBEAST_H
