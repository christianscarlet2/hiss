#include "stdafx.h"
#include "ChatTerminalServer.h"
#include "ChatTerminalWindow.h"

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
	_thread = AfxBeginThread(ServerThread, this, THREAD_PRIORITY_NORMAL, 0, 0, NULL);
	if (_thread == NULL) {
		WSACleanup();
		return false;
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

	if (bind(_listen_socket, (sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) {
		ChatTerminalAppend(kChatTerminalContext, "Terminal API server failed to bind localhost port.");
		closesocket(_listen_socket);
		_listen_socket = INVALID_SOCKET;
		return;
	}
	if (listen(_listen_socket, SOMAXCONN) == SOCKET_ERROR) {
		closesocket(_listen_socket);
		_listen_socket = INVALID_SOCKET;
		return;
	}

	CString ready;
	ready.Format("Terminal API server listening on http://127.0.0.1:%u", _port);
	ChatTerminalAppend(kChatTerminalContext, ready);

	while (!_stop) {
		SOCKET client = accept(_listen_socket, NULL, NULL);
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
