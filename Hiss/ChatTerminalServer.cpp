#include "stdafx.h"
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#include "ChatTerminalServer.h"
#include "ChatTerminalWindow.h"
#include "CEngineContainer.h"
#include "UnknownSymbols.h"
#include "..\Shared\MagicNumbers\MagicNumbers.h"
#include "CSymbolEngineTableLimits.h"
#include "CSymbolEngineGameType.h"
#include "CSymbolEngineIsOmaha.h"
#include "CSymbolEngineChipAmounts.h"
#include "CSymbolEngineValidator.h"
#include "CSymbolEngineUserchair.h"
#include "CSymbolEngineAutoplayer.h"
#include "CHeartbeatThread.h"
#include "CScarletBeast.h"
#include "CHandresetDetector.h"
#include "CTableState.h"
#include "CScraper.h"
#include "CAutoplayer.h"
#include "CPokerTrackerThread.h"
#include "COCRNameMapping.h"
#include "HudManager.h"
#include "..\CTablemap\CTablemap.h"
#include "..\CTablemap\CTablemapDB.h"   // p_tablemap_db: automation-prefs storage
#include "..\DLLs\Files_DLL\Files.h"
#include "CAutoconnector.h"   // attached_hwnd(), for the manual window override
#include "CSharedMem.h"       // PokerWindowAttached(), to flag windows another instance serves

#pragma comment(lib, "ws2_32.lib")

// ---- /api/symbols snapshot (see ChatTerminalServer.h) -------------------------------------------
// The heartbeat WRITES this; HTTP threads only READ it. Guarded by its own small lock, never by
// cs_update_in_progress, so a reader can no longer stall the bot.
#include <map>
#include <set>
#include <vector>

static std::map<CStringA, CStringA> g_symbols_snapshot;  // name -> JSON value fragment
static std::set<CStringA>           g_symbols_wanted;    // only what someone actually reads

// The lock must be initialized EXACTLY once. A plain "if (!ready) InitializeCriticalSection()" is a
// race: the HTTP thread and the heartbeat thread both call it, both can see !ready, and
// initializing the same CRITICAL_SECTION twice corrupts it -- which then hangs whoever enters it.
// (I wrote that race, and it wedged the endpoint exactly the way the original bug did.) A
// function-local static is initialized thread-safely by the C++ runtime; let it do the work.
struct SymbolsCriticalSection {
  CRITICAL_SECTION cs;
  SymbolsCriticalSection()  { InitializeCriticalSection(&cs); }
  ~SymbolsCriticalSection() { DeleteCriticalSection(&cs); }
};

static CRITICAL_SECTION *SymbolsCs() {
  static SymbolsCriticalSection instance;   // thread-safe init (magic statics)
  return &instance.cs;
}

#define g_symbols_cs (*SymbolsCs())

static void EnsureSymbolsCs() { SymbolsCs(); }

// ---------------------------------------------------------------------------------------------
// Top-level window enumeration, for the toolbar's "connect to window" picker.
//
// Nothing reusable existed: WindowFunctions_DLL has no enumerator, and Hiss's own STableList is
// compiled without the title/rect fields (they only exist in the Vision project), so the picker
// needs its own collector.
// ---------------------------------------------------------------------------------------------
struct SPickableWindow {
  HWND    hwnd;
  CString title;
  CString cls;
  int     width;
  int     height;
};

static BOOL CALLBACK EnumPickableWindows(HWND hwnd, LPARAM lparam) {
  std::vector<SPickableWindow> *out = (std::vector<SPickableWindow> *)lparam;
  if (out == NULL) return FALSE;
  // The autoconnector can only ever attach to a visible, non-degenerate window
  // (CAutoConnector.cpp:184), so anything else would be an unselectable entry in the list.
  if (!::IsWindowVisible(hwnd)) return TRUE;
  RECT r = { 0, 0, 0, 0 };
  if (!::GetWindowRect(hwnd, &r)) return TRUE;
  const int w = r.right - r.left;
  const int h = r.bottom - r.top;
  if (w <= 0 || h <= 0) return TRUE;
  char title[512] = { 0 };
  ::GetWindowTextA(hwnd, title, sizeof(title) - 1);
  if (title[0] == '\0') return TRUE;   // untitled: nothing for a human to pick by
  char cls[128] = { 0 };
  ::GetClassNameA(hwnd, cls, sizeof(cls) - 1);
  SPickableWindow info;
  info.hwnd   = hwnd;
  info.title  = CString(title);
  info.cls    = CString(cls);
  info.width  = w;
  info.height = h;
  out->push_back(info);
  return TRUE;
}

// CChatTerminalServer::JsonEscape is a member; these helpers are free functions (the heartbeat calls
// them without an instance), so they carry their own.
// ---- Automation preferences (toolbar gear -> /automation-prefs/) ---------------------------
//
// Stored in the shared `settings` table under key "automation_api".
//
// NOT "automation_prefs": Automation.exe (the region mapper) already owns that key for its own
// UI preferences. Two different programs writing one settings row would quietly overwrite each
// other's fields.
//
// These are the credentials Hiss uses to reach the tournament console at
// poker.scarletbeast.com/automation. The bot POLLS that API; each authenticated call also stamps
// users.bot_seen_at server-side, which is what makes this instance appear as a live, selectable
// bot in the console's dropdown.
static const char *kAutomationPrefsKey = "automation_api";

// The fields the React page reads and writes. Kept in one place so GET, POST and the connection
// test can never drift apart on spelling.
static const char *kAutomationPrefsFields[] = {
	"api_base", "api_token", "bot_name", "poll_seconds", "join_window_minutes", "enabled"
};

// PER-INSTANCE vs MACHINE-WIDE.
//
// Several Hiss instances run at once, one per scrcpy mirror, and each is a SEPARATE bot on
// /automation -- that is the whole point of picking a bot there: the tournament gets joined on
// that mirror's client. So the identity fields are stored per TERMINAL PORT (api_token_27655,
// ...), which is the only stable per-instance handle we have. Stored under one shared key they
// would all authenticate as the same user and collapse into a single row in the dropdown,
// making the choice meaningless.
//
// The rest genuinely IS machine-wide -- same site, same cadence -- so it stays unsuffixed and is
// edited once from any instance's gear.
static bool AutomationFieldIsPerInstance(const char *field) {
	return (strcmp(field, "api_token") == 0)
		|| (strcmp(field, "bot_name") == 0)
		|| (strcmp(field, "enabled") == 0);
}

static CString AutomationPrefField(const char *field) {
	extern int g_terminal_port;
	if (!AutomationFieldIsPerInstance(field) || g_terminal_port <= 0) {
		return CString(field);
	}
	CString out;
	out.Format("%s_%d", field, g_terminal_port);
	return out;
}

// Is this key actually present in the query string? QueryValue() cannot answer that: it
// returns "" for an absent key and for "key=" alike. The prefs POST needs the distinction so a
// caller can save one field without blanking the rest.
static bool AutomationQueryHas(const CStringA &query, const char *name) {
	int start = 0;
	CStringA want(name);
	while (start <= query.GetLength()) {
		int end = query.Find('&', start);
		CStringA pair = end >= 0 ? query.Mid(start, end - start) : query.Mid(start);
		int equals = pair.Find('=');
		CStringA key = equals >= 0 ? pair.Left(equals) : pair;
		if (key.CompareNoCase(want) == 0) {
			return true;
		}
		if (end < 0) break;
		start = end + 1;
	}
	return false;
}

static DWORD AutomationApiGet(const CStringA &base_url, const CStringA &path,
                              const CStringA &token, CStringA *body) {
	if (body != NULL) *body = "";
	CStringA host = base_url;
	host.Replace("https://", "");
	host.Replace("http://", "");
	int slash = host.Find('/');
	if (slash >= 0) host = host.Left(slash);
	host.Trim();
	if (host.IsEmpty()) return 0;

	CStringW hostw(host), pathw(path);
	HINTERNET session = WinHttpOpen(L"Hiss-Automation/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (session == NULL) return 0;
	WinHttpSetTimeouts(session, 3000, 3000, 3000, 5000);

	HINTERNET connect = WinHttpConnect(session, hostw.GetString(), INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (connect == NULL) { WinHttpCloseHandle(session); return 0; }

	HINTERNET request = WinHttpOpenRequest(connect, L"GET", pathw.GetString(), NULL,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (request == NULL) {
		WinHttpCloseHandle(connect); WinHttpCloseHandle(session);
		return 0;
	}

	CStringW headers = L"Accept: application/json\r\n";
	if (!token.IsEmpty()) {
		headers += L"Authorization: Bearer " + CStringW(token) + L"\r\n";
	}
	DWORD status = 0;
	if (WinHttpSendRequest(request, headers.GetString(), (DWORD)-1,
			WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
		&& WinHttpReceiveResponse(request, NULL)) {
		DWORD size = sizeof(status);
		WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
		if (body != NULL) {
			DWORD avail = 0;
			while (WinHttpQueryDataAvailable(request, &avail) && avail > 0) {
				if (avail > 8192) avail = 8192;
				char *buf = new char[avail + 1];
				DWORD read = 0;
				if (WinHttpReadData(request, buf, avail, &read) && read > 0) {
					buf[read] = '\0';
					*body += buf;
				}
				delete[] buf;
				if (body->GetLength() > 65536) break;   // a sane cap; we only need the first object
			}
		}
	}
	WinHttpCloseHandle(request);
	WinHttpCloseHandle(connect);
	WinHttpCloseHandle(session);
	return status;
}

// ---- Automation heartbeat/poller ----------------------------------------------------------
//
// Calls the tournament console's /api/v1/automation/poll on an interval. Two jobs in one call:
//   * PRESENCE -- the server stamps users.bot_seen_at on any authenticated request, and the
//     console marks a bot "active" when that is within 10 minutes. This poll IS what makes this
//     instance appear, and stay lit, in the bot dropdown on /automation.
//   * WORK -- the response carries any join jobs queued for this bot inside their registration
//     window.
//
// ON ITS OWN THREAD, deliberately. The obvious place would be the heartbeat, but a network call
// there stalls the bot for the request's duration: this file's header records exactly that
// failure ("a hung request used to freeze the whole bot ~31s per cycle"). The poker loop must
// never wait on poker.scarletbeast.com being reachable.
//
// Preferences are re-read every cycle rather than cached, so an edit in the gear's page takes
// effect on the next tick with no restart.
static volatile bool g_automation_poll_stop = false;

static UINT AutomationPollThread(LPVOID) {
	int wait_seconds = 30;
	while (!g_automation_poll_stop) {
		CStringA base, token, enabled;
		if (p_tablemap_db != NULL) {
			base    = CStringA(p_tablemap_db->GetSettingString(kAutomationPrefsKey, AutomationPrefField("api_base")));
			token   = CStringA(p_tablemap_db->GetSettingString(kAutomationPrefsKey, AutomationPrefField("api_token")));
			enabled = CStringA(p_tablemap_db->GetSettingString(kAutomationPrefsKey, AutomationPrefField("enabled")));
			int parsed = atoi(CStringA(p_tablemap_db->GetSettingString(kAutomationPrefsKey, AutomationPrefField("poll_seconds"))));
			// Floor of 5s: a mis-typed 0 would spin this loop against the site as fast as the
			// network allows.
			wait_seconds = (parsed >= 5) ? parsed : 30;
		}
		bool on = (enabled.CompareNoCase("on") == 0 || enabled == "1");
		if (on && !base.IsEmpty() && !token.IsEmpty()) {
			CStringA resp;
			DWORD status = AutomationApiGet(base, "/api/v1/automation/poll", token, &resp);
			// Log only on CHANGE. A 30s poll logging every cycle adds ~2900 lines a day saying
			// nothing; the transitions (came up / went 401 / went dark) are what matter.
			static DWORD s_last_status = 0xFFFFFFFF;
			if (status != s_last_status) {
				s_last_status = status;
				if (status == 200) {
					write_log(k_always_log_basic_information,
						"[Automation] poll OK -- this bot is live on /automation.\n");
				} else if (status == 401) {
					write_log(k_always_log_errors,
						"[Automation] poll REJECTED (401): wrong API token. Fix it in the gear's preferences.\n");
				} else if (status == 0) {
					write_log(k_always_log_errors,
						"[Automation] poll could not reach %s -- this bot will not appear on /automation.\n",
						base.GetString());
				} else {
					write_log(k_always_log_errors, "[Automation] poll returned HTTP %lu.\n", status);
				}
			}
			// The job list only becomes actionable once automation.exe can drive the client;
			// until then the poll still serves its presence purpose. Left intentionally unparsed.
		}
		for (int i = 0; i < wait_seconds * 4 && !g_automation_poll_stop; ++i) {
			Sleep(250);   // short slices so shutdown is prompt, not up to a full interval
		}
	}
	return 0;
}

static CStringA JsonEscapeFree(const CString &value) {
  CStringA out;
  CStringA in(value);
  for (int i = 0; i < in.GetLength(); ++i) {
    char c = in[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((unsigned char) c < 0x20) {
          CStringA esc; esc.Format("\\u%04x", (unsigned char) c);
          out += esc;
        } else {
          out += c;
        }
    }
  }
  return out;
}

static void SplitNames(const CStringA &names, std::vector<CStringA> *out) {
  int start = 0;
  while (start <= names.GetLength()) {
    int comma = names.Find(',', start);
    CStringA one = comma >= 0 ? names.Mid(start, comma - start) : names.Mid(start);
    one.Trim();
    if (!one.IsEmpty()) out->push_back(one);
    if (comma < 0) break;
    start = comma + 1;
  }
}

void WantSymbols(const CStringA &names) {
  EnsureSymbolsCs();
  std::vector<CStringA> list;
  SplitNames(names, &list);
  EnterCriticalSection(&g_symbols_cs);
  for (size_t i = 0; i < list.size(); ++i) g_symbols_wanted.insert(list[i]);
  LeaveCriticalSection(&g_symbols_cs);
}

bool SymbolsFromSnapshot(const CStringA &names, CStringA *json_body) {
  EnsureSymbolsCs();
  std::vector<CStringA> list;
  SplitNames(names, &list);
  CStringA body = "{";
  bool first = true;
  bool complete = true;
  EnterCriticalSection(&g_symbols_cs);
  for (size_t i = 0; i < list.size(); ++i) {
    std::map<CStringA, CStringA>::const_iterator it = g_symbols_snapshot.find(list[i]);
    if (it == g_symbols_snapshot.end()) { complete = false; break; }   // never snapshotted yet
    if (!first) body += ",";
    first = false;
    body += "\"" + JsonEscapeFree(CString(list[i])) + "\":" + it->second;
  }
  LeaveCriticalSection(&g_symbols_cs);
  if (!complete) return false;
  body += "}";
  *json_body = body;
  return true;
}

// Called from the heartbeat, INSIDE cs_update_in_progress, right after the engines were evaluated.
// Only the names somebody has actually asked for get refreshed, so this stays proportional to what
// is being read (the driver's ~110) rather than to the whole symbol space.
void RefreshSymbolSnapshot(void) {
  EnsureSymbolsCs();
  if (p_engine_container == NULL) return;

  std::vector<CStringA> wanted;
  EnterCriticalSection(&g_symbols_cs);
  for (std::set<CStringA>::const_iterator it = g_symbols_wanted.begin();
       it != g_symbols_wanted.end(); ++it) {
    wanted.push_back(*it);
  }
  LeaveCriticalSection(&g_symbols_cs);
  if (wanted.empty()) return;

  // Evaluate OUTSIDE our little lock (we already hold the heartbeat's), then publish in one go.
  std::map<CStringA, CStringA> fresh;
  g_suppress_unknown_symbol_warning = true;     // a typo must never pop a modal on the heartbeat
  for (size_t i = 0; i < wanted.size(); ++i) {
    CString name(wanted[i]);
    double dval = 0.0;
    CString sval;
    CStringA frag;
    if (p_engine_container->EvaluateSymbol(name, &dval, false)) {
      frag.Format("%.4f", dval);
    } else if (p_engine_container->EvaluateSymbol(name, &sval, false)) {
      frag = "\"" + JsonEscapeFree(sval) + "\"";
    } else {
      frag = "null";
    }
    fresh[wanted[i]] = frag;
  }
  g_suppress_unknown_symbol_warning = false;

  EnterCriticalSection(&g_symbols_cs);
  for (std::map<CStringA, CStringA>::const_iterator it = fresh.begin(); it != fresh.end(); ++it) {
    g_symbols_snapshot[it->first] = it->second;
  }
  LeaveCriticalSection(&g_symbols_cs);
}

CChatTerminalServer *p_chat_terminal_server = NULL;

CChatTerminalServer::CChatTerminalServer()
{
	_thread = NULL;
	_listen_socket = INVALID_SOCKET;
	_port = 0;
	_stop = false;
}

CChatTerminalServer::~CChatTerminalServer()
{
	Stop();
}

bool CChatTerminalServer::Start(unsigned short port)
{
	if (_thread != NULL) {
		return true;
	}

	WSADATA wsa_data;
	if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
		return false;
	}

	// Try the requested port, then up to 9 successors. This lets multiple
	// Hiss instances run simultaneously: each binds the next free port and
	// the embedded React page (relative /api URL) follows automatically.
	// We deliberately do NOT set SO_REUSEADDR so a second instance fails
	// fast on a port already in use and moves on to the next.
	const int kMaxPortAttempts = 10;
	SOCKET listen_socket = INVALID_SOCKET;
	unsigned short bound_port = 0;
	for (int attempt = 0; attempt < kMaxPortAttempts; ++attempt) {
		unsigned short candidate = (unsigned short)(port + attempt);
		listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listen_socket == INVALID_SOCKET) {
			break;
		}
		sockaddr_in address;
		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(candidate);
		if (::bind(listen_socket, (sockaddr *)&address, sizeof(address)) == 0
				&& ::listen(listen_socket, SOMAXCONN) == 0) {
			bound_port = candidate;
			break;
		}
		closesocket(listen_socket);
		listen_socket = INVALID_SOCKET;
	}

	if (bound_port == 0) {
		ChatTerminalAppend(kChatTerminalContext, "Terminal API server failed to bind a localhost port.");
		WSACleanup();
		return false;
	}

	_listen_socket = listen_socket;
	_port = bound_port;
	g_terminal_port = bound_port;   // expose the bound port so the NN driver can be aimed at us
	_stop = false;
	_thread = AfxBeginThread(ServerThread, this, THREAD_PRIORITY_NORMAL, 0, CREATE_SUSPENDED, NULL);
	if (_thread == NULL) {
		closesocket(_listen_socket);
		_listen_socket = INVALID_SOCKET;
		WSACleanup();
		return false;
	}
	_thread->m_bAutoDelete = false;
	_thread->ResumeThread();

	CString ready;
	// Automation presence/work poller (own thread -- see AutomationPollThread).
	// EXACTLY ONE per process. Start() can run more than once in a process (a port-bind retry, a
	// server restart), and each extra call used to spawn another poller: redundant HTTP calls to
	// the same endpoint on overlapping schedules, and a shared "log only on change" static that
	// several threads then defeat -- which is precisely how this was noticed, as the same 401
	// repeating every ~20s instead of once.
	static bool s_poller_started = false;
	if (!s_poller_started) {
		s_poller_started = true;
		g_automation_poll_stop = false;
		AfxBeginThread(AutomationPollThread, NULL, THREAD_PRIORITY_BELOW_NORMAL);
	}

	ready.Format("Terminal API server listening on http://127.0.0.1:%u", _port);
	ChatTerminalAppend(kChatTerminalContext, ready);
	// Publish the chosen port so the MCP server (and any external tool) can attach
	// to this hiss.exe without guessing.
	CString port_dir = OpenHoldemDirectory() + "\\logs";
	CreateDirectory(port_dir, NULL);
	CString port_file = port_dir + "\\terminal_port.txt";
	FILE *pf = NULL;
	if (fopen_s(&pf, port_file.GetString(), "w") == 0 && pf != NULL) {
		fprintf(pf, "%u\n", _port);
		fclose(pf);
	}
	return true;
}

void CChatTerminalServer::Stop(void)
{
	_stop = true;
	// Signal the automation poller too. It wakes in 250ms slices, so it exits promptly instead
	// of leaving a thread mid-WinHTTP-call while the process tears down around it.
	g_automation_poll_stop = true;
	if (_listen_socket != INVALID_SOCKET) {
		closesocket(_listen_socket);
		_listen_socket = INVALID_SOCKET;
	}
	if (_thread != NULL) {
		WaitForSingleObject(_thread->m_hThread, 2000);
		_thread = NULL;
	}
	WSACleanup();
}

UINT CChatTerminalServer::ServerThread(LPVOID param)
{
	CChatTerminalServer *server = (CChatTerminalServer *)param;
	if (server != NULL) {
		server->Run();
	}
	return 0;
}

void CChatTerminalServer::Run(void)
{
	// The listen socket is already bound/listening (set up in Start()).
	while (!_stop && _listen_socket != INVALID_SOCKET) {
		SOCKET client = ::accept(_listen_socket, NULL, NULL);
		if (client == INVALID_SOCKET) {
			if (!_stop) {
				Sleep(25);
			}
			continue;
		}
		HandleClient(client);
		closesocket(client);
	}

	if (_listen_socket != INVALID_SOCKET) {
		closesocket(_listen_socket);
		_listen_socket = INVALID_SOCKET;
	}
}

void CChatTerminalServer::HandleClient(SOCKET client)
{
	char buffer[8192];
	int received = recv(client, buffer, sizeof(buffer) - 1, 0);
	if (received <= 0) {
		return;
	}
	buffer[received] = 0;
	CStringA request(buffer, received);

	// READ THE WHOLE BODY, not just whatever the first packet happened to carry.
	//
	// A single recv() returns what has ARRIVED, which for a POST is frequently the headers plus
	// only part of the body -- TCP is free to split them. Every earlier endpoint took its input
	// from the query string, so nothing depended on the body and the truncation was invisible.
	// The automation-prefs POST is the first body-driven endpoint, and it silently received 54 of
	// 62 bytes: valid-looking JSON with the opening of the first key sliced off, so the field
	// parsed empty and the save became a no-op that still answered {"ok":true}.
	// Keep reading until Content-Length bytes of body are in hand (or the peer stops sending).
	{
		int hdr_end = request.Find("\r\n\r\n");
		if (hdr_end >= 0) {
			int content_length = 0;
			int cl = request.Find("Content-Length:");
			if (cl < 0) cl = request.Find("content-length:");
			if (cl >= 0 && cl < hdr_end) {
				content_length = atoi(request.Mid(cl + 15).Trim());
			}
			int have = request.GetLength() - (hdr_end + 4);
			// Bound the wait so a malformed/hostile Content-Length cannot hang this thread, which
			// serves the whole terminal API.
			int guard = 0;
			while (have < content_length && guard++ < 64) {
				int more = recv(client, buffer, sizeof(buffer) - 1, 0);
				if (more <= 0) break;
				request += CStringA(buffer, more);
				have += more;
			}
		}
	}

	int line_end = request.Find("\r\n");
	CStringA first_line = line_end >= 0 ? request.Left(line_end) : request;
	int first_space = first_line.Find(' ');
	int second_space = first_line.Find(' ', first_space + 1);
	if (first_space < 0 || second_space < 0) {
		CStringA response = Response("bad request\r\n", "400 Bad Request");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	CStringA method = first_line.Left(first_space);
	CStringA target = first_line.Mid(first_space + 1, second_space - first_space - 1);
	int question = target.Find('?');
	CStringA path = question >= 0 ? target.Left(question) : target;
	CStringA query = question >= 0 ? target.Mid(question + 1) : "";

	CStringA text = UrlDecode(QueryValue(query, "text"));
	CStringA section_text = UrlDecode(QueryValue(query, "section"));
	CStringA screen_text = UrlDecode(QueryValue(query, "screen"));
	int body_start = request.Find("\r\n\r\n");
	if (text.IsEmpty() && body_start >= 0) {
		text = request.Mid(body_start + 4);
	}

	if (path.CompareNoCase("/") == 0 || path.CompareNoCase("/table-display") == 0 || path.CompareNoCase("/table-display/") == 0) {
		ServeFile(client, "index.html");
		return;
	}

	if (path.Left(15).CompareNoCase("/table-display/") == 0) {
		CString relative(path.Mid(15));
		if (relative.IsEmpty()) {
			relative = "index.html";
		}
		ServeFile(client, relative);
		return;
	}

	// --- Browser-extended Terminal: page, live state poll, and prompt input -----
	// --- Automation preferences: read / write / connection-test ---------------------------
	// GET  /api/automation-prefs        -> {"ok":true,"prefs":{...}}
	// POST /api/automation-prefs        -> save the six flat fields from the JSON body
	// POST /api/automation-prefs/test   -> live call to the configured API, proving the token works
	if (path.CompareNoCase("/api/automation-prefs/test") == 0) {
		CStringA base, token;
		if (p_tablemap_db != NULL) {
			base  = CStringA(p_tablemap_db->GetSettingString(kAutomationPrefsKey, AutomationPrefField("api_base")));
			token = CStringA(p_tablemap_db->GetSettingString(kAutomationPrefsKey, AutomationPrefField("api_token")));
		}
		CStringA body;
		if (base.IsEmpty() || token.IsEmpty()) {
			body = "{\"ok\":false,\"error\":\"Set the API base URL and token first, then Save.\"}";
		} else {
			CStringA resp;
			// /poll is the right probe: it is side-effect free, it requires the bot token, and a
			// 200 means the server has just stamped bot_seen_at -- i.e. this bot is now live in
			// the console's dropdown. Anything else and we report the status rather than guess.
			DWORD status = AutomationApiGet(base, "/api/v1/automation/poll", token, &resp);
			if (status == 200) {
				CStringA name;
				if (p_tablemap_db != NULL) {
					name = CStringA(p_tablemap_db->GetSettingString(kAutomationPrefsKey, AutomationPrefField("bot_name")));
				}
				body.Format("{\"ok\":true,\"bot\":\"%s\"}", JsonEscapeFree(CString(name)).GetString());
			} else if (status == 401) {
				body = "{\"ok\":false,\"error\":\"The API rejected the token (401). Check the bot token.\"}";
			} else if (status == 0) {
				body = "{\"ok\":false,\"error\":\"Could not reach the API. Check the base URL and this machine's internet access.\"}";
			} else {
				body.Format("{\"ok\":false,\"error\":\"API returned HTTP %lu.\"}", status);
			}
		}
		CStringA response;
		response.Format("HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
			body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	if (path.CompareNoCase("/api/automation-prefs") == 0) {
		CStringA body;
		const int n_fields = sizeof(kAutomationPrefsFields) / sizeof(kAutomationPrefsFields[0]);
		bool is_post = (request.Left(4).CompareNoCase("POST") == 0);
		if (is_post && p_tablemap_db != NULL) {
			// Fields come in as QUERY PARAMETERS, not a JSON body -- the same way every other
			// endpoint on this server takes its input. QueryValue/UrlDecode already exist and are
			// well exercised, so there is no body parsing to get wrong.
			for (int i = 0; i < n_fields; ++i) {
				// QueryValue returns "" for BOTH an absent key and an explicitly empty one, so
				// presence is tested separately: only a parameter the caller actually sent is
				// written. Without this, saving one field would blank the other five -- and
				// clearing the token by omission is exactly the kind of silent damage that is
				// painful to notice.
				if (!AutomationQueryHas(query, kAutomationPrefsFields[i])) {
					continue;
				}
				CStringA v = UrlDecode(QueryValue(query, kAutomationPrefsFields[i]));
				p_tablemap_db->SetSettingString(kAutomationPrefsKey,
					AutomationPrefField(kAutomationPrefsFields[i]), CString(v));
			}
			body = "{\"ok\":true}";
		} else if (p_tablemap_db != NULL) {
			body = "{\"ok\":true,\"prefs\":{";
			for (int i = 0; i < n_fields; ++i) {
				CString v = p_tablemap_db->GetSettingString(kAutomationPrefsKey, AutomationPrefField(kAutomationPrefsFields[i]));
				CStringA piece;
				piece.Format("%s\"%s\":\"%s\"", (i ? "," : ""), kAutomationPrefsFields[i],
					JsonEscapeFree(v).GetString());
				body += piece;
			}
			body += "}}";
		} else {
			body = "{\"ok\":false,\"error\":\"No database connection.\"}";
		}
		CStringA response;
		response.Format("HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
			body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// Automation preferences page (toolbar gear -> CMainFrame::OnAutomation).
	if (path.CompareNoCase("/automation-prefs") == 0 || path.CompareNoCase("/automation-prefs/") == 0) {
		ServeFile(client, "automation-prefs.html");
		return;
	}

	if (path.CompareNoCase("/terminal") == 0 || path.CompareNoCase("/terminal/") == 0) {
		ServeFile(client, "terminal.html");
		return;
	}
	if (path.Left(10).CompareNoCase("/terminal/") == 0) {
		CString relative(path.Mid(10));
		if (relative.IsEmpty()) relative = "terminal.html";
		ServeFile(client, relative);
		return;
	}
	if (path.CompareNoCase("/api/terminal-state") == 0) {
		CString sec[kChatTerminalSectionCount];
		CString pinned;
		TerminalBrowserGetSnapshot(sec, &pinned);
		CStringA body = "{\"sections\":[";
		for (int i = 0; i < kChatTerminalSectionCount; ++i) {
			if (i > 0) body += ",";
			body += "\"";
			body += JsonEscape(sec[i]);
			body += "\"";
		}
		body += "],\"pinned\":\"";
		body += JsonEscape(pinned);
		body += "\"}";
		CStringA response;
		response.Format("HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
			body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	if (path.CompareNoCase("/api/terminal-input") == 0) {
		CStringA cmd = text;   // already URL-decoded above (text/body)
		if (!cmd.IsEmpty()) TerminalBrowserInject(CString(cmd));
		CStringA body = "{\"ok\":true}";
		CStringA response;
		response.Format("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
			"Content-Length: %d\r\nConnection: close\r\n\r\n%s", body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// Mappings verification UI: page + static assets + JSON API.
	if (path.CompareNoCase("/mappings") == 0 || path.CompareNoCase("/mappings/") == 0) {
		ServeFile(client, "mappings.html");
		return;
	}
	if (path.Left(10).CompareNoCase("/mappings/") == 0) {
		CString relative(path.Mid(10));
		if (relative.IsEmpty()) {
			relative = "mappings.html";
		}
		ServeFile(client, relative);
		return;
	}

	if (path.CompareNoCase("/api/mappings") == 0) {
		COCRNameMapping *mapping = (p_pokertracker_thread != NULL) ? p_pokertracker_thread->OCRNameMapping() : NULL;
		std::vector<SOCRNameMappingRow> rows;
		bool ok = false;
		if (mapping != NULL) {
			bool only_unverified = (QueryValue(query, "unverified") == "1");
			ok = mapping->ListMappings(only_unverified, 1000, &rows);
		}
		CStringA body;
		if (!ok) {
			body = "{\"error\":\"could not query mappings (is PT4 connected?)\",\"rows\":[]}";
		} else {
			body = "{\"rows\":[";
			for (size_t i = 0; i < rows.size(); ++i) {
				if (i > 0) body += ",";
				CStringA entry;
				entry.Format("{\"id\":%d,\"actual\":\"%s\",\"ocr\":\"%s\",\"site\":%d,\"verified\":%s,\"confidence\":%.2f,\"updated\":\"%s\"}",
					rows[i].id,
					JsonEscape(rows[i].actual_username).GetString(),
					JsonEscape(rows[i].ocr_detected_name).GetString(),
					rows[i].id_site,
					rows[i].verified ? "true" : "false",
					rows[i].confidence,
					JsonEscape(rows[i].last_updated).GetString());
				body += entry;
			}
			body += "]}";
		}
		CStringA response;
		response.Format(
			"HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
			body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	if (path.CompareNoCase("/api/mappings/verify") == 0
			|| path.CompareNoCase("/api/mappings/unverify") == 0
			|| path.CompareNoCase("/api/mappings/delete") == 0) {
		COCRNameMapping *mapping = (p_pokertracker_thread != NULL) ? p_pokertracker_thread->OCRNameMapping() : NULL;
		int id = atoi(QueryValue(query, "id"));
		bool ok = false;
		if (mapping != NULL && id > 0) {
			if (path.CompareNoCase("/api/mappings/verify") == 0) {
				ok = mapping->SetVerified(id, true);
			} else if (path.CompareNoCase("/api/mappings/unverify") == 0) {
				ok = mapping->SetVerified(id, false);
			} else {
				ok = mapping->DeleteMapping(id);
			}
		}
		CStringA body;
		body.Format("{\"ok\":%s}", ok ? "true" : "false");
		CStringA response;
		response.Format(
			"HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
			body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// Evaluate OpenPPL / engine symbols on demand: /api/symbols?names=prwin,Raises,...
	// Returns {"name": <number-or-"string">, ...}. Used by the MCP server to expose
	// the live internal-engine symbol values to Claude.
	if (path.CompareNoCase("/api/symbols") == 0) {
		CStringA names = UrlDecode(QueryValue(query, "names"));

		// Serve the snapshot the HEARTBEAT publishes. The HTTP thread NEVER evaluates symbols and
		// NEVER touches cs_update_in_progress, so no query -- however large or heavy -- can stall
		// the bot. That was the whole defect: a read of the bot's state could stop the bot.
		WantSymbols(names);                       // tell the heartbeat to keep these fresh
		CStringA snap;
		// On the first-ever request for a name the snapshot has no entry yet. WAIT for the heartbeat
		// to publish it (it runs at ~3.4 Hz, so this is typically one beat, ~300 ms) instead of
		// evaluating here. Evaluating here is what held the update lock -- measured at 10.7 s on the
		// first 110-symbol pull, i.e. a ten-second freeze of the bot, every restart.
		const DWORD deadline = GetTickCount() + 3000;
		for (;;) {
			if (SymbolsFromSnapshot(names, &snap)) break;
			if (GetTickCount() >= deadline) break;                  // give up, fall through below
			Sleep(25);
		}
		if (!snap.IsEmpty()) {
			CStringA fast;
			fast.Format(
				"HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
				"Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
				snap.GetLength(), snap.GetString());
			send(client, fast.GetString(), fast.GetLength(), 0);
			return;
		}
		// Only if the heartbeat never published within the deadline (it is down, or the bot is not
		// connected to a table). Fall back to the old direct evaluation so a symbol read still works
		// when there is no heartbeat to publish for us -- in that case there is nothing to stall.
		CStringA body = "{";
		int start = 0;
		bool first = true;
		// Evaluating a typo'd / unknown symbol must NOT pop Hiss's blocking modal
		// (which would freeze the heartbeat). Suppress it for the duration.
		g_suppress_unknown_symbol_warning = true;
		// EvaluateSymbol() walks the symbol engines + the OHF parse tree -- the SAME state the
		// heartbeat mutates in EvaluateAll(). Running it unsynchronized on this HTTP thread is a
		// data race: it corrupts engine state, and under a heavy symbol (prwin's Monte-Carlo, which
		// nn_driver pulls in its 86-feature infoset) it wedges this thread outright. A wedged HTTP
		// thread is silent-but-fatal: every driver poll then times out, the driver's turn gate reads
		// "not my turn", and the bot folds its arms with the autoplayer disengaged. Serialize against
		// the heartbeat's update cycle -- the same lock CHeartbeatThread::ScrapeEvaluateAct() holds.
		// The send() below stays OUTSIDE the lock: never hold it across network I/O.
		//
		// Take it ONLY while it is actually initialized. This HTTP thread outlives the heartbeat
		// object at both ends, and entering an uninitialized / already-deleted CRITICAL_SECTION
		// corrupts it -- which later blew up the heartbeat inside RtlEnterCriticalSection
		// (crash_hiss_23456). If the heartbeat isn't up, nothing is mutating the engines anyway.
		bool cs_held = (InterlockedCompareExchange(&CHeartbeatThread::cs_update_ready, 1, 1) == 1);
		if (cs_held) {
			EnterCriticalSection(&CHeartbeatThread::cs_update_in_progress);
		}
		while (start <= names.GetLength()) {
			int comma = names.Find(',', start);
			CStringA one = comma >= 0 ? names.Mid(start, comma - start) : names.Mid(start);
			one.Trim();
			if (!one.IsEmpty() && p_engine_container != NULL) {
				CString name(one);
				if (!first) body += ",";
				first = false;
				double dval = 0.0;
				CString sval;
				if (p_engine_container->EvaluateSymbol(name, &dval, false)) {
					CStringA num; num.Format("%.4f", dval);
					body += "\"" + JsonEscape(CString(one)) + "\":" + num;
				} else if (p_engine_container->EvaluateSymbol(name, &sval, false)) {
					body += "\"" + JsonEscape(CString(one)) + "\":\"" + JsonEscape(sval) + "\"";
				} else {
					body += "\"" + JsonEscape(CString(one)) + "\":null";
				}
			}
			if (comma < 0) break;
			start = comma + 1;
		}
		if (cs_held) {
			LeaveCriticalSection(&CHeartbeatThread::cs_update_in_progress);
		}
		g_suppress_unknown_symbol_warning = false;
		body += "}";
		CStringA response;
		response.Format(
			"HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
			body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// Turn the autoplayer on/off:  /api/autoplayer?on=1  or  ?on=0 . With NO ?on= it just REPORTS
	// the current engaged state (a bare GET must never silently disengage -- that was a footgun).
	// List every pickable top-level window, for the toolbar's "connect to window" picker.
	// "served" marks windows another Hiss instance already owns (the autoconnector refuses those,
	// CAutoConnector.cpp:198), and "attached" marks the one THIS instance is on.
	if (path.CompareNoCase("/api/windows") == 0) {
		std::vector<SPickableWindow> wins;
		::EnumWindows(EnumPickableWindows, (LPARAM)&wins);
		HWND mine = (p_autoconnector != NULL) ? p_autoconnector->attached_hwnd() : NULL;
		CStringA body = "{\"ok\":true,\"windows\":[";
		for (size_t i = 0; i < wins.size(); ++i) {
			const bool served = (p_sharedmem != NULL && p_sharedmem->PokerWindowAttached(wins[i].hwnd));
			CStringA item;
			item.Format("%s{\"hwnd\":%lld,\"title\":\"%s\",\"class\":\"%s\",\"w\":%d,\"h\":%d,"
				"\"served\":%s,\"attached\":%s}",
				(i ? "," : ""),
				(long long)(intptr_t)wins[i].hwnd,
				JsonEscapeFree(wins[i].title).GetString(),
				JsonEscapeFree(wins[i].cls).GetString(),
				wins[i].width, wins[i].height,
				served ? "true" : "false",
				(wins[i].hwnd == mine) ? "true" : "false");
			body += item;
		}
		body += "]}";
		CStringA response;
		response.Format("HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
			body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// Manual window override. Bare GET reports state; ?hwnd=<n> pins that window; ?clear=1 releases
	// the pin and restores automatic table selection. The actual connect happens on the heartbeat
	// thread (CHeartbeatThread.cpp), so this only records the request.
	if (path.CompareNoCase("/api/connect-window") == 0) {
		CStringA hwnd_s = QueryValue(query, "hwnd");
		CStringA clear_s = QueryValue(query, "clear");
		const bool want_clear = (clear_s == "1") || (clear_s.CompareNoCase("true") == 0);
		if (want_clear) {
			g_manual_connect_request = 0;
		} else if (!hwnd_s.IsEmpty()) {
			g_manual_connect_hwnd = _atoi64(hwnd_s.GetString());
			g_manual_connect_request = 1;
			g_manual_connect_status = "connecting...";
		}
		HWND pinned = (HWND)(intptr_t)g_manual_connect_hwnd;
		char pinned_title[512] = { 0 };
		if (pinned != NULL && ::IsWindow(pinned)) {
			::GetWindowTextA(pinned, pinned_title, sizeof(pinned_title) - 1);
		}
		HWND mine = (p_autoconnector != NULL) ? p_autoconnector->attached_hwnd() : NULL;
		CStringA body;
		body.Format("{\"ok\":true,\"override\":%s,\"hwnd\":%lld,\"title\":\"%s\","
			"\"attached_hwnd\":%lld,\"status\":\"%s\"}",
			(g_manual_connect_hwnd != 0) ? "true" : "false",
			(long long)g_manual_connect_hwnd,
			JsonEscapeFree(CString(pinned_title)).GetString(),
			(long long)(intptr_t)mine,
			JsonEscapeFree(g_manual_connect_status).GetString());
		CStringA response;
		response.Format("HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
			body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	if (path.CompareNoCase("/api/autoplayer") == 0) {
		CStringA on = QueryValue(query, "on");
		CStringA body;
		if (!on.IsEmpty()) {
			bool want_on = (on == "1") || (on.CompareNoCase("true") == 0) || (on.CompareNoCase("on") == 0);
			g_mcp_autoplayer_request = want_on ? 1 : 0;   // applied by the heartbeat thread
			body.Format("{\"ok\":true,\"request\":\"%s\"}", want_on ? "on" : "off");
		} else {
			bool engaged = (p_autoplayer != NULL && p_autoplayer->autoplayer_engaged());
			body.Format("{\"ok\":true,\"engaged\":%s}", engaged ? "true" : "false");
		}
		CStringA response;
		response.Format("HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
			body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// Engage/disengage the NN driver:  /api/nn-driver?on=1  or  ?on=0  (engaging it disengages
	// the autoplayer and vice-versa). With no ?on= it just reports the current engaged state.
	if (path.CompareNoCase("/api/nn-driver") == 0) {
		CStringA on = QueryValue(query, "on");
		if (!on.IsEmpty()) {
			bool want_on = (on == "1") || (on.CompareNoCase("true") == 0) || (on.CompareNoCase("on") == 0);
			g_mcp_nn_driver_request = want_on ? 1 : 0;   // applied by the heartbeat thread
		}
		CStringA body; body.Format("{\"ok\":true,\"engaged\":%s}", g_nn_driver_engaged ? "true" : "false");
		CStringA response;
		response.Format("HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
			body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// Engage/disengage ULTRA mode:  /api/ultra?on=1  or  ?on=0  (launches/kills ultra_mode.py,
	// which then drives OHF<->NN from the system-audio average). No ?on= reports engaged state.
	if (path.CompareNoCase("/api/ultra") == 0) {
		CStringA on = QueryValue(query, "on");
		if (!on.IsEmpty()) {
			bool want_on = (on == "1") || (on.CompareNoCase("true") == 0) || (on.CompareNoCase("on") == 0);
			g_mcp_ultra_request = want_on ? 1 : 0;   // applied by the heartbeat thread
		}
		CStringA body; body.Format("{\"ok\":true,\"engaged\":%s}", g_ultra_engaged ? "true" : "false");
		CStringA response;
		response.Format("HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
			body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// Engage/disengage SUPERSTITION/OMEN mode:  /api/superstition?on=1 | ?on=0 | ?on=toggle
	// (launches/kills card_oracle.py --superstition --speak, which feeds sb_beastfavor into the OHF).
	// No ?on= reports the engaged state. ?on=toggle flips it (one-button activate + kill switch).
	if (path.CompareNoCase("/api/superstition") == 0) {
		CStringA on = QueryValue(query, "on");
		if (!on.IsEmpty()) {
			if (on.CompareNoCase("toggle") == 0) {
				g_mcp_superstition_request = g_superstition_engaged ? 0 : 1;
			} else {
				bool want_on = (on == "1") || (on.CompareNoCase("true") == 0) || (on.CompareNoCase("on") == 0);
				g_mcp_superstition_request = want_on ? 1 : 0;   // applied by the heartbeat thread
			}
		}
		CStringA body; body.Format("{\"ok\":true,\"engaged\":%s}", g_superstition_engaged ? "true" : "false");
		CStringA response;
		response.Format("HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
			body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// 666 Card Oracle feed:  /api/beast?favor=0.0..1.0 . card_oracle.py pushes the live resonance;
	// the bot exposes it as the sb_beastfavor symbol (superstition mode) + in /api/table-state (the
	// React omen meter). Auto-stales to 0 after ~15s so superstition self-disables if the feed stops.
	if (path.CompareNoCase("/api/beast") == 0) {
		CStringA fav = QueryValue(query, "favor");
		if (!fav.IsEmpty()) {
			double f = atof(fav);
			if (f < 0.0) f = 0.0;
			if (f > 1.0) f = 1.0;
			g_beast_favor = f;
			g_beast_favor_tick = GetTickCount();
		}
		double live = (GetTickCount() - g_beast_favor_tick < 15000) ? g_beast_favor : 0.0;
		CStringA body; body.Format("{\"ok\":true,\"favor\":%.3f}", live);
		CStringA response;
		response.Format("HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
			body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// RED-decision detail feed:  /api/decision-detail?text=<url-encoded, '\n'-separated lines> (or POST body).
	// The Python brain pushes the SAME rich context the React overlay shows (exploit / branch / vs-villain /
	// confidence / mischief / energy) so the scrcpy mirror carries it too. Stales after ~12s. [Emrald: more scrcpy lines]
	if (path.CompareNoCase("/api/decision-detail") == 0) {
		CStringA det = UrlDecode(QueryValue(query, "text"));
		if (det.IsEmpty() && body_start >= 0) det = request.Mid(body_start + 4);  // also accept the raw POST body
		extern char g_hero_decision_detail[256];
		extern DWORD g_hero_decision_detail_tick;
		if (!det.IsEmpty()) {
			strncpy_s(g_hero_decision_detail, sizeof(g_hero_decision_detail), det.GetString(), _TRUNCATE);
			g_hero_decision_detail_tick = GetTickCount();
		} else {
			g_hero_decision_detail[0] = '\0';   // empty text explicitly clears the overlay detail
		}
		CStringA body = "{\"ok\":true}";
		CStringA response;
		response.Format("HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
			body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// Synapse-harmonizer runtime knobs:  /api/knob?name=openrange|aggro|bluff&value=0..1  sets one
	// (clamped 0..1); a BARE /api/knob just reports all three. Default 0.5 = NEUTRAL. The Synapse tab
	// + the manic_burst daemon push here; the OHF reads them as openai_knob_* (no rebuild to retune).
	if (path.CompareNoCase("/api/knob") == 0) {
		extern double g_knob_openrange; extern double g_knob_aggro; extern double g_knob_bluff; extern double g_knob_cbet;
		extern double g_knob_advice_raise; extern double g_knob_advice_value; extern double g_knob_advice_bluff;
		extern double g_knob_advice_fold; extern double g_knob_advice_persona; extern double g_knob_advice_conf;
		extern long g_advice_tick; g_advice_tick = (long)GetTickCount();
		CStringA nm = QueryValue(query, "name");
		CStringA vl = QueryValue(query, "value");
		if (!nm.IsEmpty() && !vl.IsEmpty()) {
			double v = atof(vl);
			if (nm.Left(7).CompareNoCase("advice_") == 0) {
				// Decision-advisor channels: leans clamp -1..1, persona -1..6, conf 0..1.
				if (nm.CompareNoCase("advice_persona") == 0) g_knob_advice_persona = (v < -1.0 ? -1.0 : (v > 6.0 ? 6.0 : v));
				else if (nm.CompareNoCase("advice_conf") == 0) g_knob_advice_conf = (v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
				else {
					double lean = (v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v));
					if (nm.CompareNoCase("advice_raise") == 0) g_knob_advice_raise = lean;
					else if (nm.CompareNoCase("advice_value") == 0) g_knob_advice_value = lean;
					else if (nm.CompareNoCase("advice_bluff") == 0) g_knob_advice_bluff = lean;
					else if (nm.CompareNoCase("advice_fold") == 0) g_knob_advice_fold = lean;
				}
			} else if (nm.CompareNoCase("cbet") == 0) {
				// cbet is special: negative = AUTO (no override / use the computed f$CbetFreq); else clamp 0..1.
				g_knob_cbet = (v < 0.0) ? -1.0 : (v > 1.0 ? 1.0 : v);
			} else if (nm.Left(9).CompareNoCase("obsbranch") == 0) {
				// OBSERVER branch (f$ObsStrategy GOTO selector 0..7) + its aggro/bluff/openrange/affinity knobs.
				extern double g_knob_obsbranch, g_knob_obsbranch_aggro, g_knob_obsbranch_bluff, g_knob_obsbranch_open, g_knob_obsbranch_affin;
				extern long g_obsbranch_tick;
				if (nm.CompareNoCase("obsbranch") == 0) g_knob_obsbranch = (v < 0.0 ? 0.0 : (v > 7.0 ? 7.0 : v));
				else if (nm.CompareNoCase("obsbranch_affinity") == 0) g_knob_obsbranch_affin = (v < 0.0 ? 0.0 : (v > 3.0 ? 3.0 : v));
				else { double c = (v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
					if (nm.CompareNoCase("obsbranch_aggro") == 0) g_knob_obsbranch_aggro = c;
					else if (nm.CompareNoCase("obsbranch_bluff") == 0) g_knob_obsbranch_bluff = c;
					else if (nm.CompareNoCase("obsbranch_openrange") == 0) g_knob_obsbranch_open = c; }
				g_obsbranch_tick = (long)GetTickCount();
			} else if (nm.Left(12).CompareNoCase("brain_action") == 0) {
				// BRAIN-ACTION soft pre-empt: kind 0..4, conf 0..1, size in bb.
				extern double g_knob_brain_action_kind, g_knob_brain_action_conf, g_knob_brain_action_size;
				extern long g_brain_action_tick;
				if (nm.CompareNoCase("brain_action_kind") == 0) g_knob_brain_action_kind = (v < 0.0 ? 0.0 : (v > 4.0 ? 4.0 : v));
				else if (nm.CompareNoCase("brain_action_conf") == 0) g_knob_brain_action_conf = (v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
				else if (nm.CompareNoCase("brain_action_size_bb") == 0) g_knob_brain_action_size = (v < 0.0 ? 0.0 : v);
				g_brain_action_tick = (long)GetTickCount();
			} else if (nm.Left(8).CompareNoCase("mischief") == 0) {
				// MISCHIEF odd-bet channel: betpct (pot fraction 0..3) + fire flag.
				extern double g_knob_mischief_betpct, g_knob_mischief_fire;
				extern long g_mischief_tick;
				if (nm.CompareNoCase("mischief_betpct") == 0) g_knob_mischief_betpct = (v < 0.0 ? 0.0 : (v > 3.0 ? 3.0 : v));
				else if (nm.CompareNoCase("mischief_fire") == 0) g_knob_mischief_fire = (v != 0.0 ? 1.0 : 0.0);
				g_mischief_tick = (long)GetTickCount();
			} else {
				if (v < 0.0) v = 0.0;
				if (v > 1.0) v = 1.0;
				if (nm.CompareNoCase("openrange") == 0) g_knob_openrange = v;
				else if (nm.CompareNoCase("aggro") == 0) g_knob_aggro = v;
				else if (nm.CompareNoCase("bluff") == 0) g_knob_bluff = v;
			}
		}
		CStringA body; body.Format("{\"ok\":true,\"openrange\":%.3f,\"aggro\":%.3f,\"bluff\":%.3f,\"cbet\":%.3f,"
			"\"advice_raise\":%.3f,\"advice_value\":%.3f,\"advice_bluff\":%.3f,\"advice_fold\":%.3f,"
			"\"advice_persona\":%.1f,\"advice_conf\":%.3f}",
			g_knob_openrange, g_knob_aggro, g_knob_bluff, g_knob_cbet,
			g_knob_advice_raise, g_knob_advice_value, g_knob_advice_bluff, g_knob_advice_fold,
			g_knob_advice_persona, g_knob_advice_conf);
		CStringA response;
		response.Format("HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
			body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// Manually act on the table (only fires on an explicit request, e.g. learner.exe):
	//   /api/action?do=fold|check|call|raise|allin            -> click that button
	//   /api/action?do=bet|raise&amount=<bb>                  -> sized bet/raise via the
	//        autoplayer's two-successive-clicks + numpad path (amount in big blinds)
	if (path.CompareNoCase("/api/action") == 0) {
		CStringA d = QueryValue(query, "do"); d.MakeLower();
		CStringA amt = QueryValue(query, "amount");
		double amount = amt.IsEmpty() ? -1.0 : atof(amt.GetString());
		int code = -1;
		if (d == "fold")  code = k_autoplayer_function_fold;
		else if (d == "check") code = k_autoplayer_function_check;
		else if (d == "call")  code = k_autoplayer_function_call;
		else if (d == "raise" || d == "bet") code = k_autoplayer_function_raise;
		else if (d == "allin" || d == "all-in") code = k_autoplayer_function_allin;
		CStringA body;
		if (code < 0) {
			body = "{\"ok\":false,\"error\":\"do must be fold|check|call|bet|raise|allin\"}";
		} else {
			g_mcp_action_amount = ((d == "raise" || d == "bet") ? amount : -1.0);
			g_mcp_action_set_tick = GetTickCount();   // for wait-for-turn expiry
			// The SPOT this action was decided for. A forced request bypasses the ismyturn gate and
			// stays pending up to 25 s, so without this stamp a decision made for hand N could still
			// be queued when hand N+1 deals -- and fire there, into a different spot, the moment a
			// matching button appeared. Callers that know their spot (the NN driver) send it; a human
			// clicking in the learner sends nothing and stays unchecked, exactly as before.
			CString hand_stamp = CString(UrlDecode(QueryValue(query, "hand")));
			CStringA br_a = QueryValue(query, "betround");
			g_mcp_action_hand = hand_stamp;
			g_mcp_action_betround = br_a.IsEmpty() ? -1 : atoi(br_a.GetString());
			// force=1 (manual learner click) bypasses the ismyturn gate: fire as soon as
			// the button is clickable, even if the buttons-visible threshold isn't met.
			g_mcp_action_force = (QueryValue(query, "force") == "1");
			g_mcp_action_request = code;   // executed by the heartbeat thread
			body.Format("{\"ok\":true,\"action\":\"%s\",\"amount\":%.2f}", d.GetString(), amount);
		}
		CStringA response;
		response.Format("HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
			body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// Trigger a one-shot dump of all region scrapes + results to logs\scrapes\
	// (the MCP server calls this, then reads the files).
	if (path.CompareNoCase("/api/dump-scrapes") == 0) {
		g_dump_scrapes_once = true;
		CStringA body = "{\"ok\":true,\"dir\":\"logs/scrapes\"}";
		CStringA response;
		response.Format(
			"HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
			body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	if (path.CompareNoCase("/api/table-game-info") == 0) {
		// Claude-parsed game info from the table image. Query params (all optional):
		//   sb, bb, ante, chips_per_bb, level, players  -> stored as table_game_info,
		// and the blinds drive the blind guesser (authoritative).
		CStringA sb = QueryValue(query, "sb");
		CStringA bb = QueryValue(query, "bb");
		CStringA an = QueryValue(query, "ante");
		CStringA cpb = QueryValue(query, "chips_per_bb");
		CStringA lvl = QueryValue(query, "level");
		CStringA pl = QueryValue(query, "players");
		if (!bb.IsEmpty()) g_tgi_bblind = atof(bb.GetString());
		if (!sb.IsEmpty()) g_tgi_sblind = atof(sb.GetString());
		if (!an.IsEmpty()) g_tgi_ante = atof(an.GetString());
		if (!cpb.IsEmpty()) g_tgi_chips_per_bb = atof(cpb.GetString());
		if (!lvl.IsEmpty()) g_tgi_level = atof(lvl.GetString());
		if (!pl.IsEmpty()) g_tgi_players_remaining = atof(pl.GetString());
		// String fields (URL-decoded): tourney name/id/table/gametype for the HH header.
		CStringA tn = UrlDecode(QueryValue(query, "tourney_name"));
		CStringA ti = UrlDecode(QueryValue(query, "tourney_id"));
		CStringA tbl = UrlDecode(QueryValue(query, "table_number"));
		CStringA gt = UrlDecode(QueryValue(query, "gametype"));
		if (!tn.IsEmpty()) g_tgi_tourney_name = CString(tn);
		if (!ti.IsEmpty()) g_tgi_tourney_id = CString(ti);
		if (!tbl.IsEmpty()) g_tgi_table_number = CString(tbl);
		if (!gt.IsEmpty()) g_tgi_gametype = CString(gt);
		// If sb wasn't given but bb was, default sb to half the big blind.
		if (g_tgi_bblind > 0 && g_tgi_sblind <= 0) g_tgi_sblind = g_tgi_bblind / 2.0;
		g_tgi_set = (g_tgi_bblind > 0);
		CStringA body;
		body.Format("{\"ok\":true,\"table_game_info\":{\"sblind\":%.4f,\"bblind\":%.4f,\"ante\":%.4f,"
			"\"chips_per_bb\":%.2f,\"level\":%.0f,\"players_remaining\":%.0f,\"set\":%s}}",
			g_tgi_sblind, g_tgi_bblind, g_tgi_ante, g_tgi_chips_per_bb, g_tgi_level,
			g_tgi_players_remaining, g_tgi_set ? "true" : "false");
		CStringA response = Response(body + "\r\n");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	if (path.CompareNoCase("/api/reset-detection") == 0) {
		// React badge BACKUP: clear the per-table game-type cache + cached identity + the game-type flag so
		// the engine re-detects the CURRENT table's game type from a clean slate (hole-card count) -- for
		// when the auto per-table detection has latched the wrong type. [Emrald]
		//
		// CLEAR GAME STATE deliberately does NOT touch the manual override. An override is a human
		// stating what this table IS, and it holds for the whole Hiss session until they clear it
		// themselves [Emrald]. Wiping it here would mean the one button you press when the detector
		// has gone wrong also throws away the correction you made -- and the detector would just
		// re-latch the same wrong type. Use CLEAR OVERRIDE (/api/gametype?set=auto) to hand control
		// back to the detector.
		g_reset_detection_request = true;
		CStringA response = Response("{\"ok\":true,\"reset\":true}\r\n");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// MANUAL GAME-TYPE OVERRIDE (the React badge menu).
	//   GET /api/gametype              -> report the override + what the table is currently playing as
	//   GET /api/gametype?set=nlh|plo|plo8|auto
	// Selecting a type forces the symbols (isomaha/isplo8 -> the strategy tree's dispatch) AND the
	// tablemap (SwitchTablemapForGameTypeIfNeeded keys off g_table_is_omaha, which the override sets),
	// so the bot plays the right tree on the right map instead of whatever the title OCR guessed.
	if (path.CompareNoCase("/api/gametype") == 0) {
		CStringA set = QueryValue(query, "set");
		set.MakeLower();
		if (!set.IsEmpty()) {
			if      (set == "nlh"  || set == "holdem") g_gametype_override = kGametypeOverrideNLH;
			else if (set == "plo"  || set == "omaha")  g_gametype_override = kGametypeOverridePLO;
			else if (set == "plo8" || set == "hilo")   g_gametype_override = kGametypeOverridePLO8;
			else if (set == "auto" || set == "clear")  g_gametype_override = kGametypeOverrideAuto;
			// Force the flag the tablemap switch keys off IMMEDIATELY, so the map swaps on the next
			// heartbeat instead of waiting for the detector to run again on the next hand.
			if (g_gametype_override != kGametypeOverrideAuto) {
				g_table_is_omaha = GametypeOverrideSaysOmaha();
			}
			write_log(k_always_log_basic_information,
				"[Gametype] MANUAL OVERRIDE -> %s (omaha=%d). The detector is now ignored for this table "
				"until it is cleared; the tablemap and the strategy tree both follow this.\n",
				set.GetString(), (int)g_table_is_omaha);
		}
		const char *ov = (g_gametype_override == kGametypeOverrideNLH)  ? "nlh"
		               : (g_gametype_override == kGametypeOverridePLO)  ? "plo"
		               : (g_gametype_override == kGametypeOverridePLO8) ? "plo8" : "auto";
		bool omaha = false, plo8 = false;
		if (p_engine_container != NULL && p_engine_container->symbol_engine_isomaha() != NULL) {
			omaha = p_engine_container->symbol_engine_isomaha()->isomaha();
			plo8  = p_engine_container->symbol_engine_isomaha()->isplo8();
		}
		const char *eff = plo8 ? "plo8" : (omaha ? "plo" : "nlh");
		CString map = (p_tablemap != NULL) ? p_tablemap->filename() : CString("");
		CStringA body;
		body.Format("{\"ok\":true,\"override\":\"%s\",\"effective\":\"%s\",\"tablemap\":\"%s\"}\r\n",
			ov, eff, CStringA(map).GetString());
		CStringA response = Response(body);
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// HUD overlay recalibration. The user right-clicks "Recalibrate all HUDs (Claude)"
	// which sets g_hud_calibrate_request; Claude/MCP polls the status, reads the table
	// screenshot, and POSTs per-seat anchor fractions back via /api/hud-positions.
	if (path.CompareNoCase("/api/hud-calibrate") == 0) {
		g_dump_scrapes_once = true;        // refresh logs/scrapes/_table.bmp for Claude to read
		g_hud_calibrate_request = true;
		CStringA response = Response("{\"ok\":true,\"pending\":true}\r\n");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	if (path.CompareNoCase("/api/hud-calibrate-status") == 0) {
		CStringA body;
		body.Format("{\"pending\":%s}\r\n", g_hud_calibrate_request ? "true" : "false");
		CStringA response = Response(body);
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}
	if (path.CompareNoCase("/api/hud-positions") == 0) {
		// json = per-seat anchor fractions, e.g. {"c0":{"x":0.12,"y":0.30},...,"locked":0}
		CStringA j = UrlDecode(QueryValue(query, "json"));
		if (!j.IsEmpty()) {
			g_hud_positions_json = CString(j);
			g_hud_positions_apply = true;   // heartbeat hands it to the overlay (off this thread)
			g_hud_calibrate_request = false;
		}
		CStringA body;
		body.Format("{\"ok\":%s}\r\n", j.IsEmpty() ? "false" : "true");
		CStringA response = Response(body);
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	if (path.CompareNoCase("/api/click-region") == 0) {
		// Click an arbitrary tablemap region by name (lobby navigation buttons, etc.).
		// The heartbeat thread performs the click (mouse DLL) on its next cycle.
		CStringA rn = UrlDecode(QueryValue(query, "name"));
		if (!rn.IsEmpty()) g_mcp_click_region = CString(rn);
		CStringA body;
		body.Format("{\"ok\":%s,\"click_region\":\"%s\",\"note\":\"clicked on next heartbeat\"}",
			rn.IsEmpty() ? "false" : "true", JsonEscape(CString(rn)).GetString());
		CStringA response = Response(body + "\r\n");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	if (path.CompareNoCase("/api/table-game-info-2") == 0) {
		// Current + previous hand numbers parsed by Claude from the table image.
		CStringA ch = QueryValue(query, "curr_hand");
		CStringA ph = QueryValue(query, "prev_hand");
		if (!ch.IsEmpty()) g_tgi2_handnumber = atof(ch.GetString());
		if (!ph.IsEmpty()) g_tgi2_prev_handnumber = atof(ph.GetString());
		CStringA body;
		body.Format("{\"ok\":true,\"table_game_info_2\":{\"handnumber\":%.0f,\"prev_handnumber\":%.0f}}",
			g_tgi2_handnumber, g_tgi2_prev_handnumber);
		CStringA response = Response(body + "\r\n");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	if (path.CompareNoCase("/api/set-region-value") == 0) {
		// Claude/MCP transform: Claude parsed a region from the image and posts its value
		// here; the scraper returns it for that region instead of OCR. name + value params.
		CStringA rn = UrlDecode(QueryValue(query, "name"));
		CStringA rv = UrlDecode(QueryValue(query, "value"));
		if (!rn.IsEmpty() && p_scraper != NULL) {
			p_scraper->SetClaudeRegionValue(CString(rn), CString(rv));
		}
		CStringA body;
		body.Format("{\"ok\":%s,\"region\":\"%s\",\"value\":\"%s\"}",
			(!rn.IsEmpty() && p_scraper != NULL) ? "true" : "false",
			JsonEscape(CString(rn)).GetString(), JsonEscape(CString(rv)).GetString());
		CStringA response = Response(body + "\r\n");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	if (path.CompareNoCase("/api/reload-ohf") == 0) {
		// Request a strategy reload; the heartbeat thread re-parses bot_logic/Strategy
		// (+ the master OHF) between evaluations, so edits take effect without a restart.
		g_mcp_reload_ohf_request = true;
		CStringA response = Response("{\"ok\":true,\"queued\":\"OHF reload requested; applied on the next heartbeat.\"}\r\n");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	if (path.CompareNoCase("/api/validate") == 0) {
		// Scrape + game-state sanity heuristics (CSymbolEngineValidator). Re-runs the
		// checks against the current state and returns the verdict + a message report.
		CStringA body = "{\"available\":false}";
		if (p_engine_container != NULL && p_symbol_engine_validator != NULL) {
			p_symbol_engine_validator->Validate();
			CString report = p_symbol_engine_validator->Report();
			body.Format(
				"{\"available\":true,\"ok\":%s,\"confidence\":%.3f,\"nerrors\":%d,\"nwarnings\":%d,"
				"\"cards_ok\":%s,\"pot_ok\":%s,\"stacks_ok\":%s,\"bets_ok\":%s,\"report\":\"%s\"}",
				p_symbol_engine_validator->Ok() ? "true" : "false",
				p_symbol_engine_validator->Confidence(),
				p_symbol_engine_validator->Nerrors(),
				p_symbol_engine_validator->Nwarnings(),
				p_symbol_engine_validator->CardsOk() ? "true" : "false",
				p_symbol_engine_validator->PotOk() ? "true" : "false",
				p_symbol_engine_validator->StacksOk() ? "true" : "false",
				p_symbol_engine_validator->BetsOk() ? "true" : "false",
				JsonEscape(report).GetString());
		}
		CStringA response;
		response.Format(
			"HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
			body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// Automation on/off for THIS instance, for the table-view tile.
	//   GET                -> {"ok":true,"enabled":bool}
	//   GET|POST ?on=1|0   -> sets it, returns the new value
	// This is deliberately the SAME per-port "enabled" field the /automation-prefs page and the
	// tournament-join poller already use (automation_api.enabled_<port>), so the tile is the one
	// switch for this instance's automation rather than a second, silently divergent flag.
	if (path.CompareNoCase("/api/automation-enabled") == 0) {
		CStringA body;
		if (p_tablemap_db == NULL) {
			body = "{\"ok\":false,\"error\":\"No database connection.\"}";
		} else {
			if (AutomationQueryHas(query, "on")) {
				CStringA v = UrlDecode(QueryValue(query, "on"));
				bool want_on = (v == "1" || v.CompareNoCase("on") == 0 || v.CompareNoCase("true") == 0);
				p_tablemap_db->SetSettingString(kAutomationPrefsKey, AutomationPrefField("enabled"),
					CString(want_on ? "on" : "off"));
				write_log(k_always_log_basic_information,
					"[Automation] enabled -> %s (via /api/automation-enabled)\n", want_on ? "on" : "off");
			}
			CString cur = p_tablemap_db->GetSettingString(kAutomationPrefsKey, AutomationPrefField("enabled"));
			bool on = (cur.CompareNoCase("on") == 0 || cur == "1" || cur.CompareNoCase("true") == 0);
			body.Format("{\"ok\":true,\"enabled\":%s}", on ? "true" : "false");
		}
		CStringA response;
		response.Format("HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
			"Content-Length: %d\r\nConnection: close\r\n\r\n%s",
			body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	// Seat status: is the hero genuinely SEATED at a table, OBSERVING one, or NOT AT A TABLE?
	// The verdict and its evidence are computed once per heartbeat (CHeartbeatThread::UpdateSeatStatus)
	// and only READ here -- this HTTP thread must never evaluate symbols, which is the race that used
	// to crash /api/symbols. "stable_ms" is how long the current verdict has held; the join-a-game
	// automation restarts the poker app on a negative, so it waits for a sustained one rather than
	// acting on a single scrape.
	if (path.CompareNoCase("/api/seat-status") == 0) {
		long state = g_seat_state;
		long ev = g_seat_evidence;
		long since = g_seat_since_tick;
		long stable_ms = (long)(GetTickCount() - (DWORD)since);
		if (stable_ms < 0) stable_ms = 0;
		const char *name = (state == kSeatSeated) ? "seated"
		                 : (state == kSeatObserving) ? "observing" : "not_at_table";
		CStringA body;
		body.Format("{\"ok\":true,\"state\":\"%s\",\"at_table\":%s,\"stable_ms\":%ld,"
			"\"table\":\"%s\",\"evidence\":{"
			"\"identity\":%s,\"blinds\":%s,\"seats\":%s,\"hero_chair\":%s,\"hero_named\":%s,"
			"\"hero_stack\":%s,\"hero_cards\":%s,\"hand\":%s,\"buttons\":%s,\"observer\":%s},"
			"\"evidence_bits\":%ld}",
			name,
			(state != kSeatNotAtTable) ? "true" : "false",
			stable_ms,
			JsonEscape(g_table_identity).GetString(),
			(ev & kSeatEvIdentity)  ? "true" : "false",
			(ev & kSeatEvBlinds)    ? "true" : "false",
			(ev & kSeatEvSeats)     ? "true" : "false",
			(ev & kSeatEvHeroChair) ? "true" : "false",
			(ev & kSeatEvHeroNamed) ? "true" : "false",
			(ev & kSeatEvHeroStack) ? "true" : "false",
			(ev & kSeatEvHeroCards) ? "true" : "false",
			(ev & kSeatEvHand)      ? "true" : "false",
			(ev & kSeatEvButtons)   ? "true" : "false",
			(ev & kSeatEvObserver)  ? "true" : "false",
			ev);
		CStringA response;
		response.Format("HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
			"Content-Length: %d\r\nConnection: close\r\n\r\n%s",
			body.GetLength(), body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	if (path.CompareNoCase("/api/table-state") == 0) {
		CStringA body = BuildTableStateJson();
		CStringA response;
		response.Format(
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: application/json; charset=utf-8\r\n"
			"Cache-Control: no-store\r\n"
			"Content-Length: %d\r\n"
			"Connection: close\r\n"
			"\r\n"
			"%s",
			body.GetLength(),
			body.GetString());
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	if (path.CompareNoCase("/clear") == 0) {
		if (screen_text.IsEmpty()) {
			ChatTerminalClear();
		}
		else {
			ChatTerminalClearScreen(CString(screen_text));
		}
		CStringA response = Response("cleared\r\n");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	if (path.CompareNoCase("/append") == 0 || path.CompareNoCase("/stream") == 0) {
		CString wide_text(text);
		CString screen(screen_text);
		int section = SectionFromText(section_text);
		if (path.CompareNoCase("/stream") == 0) {
			ChatTerminalStreamToScreen(screen, section, wide_text);
		}
		else {
			ChatTerminalAppendToScreen(screen, section, wide_text);
		}
		CStringA response = Response("ok\r\n");
		send(client, response.GetString(), response.GetLength(), 0);
		return;
	}

	CStringA response = Response("not found\r\n", "404 Not Found");
	send(client, response.GetString(), response.GetLength(), 0);
}

CStringA CChatTerminalServer::Response(CStringA body, CStringA status)
{
	CStringA response;
	response.Format(
		"HTTP/1.1 %s\r\n"
		"Content-Type: text/plain; charset=utf-8\r\n"
		"Content-Length: %d\r\n"
		"Connection: close\r\n"
		"\r\n"
		"%s",
		status.GetString(),
		body.GetLength(),
		body.GetString());
	return response;
}

CStringA CChatTerminalServer::BinaryResponse(CByteArray &body, CStringA content_type, CStringA status)
{
	CStringA response;
	response.Format(
		"HTTP/1.1 %s\r\n"
		"Content-Type: %s\r\n"
		"Cache-Control: no-cache, no-store, must-revalidate\r\n"
		"Content-Length: %d\r\n"
		"Connection: close\r\n"
		"\r\n",
		status.GetString(),
		content_type.GetString(),
		body.GetSize());
	return response;
}

bool CChatTerminalServer::ServeFile(SOCKET client, CString relative_path)
{
	relative_path.Replace("/", "\\");
	if (relative_path.Find("..") >= 0) {
		CStringA response = Response("bad path\r\n", "400 Bad Request");
		send(client, response.GetString(), response.GetLength(), 0);
		return false;
	}

	CString path;
	path.Format("laravel-react-table-display\\public\\%s", relative_path.GetString());
	if (GetFileAttributes(path) == INVALID_FILE_ATTRIBUTES) {
		TCHAR module_path[MAX_PATH] = { 0 };
		GetModuleFileName(NULL, module_path, MAX_PATH);
		CString module_dir(module_path);
		int slash = module_dir.ReverseFind('\\');
		if (slash >= 0) {
			module_dir = module_dir.Left(slash);
			CString candidate;
			candidate.Format("%s\\laravel-react-table-display\\public\\%s", module_dir.GetString(), relative_path.GetString());
			if (GetFileAttributes(candidate) != INVALID_FILE_ATTRIBUTES) {
				path = candidate;
			} else {
				candidate.Format("%s\\..\\laravel-react-table-display\\public\\%s", module_dir.GetString(), relative_path.GetString());
				if (GetFileAttributes(candidate) != INVALID_FILE_ATTRIBUTES) {
					path = candidate;
				}
			}
		}
		if (GetFileAttributes(path) == INVALID_FILE_ATTRIBUTES) {
			CStringA response = Response("not found\r\n", "404 Not Found");
			send(client, response.GetString(), response.GetLength(), 0);
			return false;
		}
	}

	CFile file;
	if (!file.Open(path, CFile::modeRead | CFile::typeBinary)) {
		CStringA response = Response("cannot read file\r\n", "500 Internal Server Error");
		send(client, response.GetString(), response.GetLength(), 0);
		return false;
	}

	CByteArray body;
	ULONGLONG length = file.GetLength();
	body.SetSize((INT_PTR)length);
	if (length > 0) {
		file.Read(body.GetData(), (UINT)length);
	}
	file.Close();

	CStringA header = BinaryResponse(body, ContentType(path));
	send(client, header.GetString(), header.GetLength(), 0);
	if (body.GetSize() > 0) {
		send(client, (const char *)body.GetData(), (int)body.GetSize(), 0);
	}
	return true;
}

CStringA CChatTerminalServer::ContentType(CString path)
{
	path.MakeLower();
	if (path.Right(5) == ".html") return "text/html; charset=utf-8";
	if (path.Right(3) == ".js") return "application/javascript; charset=utf-8";
	if (path.Right(4) == ".css") return "text/css; charset=utf-8";
	if (path.Right(5) == ".json") return "application/json; charset=utf-8";
	if (path.Right(4) == ".svg") return "image/svg+xml";
	if (path.Right(4) == ".png") return "image/png";
	return "application/octet-stream";
}

CStringA CChatTerminalServer::JsonEscape(CString value)
{
	CStringA input(value);
	CStringA escaped;
	for (int i = 0; i < input.GetLength(); ++i) {
		unsigned char c = (unsigned char)input[i];
		switch (c) {
		case '\\': escaped += "\\\\"; break;
		case '"': escaped += "\\\""; break;
		case '\r': /* drop, JSON shouldn't carry CR */ break;
		case '\n': escaped += "\\n"; break;
		case '\t': escaped += "\\t"; break;
		case '\b': escaped += "\\b"; break;
		case '\f': escaped += "\\f"; break;
		default:
			if (c < 0x20) {
				// Any other control byte (e.g. OCR noise) — emit \u00XX.
				char buf[8];
				sprintf_s(buf, sizeof(buf), "\\u%04x", c);
				escaped += buf;
			} else {
				escaped += (char)c;
			}
			break;
		}
	}
	return escaped;
}

// Encode a card for the table-state JSON. Face-down cards become "BACK" (so the UI
// can draw a card back) and empty slots become "" — ToString() alone returns garbage
// rank text (e.g. "9", "T") for the special CARD_BACK value.
static CString CardToken(Card *c)
{
	if (c == NULL) return "";
	if (c->IsCardBack()) return "BACK";
	if (c->IsNoCard()) return "";
	return c->ToString();
}

CStringA CChatTerminalServer::BuildTableStateJson(void)
{
	int nchairs = p_tablemap == NULL ? 10 : p_tablemap->nchairs();
	CStringA json;
	CString handnumber = p_handreset_detector == NULL ? "" : p_handreset_detector->GetHandNumber();
	double sblind = p_engine_container == NULL ? 0 : p_engine_container->symbol_engine_tablelimits()->sblind();
	double bblind = p_engine_container == NULL ? 0 : p_engine_container->symbol_engine_tablelimits()->bblind();
	double ante = p_engine_container == NULL ? 0 : p_engine_container->symbol_engine_tablelimits()->ante();
	double pot = p_engine_container == NULL ? 0 : p_engine_container->symbol_engine_chip_amounts()->pot();
	int gametype = p_engine_container == NULL ? 0 : p_engine_container->symbol_engine_gametype()->gametype();
	bool is_omaha = p_engine_container != NULL && p_engine_container->symbol_engine_isomaha()->isomaha();
	// The hero's TURN signal, served from the heartbeat's cached bits. Drivers (nn_driver /
	// ultra_mode) used to gate on /api/symbols?names=ismyturn, which EVALUATES on this HTTP
	// thread -- the very race this JSON was rewritten to avoid. When that endpoint wedged, the
	// driver's gate silently read "not my turn" and the bot sat out every hand with the
	// autoplayer disengaged (hand 2776921615: AT on a CHECK/RAISE spot, no action at all).
	// These are all cheap CACHED accessors (bit-test / bool / enum compare) -- no evaluation.
	bool ismyturn = p_engine_container != NULL && p_engine_container->symbol_engine_autoplayer()->ismyturn();
	bool is_plo8 = p_engine_container != NULL && p_engine_container->symbol_engine_isomaha()->isplo8();
	bool is_pl = p_engine_container != NULL && p_engine_container->symbol_engine_gametype()->ispl();
	// DO NOT refresh HUD/PT4 stats here: this runs on the HTTP server thread, and
	// PT_DLL_GetStat evaluates PT4 query symbols through the (non-thread-safe) symbol
	// engine. Doing that off the heartbeat/UI thread races the engine and crashes
	// (CFunction::Evaluate throws -> SEH). The UI display timer refreshes the cache under
	// the heartbeat update lock; here we only READ the cached SamplesForChair/StatsForChair
	// (both mutex-guarded copies), so the JSON is safe + slightly-throttled-stale.

	// Observer mode: when "p3observer" is true, p3's scraped values come from the
	// p3observer_ regions and p3 should render as a normal seat (not the hero).
	bool observer = (p_scraper != NULL) && p_scraper->ObserverActive();
	// The hero's chair (userchair) so the display can seat the real player at the
	// bottom instead of assuming a fixed chair. -1 when unknown.
	int userchair = -1;
	if (p_engine_container != NULL
		&& p_engine_container->symbol_engine_userchair()->userchair_confirmed()) {
		userchair = p_engine_container->symbol_engine_userchair()->userchair();
	}
	// The chair whose turn it is (to act), so the display highlights exactly one
	// seat instead of every player still in the hand. -1 when unknown (the display
	// then falls back to the per-player "active" flag). Server-scrape only for now.
	int toact = -1;
	if (p_scarlet_beast != NULL && p_scarlet_beast->ScrapeFromServer()) {
		long ta = p_scarlet_beast->ServerToAct();  // 1-based server seat
		if (ta > 0) toact = (int)ta - 1;
	}
	double beastfavor_live = (GetTickCount() - g_beast_favor_tick < 15000) ? g_beast_favor : 0.0;
	// Snapshot the button indicators with ONE 8-byte load each (they are published by the heartbeat
	// with one 8-byte store). Formatting straight from the globals let "%s" read them byte-by-byte
	// while the heartbeat was rewriting them -- which crashed the process. Both are always
	// NUL-terminated within their 8 bytes, so these copies are safe to print.
	char fckra_snapshot[9] = {0};
	char tiolp_snapshot[9] = {0};
	*(__int64 *)fckra_snapshot = *(volatile __int64 *)g_fckra_indicator;
	*(__int64 *)tiolp_snapshot = *(volatile __int64 *)g_tiolp_indicator;
	fckra_snapshot[8] = '\0';
	tiolp_snapshot[8] = '\0';
	json.Format("{\"nchairs\":%d,\"userchair\":%d,\"toact\":%d,\"handnumber\":\"%s\",\"ismyturn\":%s,\"isomaha\":%s,\"isplo8\":%s,\"ispl\":%s,\"observer\":%s,\"table\":\"%s\",\"limits\":{\"sblind\":%.2f,\"bblind\":%.2f,\"ante\":%.2f,\"gametype\":%d},\"pot\":%.2f,\"beastfavor\":%.3f,\"fckra\":\"%s\",\"tiolp\":\"%s\",",
		nchairs, userchair, toact, JsonEscape(handnumber).GetString(),
		ismyturn ? "true" : "false", is_omaha ? "true" : "false",
		is_plo8 ? "true" : "false", is_pl ? "true" : "false",
		observer ? "true" : "false", JsonEscape(g_table_identity).GetString(), sblind, bblind, ante, gametype, pot, beastfavor_live,
		fckra_snapshot, tiolp_snapshot);
	json += "\"commonCards\":[";
	for (int i = 0; i < kNumberOfCommunityCards; ++i) {
		if (i > 0) json += ",";
		CString card = p_table_state == NULL ? "" : CardToken(p_table_state->CommonCards(i));
		json.AppendFormat("\"%s\"", JsonEscape(card).GetString());
	}
	json += "],\"players\":[";
	for (int chair = 0; chair < nchairs; ++chair) {
		if (chair > 0) json += ",";
		CPlayer *player = p_table_state == NULL ? NULL : p_table_state->Player(chair);
		CString name = player == NULL ? "" : player->name();

		// PokerTracker name-match state for this seat.
		//   matched  = scraped name fuzzy-matched to a PT4 player (name shown bold + green)
		//   verified = mapping confirmed in ocr_name_mappings (stats + sample size shown)
		bool name_matched = false;
		bool name_verified = false;
		CString pt_name = "";
		int pt_samples = -1;  // -1 => unknown / not displayed
		bool seated_player = player != NULL && player->seated();
		if (seated_player && chair >= kFirstChair && chair <= kLastChair) {
			// found/verified and the cached sample size are refreshed (throttled)
			// by p_hud_manager->RefreshIfNeeded() above.
			name_matched = _player_data[chair].found;
			name_verified = _player_data[chair].verified;
			pt_name = _player_data[chair].pt_name;
			// The hero's own name needs no OCR name-verification (the bot IS the hero), so
			// always surface the hero's own HUD stats + sample size.
			if (userchair >= 0 && chair == userchair) { name_verified = true; if (pt_name.IsEmpty()) pt_name = name; }
			if (p_hud_manager != NULL) {   // [Emrald] no name_verified gate: surface samples for any chair, like scrcpy
				pt_samples = p_hud_manager->SamplesForChair(chair);
			}
		}

		json.AppendFormat("{\"chair\":%d,\"name\":\"%s\",\"seated\":%s,\"active\":%s,\"dealer\":%s,\"balance\":%.2f,\"bet\":%.2f,\"matched\":%s,\"verified\":%s,\"ptname\":\"%s\",\"samples\":%d,\"cards\":[",
			chair,
			JsonEscape(name).GetString(),
			player != NULL && player->seated() ? "true" : "false",
			player != NULL && player->active() ? "true" : "false",
			player != NULL && player->dealer() ? "true" : "false",
			player == NULL ? 0 : player->_balance.GetValue(),
			player == NULL ? 0 : player->_bet.GetValue(),
			name_matched ? "true" : "false",
			name_verified ? "true" : "false",
			JsonEscape(pt_name).GetString(),
			pt_samples);
		for (int card_index = 0; card_index < kMaxNumberOfCardsPerPlayer; ++card_index) {
			if (card_index > 0) json += ",";
			CString card = player == NULL ? "" : CardToken(player->hole_cards(card_index));
			json.AppendFormat("\"%s\"", JsonEscape(card).GetString());
		}
		json += "],\"hud\":[";
		// Scarlet Beast server-scrape: render the HUD from the API's active profile
		// (the .pt4hud layout) + live per-seat stats, since there's no PokerTracker DB.
		if (p_scarlet_beast != NULL && p_scarlet_beast->ScrapeFromServer()) {
			json += p_scarlet_beast->ServerHudArrayForChair(chair).c_str();
		}
		// Otherwise: PT4 stats, only once the name mapping is verified ("confirmed").
		else if (p_hud_manager != NULL && p_hud_manager->IsEnabled()) {   // [Emrald] no name_verified gate: HUD for any chair, like scrcpy
			const std::vector<SHudStatValue> &stats = p_hud_manager->StatsForChair(chair);
			for (size_t stat_index = 0; stat_index < stats.size(); ++stat_index) {
				if (stat_index > 0) json += ",";
				json.AppendFormat("{\"abbr\":\"%s\",\"name\":\"%s\",\"value\":\"%s\",\"important\":%s}",
					JsonEscape(stats[stat_index].abbreviation).GetString(),
					JsonEscape(stats[stat_index].full_name).GetString(),
					JsonEscape(stats[stat_index].value).GetString(),
					stats[stat_index].important ? "true" : "false");
			}
		}
		json += "]}";
	}
	json += "]}";
	return json;
}

CStringA CChatTerminalServer::QueryValue(CStringA query, CStringA name)
{
	int start = 0;
	while (start <= query.GetLength()) {
		int end = query.Find('&', start);
		CStringA pair = end >= 0 ? query.Mid(start, end - start) : query.Mid(start);
		int equals = pair.Find('=');
		CStringA key = equals >= 0 ? pair.Left(equals) : pair;
		if (key.CompareNoCase(name) == 0) {
			return equals >= 0 ? pair.Mid(equals + 1) : "";
		}
		if (end < 0) {
			break;
		}
		start = end + 1;
	}
	return "";
}

CStringA CChatTerminalServer::UrlDecode(CStringA value)
{
	CStringA decoded;
	for (int i = 0; i < value.GetLength(); ++i) {
		char c = value[i];
		if (c == '+') {
			decoded += ' ';
		}
		else if (c == '%' && i + 2 < value.GetLength()) {
			char hex[3] = { value[i + 1], value[i + 2], 0 };
			char *end = NULL;
			long parsed = strtol(hex, &end, 16);
			if (end != hex) {
				decoded += (char)parsed;
				i += 2;
			}
		}
		else {
			decoded += c;
		}
	}
	return decoded;
}

int CChatTerminalServer::SectionFromText(CStringA value)
{
	if (value.IsEmpty()) return kChatTerminalContext;
	if (value.CompareNoCase("context") == 0) return kChatTerminalContext;
	if (value.CompareNoCase("state") == 0) return kChatTerminalState;
	if (value.CompareNoCase("decisions") == 0) return kChatTerminalDecisions;
	if (value.CompareNoCase("decision") == 0) return kChatTerminalDecisions;
	if (value.CompareNoCase("chat") == 0) return kChatTerminalChat;
	int section = atoi(value.GetString());
	if (section < 0 || section >= kChatTerminalSectionCount) {
		section = kChatTerminalContext;
	}
	return section;
}
