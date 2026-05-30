#pragma once

// Minimal localhost HTTP server for the trainer web table. Modeled on Hiss's
// CChatTerminalServer. Serves the static page from trainer\web\ and a small JSON
// + image + save API backed by CSampleStore (p_sample_store).
class CTrainerServer {
public:
	CTrainerServer();
	~CTrainerServer();

	bool Start(unsigned short port = 28700);
	void Stop(void);
	unsigned short port(void) const { return _port; }

private:
	static UINT ServerThread(LPVOID param);
	void Run(void);
	void HandleClient(SOCKET client);
	CStringA Response(CStringA body, CStringA content_type = "text/plain; charset=utf-8", CStringA status = "200 OK");
	void SendBinary(SOCKET client, const unsigned char *data, int len, CStringA content_type);
	bool ServeFile(SOCKET client, CString relative_path);
	CStringA ContentType(CString path);
	CStringA QueryValue(CStringA query, CStringA name);
	CStringA UrlDecode(CStringA value);

	CWinThread *_thread;
	SOCKET _listen_socket;
	unsigned short _port;
	volatile bool _stop;
};

extern CTrainerServer *p_trainer_server;
