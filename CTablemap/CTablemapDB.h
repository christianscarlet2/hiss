//*******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//*******************************************************************************
//
// Purpose: Persist tablemaps in a PostgreSQL database ("hiss") instead of
//   flat *.tm text files. Mirrors CTablemap's file load/save logic, populating
//   the same in-memory CTablemap model via its insert mutators. Shared by Hiss,
//   Vision (OpenScrape) and Trainer.
//
//*******************************************************************************

#ifndef INC_CTABLEMAPDB_H
#define INC_CTABLEMAPDB_H

#include <atlstr.h>
#include <vector>
#include "CTablemap.h"
#include "..\Shared\CCritSec\CCritSec.h"

// Lightweight summary of a tablemap row, used for the picker UI and for
// Hiss' connection matching.
struct STablemapDBInfo {
	CString name;
	CString sitename;
	CString titletext;
};

// One live Hiss instance's auto-connector state (oh_attached_windows row).
struct SAttachedWindow {
	int       session_id;
	long      pid;
	long long hwnd;          // attached poker window (0 = none)
};

// Own-data HUD stats for one player (computed from hud_player_stats). -1.0 = no opportunities yet.
struct SHudDbStats {
	bool   found;
	int    hands;
	double vpip, pfr, threeb, fourb, fiveb, f3b, f4b, af, cbet, ftc, steal, fts, wtsd;
};

// Per-opponent INTROSPECTION profile (opponent_profile, fed by introspect_aggregator.py): a
// rolling-100-hand behavioural model, SEPARATED by gametype. -1.0 = unknown / not enough sample.
struct SOppProfile {
	bool    found;
	int     window_hands;
	double  cont_freq;         // likelihood to KEEP firing after an aggressive action (rhythm)
	double  aggr_index;        // aggression density 0..1
	double  fold_to_pressure;  // over-fold tendency 0..1
	double  sd_strong_rate;    // of showdowns, how often they actually had it
	double  fastbet_tell;      // P(strong | bet fast); -1 = unknown
	int     fast_n;
	int     profile_code;      // 0=unknown 1=nit 2=tag 3=lag 4=station 5=fish 6=maniac
	// concrete exploit flags (0/1), deep-wired into the OHF / NN / advisor.
	int overfold, folds_to_3bet, gives_up, keeps_firing, never_folds, honest, fast_is_weak, fast_is_strong;
};

// One per-action latency record (timing tell), emitted off the hot path by the introspection engine.
struct SOppTimingRow {
	CString   player;
	int       street;
	CString   action;
	int       latency_ms;
	long long ts_ms;
};

class CTablemapDB {
public:
	CTablemapDB();
	~CTablemapDB();

public:
	// Connection. The connection string defaults to the PokerTracker server
	// credentials (Hiss) or a localhost fallback, always with dbname=hiss.
	static CString DefaultConnString();
	static void    SetConnString(const CString &conn_str);
	bool    Connect();
	void    Disconnect();
	bool    IsConnected();
	CString LastError() { return _last_error; }

public:
	// Creates any missing tables (idempotent). Called automatically by Connect().
	bool EnsureSchema();

public:
	// Load/save using the shared in-memory CTablemap model.
	// Return SUCCESS / ERR_* (see MagicNumbers.h).
	int  LoadTablemapFromDB(const CString name, CTablemap *tm);
	int  SaveTablemapToDB(const CString name, CTablemap *tm);
	bool DeleteTablemap(const CString name);

public:
	// Import legacy *.tm files (the only remaining use of the file parser).
	// ImportTmFileToDB parses one file and writes it to the DB.
	// ImportFolderToDB recursively imports every *.tm under a folder.
	bool ImportTmFileToDB(const CString path, CString *out_name);
	int  ImportFolderToDB(const CString folder, int *imported, int *failed, CString *report);

public:
	// Enumeration.
	bool ListTablemaps(std::vector<STablemapDBInfo> *out);
	bool TablemapExists(const CString name);

public:
	// Application settings (the `settings` table; one row per key, JSON value).
	// `field` is a top-level key inside the JSON object.
	CString GetSettingString(const CString key, const CString field);
	bool    GetSettingArray(const CString key, const CString field, std::vector<CString> *out);
	bool    SetSettingString(const CString key, const CString field, const CString value);
	bool    SetSettingArray(const CString key, const CString field, const std::vector<CString> &values);

	// Cheap "anything changed?" probe for live-reload polling: the latest updated_at
	// across the settings + tablemaps tables, as a text timestamp ("" on error).
	// Hiss's heartbeat compares this against the last seen value.
	CString GetSettingsRevision();

public:
	// Cross-instance auto-connector coordination (oh_attached_windows table).
	// Replaces the unreliable in-process shared-memory segment. Used by CSharedMem.
	//   DBSetAttachedWindow: upsert this instance's row (hwnd=0 clears the attachment).
	//   DBListAttachedWindows: every instance's current row (caller checks pid liveness).
	//   DBClearAttachedSession: remove a row (used to reap dead instances).
	bool DBSetAttachedWindow(int session_id, long pid, long long hwnd);
	bool DBListAttachedWindows(std::vector<SAttachedWindow> *out);
	bool DBClearAttachedSession(int session_id);

	// Own-data HUD: read computed per-player stats from hud_player_stats (fed by hud_aggregator.py).
	// The gametype overload filters to 'nlhe'/'plo'/'plo8' so a player's PLO read never bleeds
	// into their NLH read; the legacy overload sums across gametypes (back-compat).
	bool GetHudPlayerStats(const CString &player, SHudDbStats *out);
	bool GetHudPlayerStats(const CString &player, const CString &gametype, SHudDbStats *out);
	// Per-opponent introspection profile (opponent_profile), gametype-matched.
	bool GetOpponentProfile(const CString &player, const CString &gametype, SOppProfile *out);
	// Durable per-action timing rows (latency tells) -> opponent_timing. One batched INSERT,
	// called off the hot path (hand end), NEVER per heartbeat.
	bool EmitOpponentTiming(const CString &gametype, const CString &handnumber,
		const std::vector<SOppTimingRow> &rows);

private:
	long GetTablemapId(const CString name);   // -1 if not found / error
	bool ExecCommand(const CString &sql);      // expects PGRES_COMMAND_OK
	static CString NameFromPath(const CString path);

private:
	void   *_conn;          // PGconn* (opaque so libpq stays out of the header)
	CString _last_error;
	static CString s_conn_str;
	// libpq is NOT thread-safe on a single connection. Hiss touches this shared
	// connection from the heartbeat thread (scrape + settings live-reload probe)
	// and from the GUI/autoconnector, so every method that uses _conn serialises
	// through this (recursive) lock. Without it, concurrent PQexec corrupts the
	// PGresult and crashes in PQgetisnull. Recursive, so nested Connect() is fine.
	CCritSec _db_cs;
};

extern CTablemapDB *p_tablemap_db;

#endif // INC_CTABLEMAPDB_H
