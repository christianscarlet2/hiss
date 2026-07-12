#include "stdafx.h"
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
#include "..\DLLs\Files_DLL\Files.h"

#pragma comment(lib, "ws2_32.lib")

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
	CStringA request(buffer);

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
		EnterCriticalSection(&CHeartbeatThread::cs_update_in_progress);
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
		LeaveCriticalSection(&CHeartbeatThread::cs_update_in_progress);
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
		g_reset_detection_request = true;
		CStringA response = Response("{\"ok\":true,\"reset\":true}\r\n");
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
	json.Format("{\"nchairs\":%d,\"userchair\":%d,\"toact\":%d,\"handnumber\":\"%s\",\"ismyturn\":%s,\"isomaha\":%s,\"isplo8\":%s,\"ispl\":%s,\"observer\":%s,\"table\":\"%s\",\"limits\":{\"sblind\":%.2f,\"bblind\":%.2f,\"ante\":%.2f,\"gametype\":%d},\"pot\":%.2f,\"beastfavor\":%.3f,\"fckra\":\"%s\",\"tiolp\":\"%s\",",
		nchairs, userchair, toact, JsonEscape(handnumber).GetString(),
		ismyturn ? "true" : "false", is_omaha ? "true" : "false",
		is_plo8 ? "true" : "false", is_pl ? "true" : "false",
		observer ? "true" : "false", JsonEscape(g_table_identity).GetString(), sblind, bblind, ante, gametype, pot, beastfavor_live,
		g_fckra_indicator, g_tiolp_indicator);
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
