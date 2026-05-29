#include "stdafx.h"
#include "ChatTerminalServer.h"
#include "ChatTerminalWindow.h"
#include "CEngineContainer.h"
#include "CSymbolEngineTableLimits.h"
#include "CSymbolEngineGameType.h"
#include "CSymbolEngineChipAmounts.h"
#include "CHandresetDetector.h"
#include "CTableState.h"
#include "CPokerTrackerThread.h"
#include "HudManager.h"
#include "..\CTablemap\CTablemap.h"
#include "..\PokerTracker_Query_Definitions\pokertracker_query_definitions.h"

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

	_port = port;
	_stop = false;
	_thread = AfxBeginThread(ServerThread, this, THREAD_PRIORITY_NORMAL, 0, CREATE_SUSPENDED, NULL);
	if (_thread == NULL) {
		WSACleanup();
		return false;
	}
	_thread->m_bAutoDelete = false;
	_thread->ResumeThread();
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
	_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (_listen_socket == INVALID_SOCKET) {
		return;
	}

	BOOL reuse = TRUE;
	setsockopt(_listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

	sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(_port);

	if (::bind(_listen_socket, (sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) {
		ChatTerminalAppend(kChatTerminalContext, "Terminal API server failed to bind localhost port.");
		closesocket(_listen_socket);
		_listen_socket = INVALID_SOCKET;
		return;
	}
	if (::listen(_listen_socket, SOMAXCONN) == SOCKET_ERROR) {
		closesocket(_listen_socket);
		_listen_socket = INVALID_SOCKET;
		return;
	}

	CString ready;
	ready.Format("Terminal API server listening on http://127.0.0.1:%u", _port);
	ChatTerminalAppend(kChatTerminalContext, ready);

	while (!_stop) {
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
		char c = input[i];
		switch (c) {
		case '\\': escaped += "\\\\"; break;
		case '"': escaped += "\\\""; break;
		case '\r': break;
		case '\n': escaped += "\\n"; break;
		case '\t': escaped += "\\t"; break;
		default: escaped += c; break;
		}
	}
	return escaped;
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
	if (p_hud_manager != NULL) {
		p_hud_manager->RefreshIfNeeded(handnumber, false);
	}

	json.Format("{\"nchairs\":%d,\"handnumber\":\"%s\",\"limits\":{\"sblind\":%.2f,\"bblind\":%.2f,\"ante\":%.2f,\"gametype\":%d},\"pot\":%.2f,",
		nchairs, JsonEscape(handnumber).GetString(), sblind, bblind, ante, gametype, pot);
	json += "\"commonCards\":[";
	for (int i = 0; i < kNumberOfCommunityCards; ++i) {
		if (i > 0) json += ",";
		CString card = p_table_state == NULL ? "" : p_table_state->CommonCards(i)->ToString();
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
			// Reading pt_hands also drives the name-matching / verification flow.
			double hands = PT_DLL_GetStat("pt_hands", chair);
			name_matched = _player_data[chair].found;
			name_verified = _player_data[chair].verified;
			pt_name = _player_data[chair].pt_name;
			if (name_verified && hands != kUndefined && hands >= 0) {
				pt_samples = (int)hands;
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
			CString card = player == NULL ? "" : player->hole_cards(card_index)->ToString();
			json.AppendFormat("\"%s\"", JsonEscape(card).GetString());
		}
		json += "],\"hud\":[";
		// Stats are only exposed once the name mapping is verified ("confirmed").
		if (name_verified && p_hud_manager != NULL && p_hud_manager->IsEnabled()) {
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
