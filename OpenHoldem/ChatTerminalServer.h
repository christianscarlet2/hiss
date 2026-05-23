#ifndef INC_CHAT_TERMINAL_SERVER_H
#define INC_CHAT_TERMINAL_SERVER_H

class CChatTerminalServer {
public:
	CChatTerminalServer();
	~CChatTerminalServer();

	bool Start(unsigned short port = 27654);
	void Stop(void);
	unsigned short port(void) const { return _port; }

private:
	static UINT ServerThread(LPVOID param);
	void Run(void);
	void HandleClient(SOCKET client);
	CStringA Response(CStringA body, CStringA status = "200 OK");
	CStringA QueryValue(CStringA query, CStringA name);
	CStringA UrlDecode(CStringA value);
	int SectionFromText(CStringA value);

	CWinThread *_thread;
	SOCKET _listen_socket;
	unsigned short _port;
	volatile bool _stop;
};

extern CChatTerminalServer *p_chat_terminal_server;

#endif // INC_CHAT_TERMINAL_SERVER_H
