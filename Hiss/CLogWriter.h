//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Background logging writer for the replay/debug system. The heartbeat
//   thread ENQUEUES cheap items (a copied frame buffer, or a ready SQL row); a
//   dedicated worker thread does the expensive work (PNG encode + postgres bulk
//   inserts + local rotation) so the bot's heartbeat is never stalled. Rows land
//   in the local postgres `hiss_log_*` outbox (see CTablemapDB::EnsureSchema);
//   the shipper later forwards them to hiss.scarletbeast.com.
//
//******************************************************************************

#ifndef INC_CLOGWRITER_H
#define INC_CLOGWRITER_H

#include <afx.h>
#include <windows.h>
#include <vector>
#include <deque>

class CLogWriter {
public:
	CLogWriter();
	~CLogWriter();

	void Start();
	void Stop();
	// Re-read the `logging_enabled` toggle from the DB (prefs). Cheap; call occasionally.
	void RefreshEnabled();
	bool Enabled() const { return _enabled; }

	// --- Enqueue API (called from the heartbeat / decision / flush; cheap) -------------
	// Copies the BGRA frame buffer; the worker encodes the PNG + inserts the row.
	// active_seat = the chair whose turn it is (or hero when it's our turn), hole = hero's hole cards.
	// Both are persisted so the Advanced Replay can KEY frames by active-player + hand + street and
	// list/search by hole card + hand number. [Emrald]
	void LogFrame(const void *bgra, int width, int height,
		long long ts_ms, const char *handnumber, int betround, int active_seat, const char *hole);
	void LogScrape(long long ts_ms, const char *handnumber, int betround,
		const char *region, const char *text, bool is_crop);
	void LogSymbol(long long ts_ms, const char *handnumber, int betround,
		const char *name, const char *value);
	void LogDecision(long long ts_ms, const char *handnumber, int betround,
		const char *hero_cards, const char *action, double amount,
		double f_fold, double f_call, double f_check, double f_raise,
		double f_allin, double f_betsize, const char *trace);
	void LogHand(long long ts_ms, const char *handnumber, bool complete, const char *hh_text);

private:
	struct FrameJob {
		long long ts; CString hand; int betround; int w; int h; std::vector<BYTE> bits;
		int active_seat; CString hole;
	};
	static UINT __cdecl ThreadProc(LPVOID param);
	void Run();
	void *Connect();                                   // returns PGconn* (opaque here)
	bool Exec(void *conn, const CString &sql);
	void HandleFrame(void *conn, const FrameJob &f);
	void RotateLocalIfNewHand(void *conn, const CString &hand);
	void EnqueueSql(const CString &sql);
	static CString EscLit(const CString &s);           // SQL ' escaping
	static unsigned long long HashBits(const std::vector<BYTE> &b);

	volatile bool _enabled;
	volatile bool _stop;
	HANDLE _thread;
	HANDLE _wake;                                      // auto-reset event: new work / stop
	CRITICAL_SECTION _cs;
	std::deque<FrameJob> _frames;
	std::deque<CString>  _sql;                         // ready SQL rows (scrapes/symbols/decisions/hands)
	CString _last_frame_hash;                          // hex of last frame -> `changed`
	CString _last_rotated_hand;                         // rotate only on a new hand
};

extern CLogWriter *p_log_writer;

#endif // INC_CLOGWRITER_H
