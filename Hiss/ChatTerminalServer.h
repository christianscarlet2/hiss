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
	CStringA BinaryResponse(CByteArray &body, CStringA content_type, CStringA status = "200 OK");
	bool ServeFile(SOCKET client, CString relative_path);
	CStringA ContentType(CString path);
	CStringA BuildTableStateJson(void);
	CStringA JsonEscape(CString value);
	CStringA QueryValue(CStringA query, CStringA name);
	CStringA UrlDecode(CStringA value);
	int SectionFromText(CStringA value);

	CWinThread *_thread;
	SOCKET _listen_socket;
	unsigned short _port;
	volatile bool _stop;
};

extern CChatTerminalServer *p_chat_terminal_server;

// ---- /api/symbols snapshot -------------------------------------------------------------------
// /api/symbols used to EVALUATE symbols on the HTTP thread while holding the heartbeat's update
// lock. That made a read of the bot's state able to STALL the bot: a heavy or merely large query
// blocked the heartbeat for its whole duration, and the endpoint wedged (reproduced live -- a few
// back-to-back 110-symbol pulls and every subsequent request, even a single cheap symbol, timed
// out). The nn_driver cannot decide a hand without this endpoint, so the one thing it must never
// do is take the bot down with it.
//
// Now the HEARTBEAT publishes the values (it is already inside the lock, and it has already
// evaluated the engines -- measured at 0 ms) and the HTTP thread only READS the snapshot. The
// reader never touches cs_update_in_progress, so no query, however large, can stall the bot.
//
// RefreshSymbolSnapshot() is called from the heartbeat INSIDE cs_update_in_progress.
void RefreshSymbolSnapshot(void);

// Names the HTTP callers have asked for at least once. The heartbeat keeps exactly these fresh --
// we do not evaluate every symbol in the engine, only the ones somebody is actually reading.
void WantSymbols(const CStringA &comma_separated_names);

// Serve the snapshot. Returns false when a requested name has never been snapshotted (a first-ever
// request), in which case the caller falls back to the old evaluate-under-the-lock path once, which
// also seeds the snapshot for every request after it.
bool SymbolsFromSnapshot(const CStringA &comma_separated_names, CStringA *json_body);

#endif // INC_CHAT_TERMINAL_SERVER_H
