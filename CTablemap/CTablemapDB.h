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

// Lightweight summary of a tablemap row, used for the picker UI and for
// Hiss' connection matching.
struct STablemapDBInfo {
	CString name;
	CString sitename;
	CString titletext;
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

private:
	long GetTablemapId(const CString name);   // -1 if not found / error
	bool ExecCommand(const CString &sql);      // expects PGRES_COMMAND_OK
	static CString NameFromPath(const CString path);

private:
	void   *_conn;          // PGconn* (opaque so libpq stays out of the header)
	CString _last_error;
	static CString s_conn_str;
};

extern CTablemapDB *p_tablemap_db;

#endif // INC_CTABLEMAPDB_H
