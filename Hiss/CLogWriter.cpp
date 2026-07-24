//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: see CLogWriter.h
//
//******************************************************************************

#include "stdafx.h"
#include "CLogWriter.h"

#include <shlobj.h>      // SHCreateDirectoryExA
#include <shellapi.h>    // SHFileOperationA
#include "libpq-fe.h"
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include "..\CTablemap\CTablemapDB.h"   // p_tablemap_db (read the logging_enabled toggle)

CLogWriter *p_log_writer = NULL;

// Keep the most-recent N hands locally (server keeps 100).
// 100, not 10: at 10 the local Advanced Replay pruned a hand within a couple of minutes, so by the
// time a misplay was reported its frames were already gone (hand 2777172499 -- the frames the bug
// report pointed at had been deleted, and /api/frames returned []). Debugging a live bot means
// looking BACK at the moment, so keep the local window as deep as the server's. [Emrald]
static const int kLocalHandsToKeep = 100;

static CString EnvOr(const char *name, const char *def) {
	char buf[512] = {0}; size_t n = 0;
	if (getenv_s(&n, buf, sizeof(buf), name) == 0 && n > 0 && buf[0] != '\0') return CString(buf);
	return CString(def);
}

static CString ReplayBaseDir() {
	char path[MAX_PATH] = {0};
	GetModuleFileNameA(NULL, path, MAX_PATH);
	char *slash = strrchr(path, '\\');
	if (slash) *slash = '\0';
	CString dir(path);
	dir += "\\logs\\replay";
	return dir;
}

CLogWriter::CLogWriter() {
	_enabled = false;
	_stop = false;
	_thread = NULL;
	_wake = NULL;
	_last_frame_hash = "";
	_last_rotated_hand = "";
	InitializeCriticalSection(&_cs);
}

CLogWriter::~CLogWriter() {
	Stop();
	DeleteCriticalSection(&_cs);
}

void CLogWriter::RefreshEnabled() {
	if (p_tablemap_db == NULL) return;
	CString v = p_tablemap_db->GetSettingString("prefs", "logging_enabled");
	_enabled = (v == "1" || v.CompareNoCase("true") == 0);
}

void CLogWriter::Start() {
	if (_thread != NULL) return;
	RefreshEnabled();
	_stop = false;
	_wake = CreateEvent(NULL, FALSE, FALSE, NULL);   // auto-reset
	CWinThread *t = AfxBeginThread(ThreadProc, this);
	_thread = (t != NULL) ? t->m_hThread : NULL;
}

void CLogWriter::Stop() {
	if (_thread == NULL) { if (_wake) { CloseHandle(_wake); _wake = NULL; } return; }
	_stop = true;
	if (_wake) SetEvent(_wake);
	WaitForSingleObject(_thread, 5000);
	_thread = NULL;
	if (_wake) { CloseHandle(_wake); _wake = NULL; }
}

UINT __cdecl CLogWriter::ThreadProc(LPVOID param) {
	((CLogWriter *)param)->Run();
	return 0;
}

void CLogWriter::EnqueueSql(const CString &sql) {
	if (!_enabled) return;
	EnterCriticalSection(&_cs);
	_sql.push_back(sql);
	LeaveCriticalSection(&_cs);
	if (_wake) SetEvent(_wake);
}

CString CLogWriter::EscLit(const CString &s) {
	CString out; out.Preallocate(s.GetLength() + 8);
	for (int i = 0; i < s.GetLength(); ++i) { TCHAR c = s[i]; if (c == '\'') out += "''"; else out += c; }
	return out;
}

unsigned long long CLogWriter::HashBits(const std::vector<BYTE> &b) {
	unsigned long long h = 1469598103934665603ULL;   // FNV-1a 64
	for (size_t i = 0; i < b.size(); i += 257) {       // sparse sample: plenty for change-detection
		h ^= b[i]; h *= 1099511628211ULL;
	}
	h ^= b.size(); h *= 1099511628211ULL;
	return h;
}

void *CLogWriter::Connect() {
	CString conn;
	conn.Format("host=%s port=%s user=%s password='%s' dbname='%s'",
		EnvOr("PGHOST", "127.0.0.1").GetString(), EnvOr("PGPORT", "5432").GetString(),
		EnvOr("PGUSER", "postgres").GetString(), EnvOr("PGPASSWORD", "dbpass").GetString(),
		EnvOr("PGDATABASE", "hiss").GetString());
	PGconn *c = PQconnectdb(conn.GetString());
	if (c == NULL || PQstatus(c) != CONNECTION_OK) { if (c) PQfinish(c); return NULL; }
	return c;
}

bool CLogWriter::Exec(void *conn, const CString &sql) {
	if (conn == NULL) return false;
	PGresult *r = PQexec((PGconn *)conn, CStringA(sql).GetString());
	bool ok = r && (PQresultStatus(r) == PGRES_COMMAND_OK || PQresultStatus(r) == PGRES_TUPLES_OK);
	if (r) PQclear(r);
	return ok;
}

void CLogWriter::HandleFrame(void *conn, const FrameJob &f) {
	if (f.w <= 0 || f.h <= 0 || f.bits.empty()) return;
	// Change detection (sparse FNV hash of the pixels).
	unsigned long long hh = HashBits(f.bits);
	CString hex; hex.Format("%016llx", hh);
	bool changed = (hex != _last_frame_hash);
	_last_frame_hash = hex;
	// Encode PNG (bits are top-down BGRA from GetDIBits). Filename is KEYED by street + active-seat (+ ts
	// for uniqueness within the same key) so a frame is trivially identifiable on disk: e.g. s2_seat3_<ts>.png
	// = flop, seat 3 to act. [Emrald: key files on active-player + hand + street]
	CString dir = ReplayBaseDir() + "\\" + f.hand;
	SHCreateDirectoryExA(NULL, CStringA(dir).GetString(), NULL);
	CString png; png.Format("%s\\s%d_seat%d_%lld.png", dir.GetString(), f.betround, f.active_seat, f.ts);
	try {
		cv::Mat m(f.h, f.w, CV_8UC4, (void *)f.bits.data());
		std::vector<int> params; params.push_back(cv::IMWRITE_PNG_COMPRESSION); params.push_back(3);
		cv::imwrite(CStringA(png).GetString(), m, params);
	} catch (...) { return; }
	CString sql;
	sql.Format("INSERT INTO hiss_log_frames(ts_ms,handnumber,betround,png_path,sha256,changed,active_seat,hole)"
		" VALUES (%lld,'%s',%d,'%s','%s',%s,%d,'%s')",
		f.ts, EscLit(f.hand).GetString(), f.betround, EscLit(png).GetString(),
		hex.GetString(), changed ? "true" : "false", f.active_seat, EscLit(f.hole).GetString());
	Exec(conn, sql);
	RotateLocalIfNewHand(conn, f.hand);
}

void CLogWriter::RotateLocalIfNewHand(void *conn, const CString &hand) {
	if (hand.IsEmpty() || hand == _last_rotated_hand) return;
	_last_rotated_hand = hand;
	// Keep rows for the kLocalHandsToKeep most-recent distinct handnumbers (by max ts_ms).
	const char *tables[] = { "hiss_log_frames", "hiss_log_scrapes", "hiss_log_symbols",
		"hiss_log_decisions", "hiss_log_debug", "hiss_log_hands" };
	for (int i = 0; i < 6; ++i) {
		CString sql;
		sql.Format("DELETE FROM %s WHERE handnumber NOT IN "
			"(SELECT handnumber FROM %s WHERE handnumber<>'' GROUP BY handnumber "
			"ORDER BY max(ts_ms) DESC LIMIT %d)", tables[i], tables[i], kLocalHandsToKeep);
		Exec(conn, sql);
	}
	// Prune PNG dirs that are no longer in hiss_log_frames.
	PGresult *r = PQexec((PGconn *)conn,
		"SELECT DISTINCT handnumber FROM hiss_log_frames WHERE handnumber<>''");
	if (r && PQresultStatus(r) == PGRES_TUPLES_OK) {
		std::vector<CString> keep;
		for (int i = 0; i < PQntuples(r); ++i) keep.push_back(CString(PQgetvalue(r, i, 0)));
		CString base = ReplayBaseDir();
		WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(CStringA(base + "\\*").GetString(), &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
				if (fd.cFileName[0] == '.') continue;
				CString name(fd.cFileName); bool found = false;
				for (size_t k = 0; k < keep.size(); ++k) if (keep[k] == name) { found = true; break; }
				if (!found) {
					CString del = base + "\\" + name;
					CStringA delA(del); char from[MAX_PATH + 2] = {0};
					strcpy_s(from, delA.GetString()); from[strlen(from) + 1] = '\0';   // double-null
					SHFILEOPSTRUCTA op = {0}; op.wFunc = FO_DELETE; op.pFrom = from;
					op.fFlags = FOF_NO_UI | FOF_SILENT | FOF_NOCONFIRMATION;
					SHFileOperationA(&op);
				}
			} while (FindNextFileA(h, &fd));
			FindClose(h);
		}
	}
	if (r) PQclear(r);
}

void CLogWriter::Run() {
	void *conn = Connect();
	while (!_stop) {
		WaitForSingleObject(_wake, 1000);
		// Drain frames
		for (;;) {
			FrameJob f; bool have = false;
			EnterCriticalSection(&_cs);
			if (!_frames.empty()) { f = _frames.front(); _frames.pop_front(); have = true; }
			LeaveCriticalSection(&_cs);
			if (!have) break;
			if (conn == NULL) conn = Connect();
			if (conn) HandleFrame(conn, f);
		}
		// Drain SQL rows (batch in a transaction)
		std::deque<CString> batch;
		EnterCriticalSection(&_cs);
		batch.swap(_sql);
		LeaveCriticalSection(&_cs);
		if (!batch.empty()) {
			if (conn == NULL) conn = Connect();
			if (conn) {
				Exec(conn, "BEGIN");
				while (!batch.empty()) { Exec(conn, batch.front()); batch.pop_front(); }
				Exec(conn, "COMMIT");
			}
		}
	}
	if (conn) PQfinish((PGconn *)conn);
}

// ---------------------------------------------------------------------------
// Enqueue API (cheap; runs on the heartbeat / decision / flush thread)
// ---------------------------------------------------------------------------
void CLogWriter::LogFrame(const void *bgra, int width, int height,
		long long ts_ms, const char *handnumber, int betround, int active_seat, const char *hole) {
	if (!_enabled || bgra == NULL || width <= 0 || height <= 0) return;
	FrameJob f; f.ts = ts_ms; f.hand = handnumber ? handnumber : ""; f.betround = betround;
	f.active_seat = active_seat; f.hole = hole ? hole : "";
	f.w = width; f.h = height;
	size_t bytes = (size_t)width * height * 4;
	f.bits.resize(bytes);
	memcpy(f.bits.data(), bgra, bytes);
	EnterCriticalSection(&_cs);
	if (_frames.size() < 240) _frames.push_back(std::move(f));   // safety cap (~2-4 min backlog)
	LeaveCriticalSection(&_cs);
	if (_wake) SetEvent(_wake);
}

void CLogWriter::LogScrape(long long ts_ms, const char *hand, int betround,
		const char *region, const char *text, bool is_crop) {
	if (!_enabled) return;
	CString sql;
	sql.Format("INSERT INTO hiss_log_scrapes(ts_ms,handnumber,betround,region_name,ocr_text,is_crop)"
		" VALUES (%lld,'%s',%d,'%s','%s',%s)", ts_ms, EscLit(hand ? hand : "").GetString(), betround,
		EscLit(region ? region : "").GetString(), EscLit(text ? text : "").GetString(),
		is_crop ? "true" : "false");
	EnqueueSql(sql);
}

void CLogWriter::LogSymbol(long long ts_ms, const char *hand, int betround,
		const char *name, const char *value) {
	if (!_enabled) return;
	CString sql;
	sql.Format("INSERT INTO hiss_log_symbols(ts_ms,handnumber,betround,name,value)"
		" VALUES (%lld,'%s',%d,'%s','%s')", ts_ms, EscLit(hand ? hand : "").GetString(), betround,
		EscLit(name ? name : "").GetString(), EscLit(value ? value : "").GetString());
	EnqueueSql(sql);
}

void CLogWriter::LogDecision(long long ts_ms, const char *hand, int betround,
		const char *hero_cards, const char *action, double amount,
		double f_fold, double f_call, double f_check, double f_raise,
		double f_allin, double f_betsize, const char *trace) {
	if (!_enabled) return;
	CString sql;
	sql.Format("INSERT INTO hiss_log_decisions(ts_ms,handnumber,betround,hero_cards,action,amount,"
		"f_fold,f_call,f_check,f_raise,f_allin,f_betsize,trace) VALUES "
		"(%lld,'%s',%d,'%s','%s',%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,'%s')",
		ts_ms, EscLit(hand ? hand : "").GetString(), betround, EscLit(hero_cards ? hero_cards : "").GetString(),
		EscLit(action ? action : "").GetString(), amount, f_fold, f_call, f_check, f_raise, f_allin,
		f_betsize, EscLit(trace ? trace : "").GetString());
	EnqueueSql(sql);
}

void CLogWriter::LogHand(long long ts_ms, const char *hand, bool complete, const char *hh_text) {
	if (!_enabled) return;
	CString sql;
	sql.Format("INSERT INTO hiss_log_hands(ts_ms,handnumber,complete,hh_text)"
		" VALUES (%lld,'%s',%s,'%s')", ts_ms, EscLit(hand ? hand : "").GetString(),
		complete ? "true" : "false", EscLit(hh_text ? hh_text : "").GetString());
	EnqueueSql(sql);
}

void CLogWriter::LogLearnerDecision(const char *hand, int betround, const char *hero_cards,
		const char *board, double pot, double to_call,
		const char *human_action, double human_amount,
		const char *bot_action, double bot_amount, const char *source) {
	// NOT gated on _enabled: this is the manual-play capture, not replay logging. Run() drains _sql
	// regardless of the toggle, so the row lands even with advanced logging off. ts/reasoning/game_state
	// are left to their column defaults/NULL (the panel captures no free-text reason). source tags which
	// engine produced bot_action ('ohf' or 'nn') so the learn daemon routes the improvement to the right
	// place -- OHF divergences -> OHF proposals, NN divergences -> the NN retraining dataset.
	CString br;   if (betround >= 0)     br.Format("%d", betround);       else br = "NULL";
	CString ha;   if (human_amount >= 0) ha.Format("%.4f", human_amount); else ha = "NULL";
	CString oa;   if (bot_action && bot_action[0]) oa.Format("'%s'", EscLit(bot_action).GetString()); else oa = "NULL";
	CString oamt; if (bot_amount >= 0)   oamt.Format("%.4f", bot_amount); else oamt = "NULL";
	CString sql;
	sql.Format("INSERT INTO learner_decisions"
		"(handnumber,betround,hero_cards,board,pot,amount_to_call,action,amount,ohf_action,ohf_amount,source)"
		" VALUES ('%s',%s,'%s','%s',%.4f,%.4f,'%s',%s,%s,%s,'%s')",
		EscLit(hand ? hand : "").GetString(), br.GetString(),
		EscLit(hero_cards ? hero_cards : "").GetString(), EscLit(board ? board : "").GetString(),
		pot, to_call, EscLit(human_action ? human_action : "").GetString(),
		ha.GetString(), oa.GetString(), oamt.GetString(),
		EscLit(source && source[0] ? source : "ohf").GetString());
	EnqueueSql(sql);
}
