//*******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//*******************************************************************************
//
// Purpose: PostgreSQL persistence for tablemaps. See CTablemapDB.h.
//
//*******************************************************************************

#include "StdAfx.h"
#include "CTablemapDB.h"

#include <set>
#include <vector>
#include "libpq-fe.h"
#include "../CTransform/pdiff/RGBAImage.h"
#include "..\Shared\MagicNumbers\MagicNumbers.h"

#ifdef OPENHOLDEM_PROGRAM
#include "../DLLs/Preferences_DLL/Preferences.h"
#endif

CTablemapDB *p_tablemap_db = NULL;
CString CTablemapDB::s_conn_str = "";

// Escape a string for safe inclusion inside a single-quoted SQL literal
// by doubling any embedded single quotes. (Same approach as COCRNameMapping.)
static CString EscapeSqlLiteral(const char *value) {
	CString result;
	if (value == NULL) {
		return result;
	}
	for (const char *c = value; *c != '\0'; ++c) {
		if (*c == '\'') {
			result += "''";
		} else {
			result += *c;
		}
	}
	return result;
}

static CString EscapeSqlLiteral(const CString &value) {
	return EscapeSqlLiteral(value.GetString());
}

// Split a multi-line pixel blob into rows, stripping any trailing '\r'.
static void SplitLines(const CString &text, std::vector<CString> *out) {
	out->clear();
	int start = 0;
	int len = text.GetLength();
	while (start <= len) {
		int nl = text.Find('\n', start);
		CString line = (nl < 0) ? text.Mid(start) : text.Mid(start, nl - start);
		line.TrimRight("\r");
		out->push_back(line);
		if (nl < 0) break;
		start = nl + 1;
	}
}

CTablemapDB::CTablemapDB()
	: _conn(NULL) {
}

CTablemapDB::~CTablemapDB() {
	Disconnect();
}

CString CTablemapDB::DefaultConnString() {
	CString s;
#ifdef OPENHOLDEM_PROGRAM
	// Reuse the PokerTracker server credentials, but a fixed dbname=hiss.
	s.Format("host=%s port=%s user=%s password='%s' dbname='hiss'",
		Preferences()->pt_ip_addr(), Preferences()->pt_port(),
		Preferences()->pt_user(), Preferences()->pt_pass());
#else
	s = "host=127.0.0.1 port=5432 user=postgres password='dbpass' dbname='hiss'";
#endif
	return s;
}

void CTablemapDB::SetConnString(const CString &conn_str) {
	s_conn_str = conn_str;
}

bool CTablemapDB::IsConnected() {
	return (_conn != NULL) && (PQstatus((PGconn *)_conn) == CONNECTION_OK);
}

bool CTablemapDB::Connect() {
	if (IsConnected()) {
		return true;
	}
	if (_conn != NULL) {
		PQfinish((PGconn *)_conn);
		_conn = NULL;
	}
	CString conn = s_conn_str.IsEmpty() ? DefaultConnString() : s_conn_str;
	PGconn *c = PQconnectdb(conn.GetString());
	if (c == NULL || PQstatus(c) != CONNECTION_OK) {
		_last_error.Format("Could not connect to the hiss database:\n%s",
			c ? PQerrorMessage(c) : "PQconnectdb returned NULL");
		if (c) PQfinish(c);
		_conn = NULL;
		return false;
	}
	_conn = c;
	EnsureSchema();
	return true;
}

void CTablemapDB::Disconnect() {
	if (_conn != NULL) {
		PQfinish((PGconn *)_conn);
		_conn = NULL;
	}
}

bool CTablemapDB::ExecCommand(const CString &sql) {
	if (!IsConnected()) {
		_last_error = "Not connected to the hiss database.";
		return false;
	}
	PGresult *res = PQexec((PGconn *)_conn, sql.GetString());
	ExecStatusType st = PQresultStatus(res);
	bool ok = (st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK);
	if (!ok) {
		_last_error.Format("SQL error: %s", PQerrorMessage((PGconn *)_conn));
	}
	if (res) PQclear(res);
	return ok;
}

bool CTablemapDB::EnsureSchema() {
	// Idempotent: matches postgres/hiss_schema.sql (without CREATE DATABASE).
	const char *ddl =
		"CREATE TABLE IF NOT EXISTS settings ("
		" key TEXT PRIMARY KEY, value JSONB NOT NULL DEFAULT '{}'::jsonb,"
		" updated_at TIMESTAMP DEFAULT now());"
		"CREATE TABLE IF NOT EXISTS tablemaps ("
		" id SERIAL PRIMARY KEY, name TEXT UNIQUE NOT NULL, sitename TEXT,"
		" titletext TEXT, source_file TEXT, bits_per_pixel INT DEFAULT 32,"
		" updated_at TIMESTAMP DEFAULT now());"
		"CREATE TABLE IF NOT EXISTS tm_sizes ("
		" tablemap_id INT REFERENCES tablemaps(id) ON DELETE CASCADE,"
		" name TEXT, width INT, height INT, PRIMARY KEY (tablemap_id, name));"
		"CREATE TABLE IF NOT EXISTS tm_symbols ("
		" tablemap_id INT REFERENCES tablemaps(id) ON DELETE CASCADE,"
		" name TEXT, text TEXT, PRIMARY KEY (tablemap_id, name));"
		"CREATE TABLE IF NOT EXISTS tm_regions ("
		" tablemap_id INT REFERENCES tablemaps(id) ON DELETE CASCADE,"
		" name TEXT, rgn_left INT, rgn_top INT, rgn_right INT, rgn_bottom INT,"
		" color BIGINT, radius INT, transform TEXT, use_default BOOL, threshold INT,"
		" use_cropping BOOL, crop_size INT, match_mode INT, sharpen INT,"
		" color2 BIGINT DEFAULT 0, color2_enabled BOOL DEFAULT false,"
		" PRIMARY KEY (tablemap_id, name));"
		// Bring older databases up to date (no-op if the columns already exist).
		"ALTER TABLE tm_regions ADD COLUMN IF NOT EXISTS color2 BIGINT DEFAULT 0;"
		"ALTER TABLE tm_regions ADD COLUMN IF NOT EXISTS color2_enabled BOOL DEFAULT false;"
		"CREATE TABLE IF NOT EXISTS tm_fonts ("
		" tablemap_id INT REFERENCES tablemaps(id) ON DELETE CASCADE,"
		" font_group INT, ch TEXT, hexmash TEXT, x_values TEXT,"
		" PRIMARY KEY (tablemap_id, font_group, hexmash));"
		"CREATE TABLE IF NOT EXISTS tm_hash_points ("
		" tablemap_id INT REFERENCES tablemaps(id) ON DELETE CASCADE,"
		" hash_group INT, x INT, y INT, PRIMARY KEY (tablemap_id, hash_group, x, y));"
		"CREATE TABLE IF NOT EXISTS tm_hash_values ("
		" tablemap_id INT REFERENCES tablemaps(id) ON DELETE CASCADE,"
		" hash_group INT, name TEXT, hash BIGINT, PRIMARY KEY (tablemap_id, hash_group, hash));"
		// Images are keyed in memory by name+pixels, so duplicate names are legal
		// within one tablemap; a surrogate id is the PK (not the name).
		"CREATE TABLE IF NOT EXISTS tm_images ("
		" id SERIAL PRIMARY KEY,"
		" tablemap_id INT REFERENCES tablemaps(id) ON DELETE CASCADE,"
		" name TEXT, width INT, height INT, pixels TEXT);"
		"CREATE INDEX IF NOT EXISTS idx_tm_images_tm ON tm_images(tablemap_id);"
		"CREATE TABLE IF NOT EXISTS tm_templates ("
		" tablemap_id INT REFERENCES tablemaps(id) ON DELETE CASCADE,"
		" name TEXT, rgn_left INT, rgn_top INT, rgn_right INT, rgn_bottom INT,"
		" width INT, height INT, use_default BOOL, match_mode INT, created BOOL,"
		" pixels TEXT, PRIMARY KEY (tablemap_id, name));";
	return ExecCommand(ddl);
}

long CTablemapDB::GetTablemapId(const CString name) {
	if (!Connect()) {
		return -1;
	}
	CString sql;
	sql.Format("SELECT id FROM tablemaps WHERE name = '%s'",
		EscapeSqlLiteral(name).GetString());
	PGresult *res = PQexec((PGconn *)_conn, sql.GetString());
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		_last_error.Format("SQL error: %s", PQerrorMessage((PGconn *)_conn));
		if (res) PQclear(res);
		return -1;
	}
	long id = -1;
	if (PQntuples(res) > 0) {
		id = atol(PQgetvalue(res, 0, 0));
	}
	PQclear(res);
	return id;
}

bool CTablemapDB::TablemapExists(const CString name) {
	return GetTablemapId(name) >= 0;
}

bool CTablemapDB::ListTablemaps(std::vector<STablemapDBInfo> *out) {
	if (out == NULL) {
		return false;
	}
	out->clear();
	if (!Connect()) {
		return false;
	}
	PGresult *res = PQexec((PGconn *)_conn,
		"SELECT name, COALESCE(sitename,''), COALESCE(titletext,'') "
		"FROM tablemaps ORDER BY name");
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		_last_error.Format("SQL error: %s", PQerrorMessage((PGconn *)_conn));
		if (res) PQclear(res);
		return false;
	}
	int rows = PQntuples(res);
	for (int i = 0; i < rows; ++i) {
		STablemapDBInfo info;
		info.name = PQgetvalue(res, i, 0);
		info.sitename = PQgetvalue(res, i, 1);
		info.titletext = PQgetvalue(res, i, 2);
		out->push_back(info);
	}
	PQclear(res);
	return true;
}

bool CTablemapDB::DeleteTablemap(const CString name) {
	if (!Connect()) {
		return false;
	}
	CString sql;
	// Children are removed via ON DELETE CASCADE.
	sql.Format("DELETE FROM tablemaps WHERE name = '%s'",
		EscapeSqlLiteral(name).GetString());
	return ExecCommand(sql);
}

//*******************************************************************************
// Settings (the `settings` table; JSON value)
//*******************************************************************************

// Escape a string for inclusion inside a JSON string literal.
static CString EscapeJsonString(const CString &value) {
	CString out;
	for (int i = 0; i < value.GetLength(); ++i) {
		TCHAR c = value[i];
		if (c == '\\') out += "\\\\";
		else if (c == '"') out += "\\\"";
		else out += c;
	}
	return out;
}

CString CTablemapDB::GetSettingString(const CString key, const CString field) {
	CString result;
	if (!Connect()) return result;
	CString sql;
	sql.Format("SELECT value->>'%s' FROM settings WHERE key='%s'",
		EscapeSqlLiteral(field).GetString(), EscapeSqlLiteral(key).GetString());
	PGresult *res = PQexec((PGconn *)_conn, sql.GetString());
	if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0
			&& !PQgetisnull(res, 0, 0)) {
		result = PQgetvalue(res, 0, 0);
	}
	if (res) PQclear(res);
	return result;
}

CString CTablemapDB::GetSettingsRevision() {
	CString result;
	if (!Connect()) return result;
	// GREATEST of the newest settings row and the newest tablemap row. OCR/scrape
	// settings bump settings.updated_at; tablemap + region edits bump tablemaps.updated_at.
	const char *sql =
		"SELECT GREATEST("
		"COALESCE((SELECT max(updated_at) FROM settings), 'epoch'::timestamp),"
		"COALESCE((SELECT max(updated_at) FROM tablemaps), 'epoch'::timestamp))::text";
	PGresult *res = PQexec((PGconn *)_conn, sql);
	if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0
			&& !PQgetisnull(res, 0, 0)) {
		result = PQgetvalue(res, 0, 0);
	}
	if (res) PQclear(res);
	return result;
}

bool CTablemapDB::GetSettingArray(const CString key, const CString field, std::vector<CString> *out) {
	if (out == NULL) return false;
	out->clear();
	if (!Connect()) return false;
	CString sql;
	sql.Format("SELECT jsonb_array_elements_text(value->'%s') FROM settings WHERE key='%s'",
		EscapeSqlLiteral(field).GetString(), EscapeSqlLiteral(key).GetString());
	PGresult *res = PQexec((PGconn *)_conn, sql.GetString());
	bool ok = (PQresultStatus(res) == PGRES_TUPLES_OK);
	if (ok) {
		int rows = PQntuples(res);
		for (int i = 0; i < rows; ++i) {
			out->push_back(CString(PQgetvalue(res, i, 0)));
		}
	}
	if (res) PQclear(res);
	return ok;
}

bool CTablemapDB::SetSettingString(const CString key, const CString field, const CString value) {
	if (!Connect()) return false;
	CString sql;
	sql.Format(
		"INSERT INTO settings(key,value,updated_at) "
		"VALUES ('%s', jsonb_build_object('%s', to_jsonb('%s'::text)), now()) "
		"ON CONFLICT (key) DO UPDATE SET "
		"value = jsonb_set(COALESCE(settings.value,'{}'::jsonb), '{%s}', to_jsonb('%s'::text)), "
		"updated_at = now()",
		EscapeSqlLiteral(key).GetString(),
		EscapeSqlLiteral(field).GetString(), EscapeSqlLiteral(value).GetString(),
		EscapeSqlLiteral(field).GetString(), EscapeSqlLiteral(value).GetString());
	return ExecCommand(sql);
}

bool CTablemapDB::SetSettingArray(const CString key, const CString field, const std::vector<CString> &values) {
	if (!Connect()) return false;
	// Build a JSON array literal: ["a","b",...]
	CString arr = "[";
	for (size_t i = 0; i < values.size(); ++i) {
		if (i > 0) arr += ",";
		arr += "\"";
		arr += EscapeJsonString(values[i]);
		arr += "\"";
	}
	arr += "]";
	CString sql;
	sql.Format(
		"INSERT INTO settings(key,value,updated_at) "
		"VALUES ('%s', jsonb_build_object('%s', '%s'::jsonb), now()) "
		"ON CONFLICT (key) DO UPDATE SET "
		"value = jsonb_set(COALESCE(settings.value,'{}'::jsonb), '{%s}', '%s'::jsonb), "
		"updated_at = now()",
		EscapeSqlLiteral(key).GetString(),
		EscapeSqlLiteral(field).GetString(), EscapeSqlLiteral(arr).GetString(),
		EscapeSqlLiteral(field).GetString(), EscapeSqlLiteral(arr).GetString());
	return ExecCommand(sql);
}

//*******************************************************************************
// Load
//*******************************************************************************

int CTablemapDB::LoadTablemapFromDB(const CString name, CTablemap *tm) {
	if (tm == NULL) {
		return ERR_SYNTAX;
	}
	if (!Connect()) {
		return ERR_NOTMASTER;
	}
	long id = GetTablemapId(name);
	if (id < 0) {
		_last_error.Format("Tablemap '%s' not found in the hiss database.", name.GetString());
		return ERR_EOF;
	}

	tm->ClearTablemap();

	CString sql;
	PGresult *res = NULL;
	int i = 0, n = 0;

	// --- sizes (z$) ---
	sql.Format("SELECT name, width, height FROM tm_sizes WHERE tablemap_id=%ld", id);
	res = PQexec((PGconn *)_conn, sql.GetString());
	if (PQresultStatus(res) == PGRES_TUPLES_OK) {
		n = PQntuples(res);
		for (i = 0; i < n; ++i) {
			STablemapSize s;
			s.name = PQgetvalue(res, i, 0);
			s.width = atoi(PQgetvalue(res, i, 1));
			s.height = atoi(PQgetvalue(res, i, 2));
			tm->z$_insert(s);
		}
	}
	if (res) PQclear(res);

	// --- symbols (s$) ---
	sql.Format("SELECT name, text FROM tm_symbols WHERE tablemap_id=%ld", id);
	res = PQexec((PGconn *)_conn, sql.GetString());
	if (PQresultStatus(res) == PGRES_TUPLES_OK) {
		n = PQntuples(res);
		for (i = 0; i < n; ++i) {
			STablemapSymbol s;
			s.name = PQgetvalue(res, i, 0);
			s.text = PQgetvalue(res, i, 1);
			tm->s$_insert(s);
		}
	}
	if (res) PQclear(res);

	// --- regions (r$) ---
	sql.Format("SELECT name, rgn_left, rgn_top, rgn_right, rgn_bottom, color, radius,"
		" transform, use_default, threshold, use_cropping, crop_size, match_mode, sharpen,"
		" COALESCE(color2,0), COALESCE(color2_enabled,false)"
		" FROM tm_regions WHERE tablemap_id=%ld", id);
	res = PQexec((PGconn *)_conn, sql.GetString());
	if (PQresultStatus(res) == PGRES_TUPLES_OK) {
		n = PQntuples(res);
		for (i = 0; i < n; ++i) {
			STablemapRegion r;
			r.name = PQgetvalue(res, i, 0);
			r.left = (unsigned int)atol(PQgetvalue(res, i, 1));
			r.top = (unsigned int)atol(PQgetvalue(res, i, 2));
			r.right = (unsigned int)atol(PQgetvalue(res, i, 3));
			r.bottom = (unsigned int)atol(PQgetvalue(res, i, 4));
			r.color = (COLORREF)strtoul(PQgetvalue(res, i, 5), 0, 10);
			r.radius = atoi(PQgetvalue(res, i, 6));
			r.transform = PQgetvalue(res, i, 7);
			r.cur_bmp = NULL;
			r.last_bmp = NULL;
			r.use_default = (strcmp(PQgetvalue(res, i, 8), "t") == 0);
			r.threshold = atoi(PQgetvalue(res, i, 9));
			r.use_cropping = (strcmp(PQgetvalue(res, i, 10), "t") == 0);
			r.crop_size = atoi(PQgetvalue(res, i, 11));
			r.match_mode = atoi(PQgetvalue(res, i, 12));
			r.sharpen = atoi(PQgetvalue(res, i, 13));
			r.color2 = (COLORREF)strtoul(PQgetvalue(res, i, 14), 0, 10);
			r.color2_enabled = (strcmp(PQgetvalue(res, i, 15), "t") == 0);
			tm->r$_insert(r);
		}
	}
	if (res) PQclear(res);

	// --- fonts (t$) ---
	sql.Format("SELECT font_group, ch, hexmash, x_values FROM tm_fonts WHERE tablemap_id=%ld", id);
	res = PQexec((PGconn *)_conn, sql.GetString());
	if (PQresultStatus(res) == PGRES_TUPLES_OK) {
		n = PQntuples(res);
		for (i = 0; i < n; ++i) {
			STablemapFont f;
			int font_group = atoi(PQgetvalue(res, i, 0));
			CString ch = PQgetvalue(res, i, 1);
			f.ch = ch.IsEmpty() ? ' ' : ch.GetAt(0);
			f.hexmash = PQgetvalue(res, i, 2);
			CString xvals = PQgetvalue(res, i, 3);
			int pos = 0, count = 0;
			CString token = xvals.Tokenize(" \t", pos);
			while (!token.IsEmpty() && count < MAX_SINGLE_CHAR_WIDTH) {
				f.x[count++] = strtoul(token, 0, 16);
				token = (pos >= 0) ? xvals.Tokenize(" \t", pos) : CString("");
			}
			f.x_count = count;
			tm->t$_insert(font_group, f);
		}
	}
	if (res) PQclear(res);

	// --- hash points (p$) ---
	sql.Format("SELECT hash_group, x, y FROM tm_hash_points WHERE tablemap_id=%ld", id);
	res = PQexec((PGconn *)_conn, sql.GetString());
	if (PQresultStatus(res) == PGRES_TUPLES_OK) {
		n = PQntuples(res);
		for (i = 0; i < n; ++i) {
			STablemapHashPoint p;
			int hash_group = atoi(PQgetvalue(res, i, 0));
			p.x = atoi(PQgetvalue(res, i, 1));
			p.y = atoi(PQgetvalue(res, i, 2));
			tm->p$_insert(hash_group, p);
		}
	}
	if (res) PQclear(res);

	// --- hash values (h$) ---
	sql.Format("SELECT hash_group, name, hash FROM tm_hash_values WHERE tablemap_id=%ld", id);
	res = PQexec((PGconn *)_conn, sql.GetString());
	if (PQresultStatus(res) == PGRES_TUPLES_OK) {
		n = PQntuples(res);
		for (i = 0; i < n; ++i) {
			STablemapHashValue h;
			int hash_group = atoi(PQgetvalue(res, i, 0));
			h.name = PQgetvalue(res, i, 1);
			h.hash = (uint32_t)strtoul(PQgetvalue(res, i, 2), 0, 10);
			tm->h$_insert(hash_group, h);
		}
	}
	if (res) PQclear(res);

	// --- images (i$) ---
	sql.Format("SELECT name, width, height, pixels FROM tm_images WHERE tablemap_id=%ld", id);
	res = PQexec((PGconn *)_conn, sql.GetString());
	if (PQresultStatus(res) == PGRES_TUPLES_OK) {
		n = PQntuples(res);
		for (i = 0; i < n; ++i) {
			STablemapImage img;
			img.name = PQgetvalue(res, i, 0);
			img.width = atoi(PQgetvalue(res, i, 1));
			img.height = atoi(PQgetvalue(res, i, 2));
			CString t = img.name + ".ppm";
			img.image = new RGBAImage(img.width, img.height, t.GetString());
			std::vector<CString> rows;
			SplitLines(PQgetvalue(res, i, 3), &rows);
			for (int y = 0; y < img.height; ++y) {
				CString row = (y < (int)rows.size()) ? rows[y] : CString("");
				for (int x = 0; x < img.width; ++x) {
					CString hexval = row.Mid(x * 8, 8);
					uint32_t pix = strtoul(hexval, 0, 16);
					img.pixel[y * img.width + x] = pix;
					BYTE alpha = (pix >> 24) & 0xff;
					BYTE blue = (pix >> 16) & 0xff;
					BYTE green = (pix >> 8) & 0xff;
					BYTE red = (pix >> 0) & 0xff;
					img.image->Set(red, green, blue, alpha, y * img.width + x);
				}
			}
			tm->i$_insert(img);
		}
	}
	if (res) PQclear(res);

	// --- templates (tpl$) ---
	sql.Format("SELECT name, rgn_left, rgn_top, rgn_right, rgn_bottom, width, height,"
		" use_default, match_mode, created, pixels FROM tm_templates WHERE tablemap_id=%ld", id);
	res = PQexec((PGconn *)_conn, sql.GetString());
	if (PQresultStatus(res) == PGRES_TUPLES_OK) {
		n = PQntuples(res);
		for (i = 0; i < n; ++i) {
			STablemapTemplate tpl;
			tpl.name = PQgetvalue(res, i, 0);
			tpl.left = (unsigned int)atol(PQgetvalue(res, i, 1));
			tpl.top = (unsigned int)atol(PQgetvalue(res, i, 2));
			tpl.right = (unsigned int)atol(PQgetvalue(res, i, 3));
			tpl.bottom = (unsigned int)atol(PQgetvalue(res, i, 4));
			tpl.width = atoi(PQgetvalue(res, i, 5));
			tpl.height = atoi(PQgetvalue(res, i, 6));
			tpl.use_default = (strcmp(PQgetvalue(res, i, 7), "t") == 0);
			tpl.match_mode = (unsigned int)atol(PQgetvalue(res, i, 8));
			tpl.created = (strcmp(PQgetvalue(res, i, 9), "t") == 0);
			CString t = tpl.name + ".ppm";
			tpl.image = new RGBAImage(tpl.width, tpl.height, t.GetString());
			std::vector<CString> rows;
			SplitLines(PQgetvalue(res, i, 10), &rows);
			for (int y = 0; y < tpl.height; ++y) {
				CString row = (y < (int)rows.size()) ? rows[y] : CString("");
				for (int x = 0; x < tpl.width; ++x) {
					CString hexval = row.Mid(x * 8, 8);
					uint32_t pix = strtoul(hexval, 0, 16);
					tpl.pixel[y * tpl.width + x] = pix;
					BYTE alpha = (pix >> 24) & 0xff;
					BYTE blue = (pix >> 16) & 0xff;
					BYTE green = (pix >> 8) & 0xff;
					BYTE red = (pix >> 0) & 0xff;
					tpl.image->Set(red, green, blue, alpha, y * tpl.width + x);
				}
			}
			tm->tpl$_insert(tpl);
		}
	}
	if (res) PQclear(res);

	tm->InitNChairs();
	tm->_valid = true;
	tm->_filename = name;
	tm->_filepath = name;
	return SUCCESS;
}

//*******************************************************************************
// Save
//*******************************************************************************

int CTablemapDB::SaveTablemapToDB(const CString name, CTablemap *tm) {
	if (tm == NULL) {
		return ERR_SYNTAX;
	}
	if (!Connect()) {
		return ERR_NOTMASTER;
	}
	if (!ExecCommand("BEGIN")) {
		return ERR_SYNTAX;
	}

	CString sql;

	// Upsert the parent row.
	sql.Format("INSERT INTO tablemaps (name, sitename, titletext, source_file, bits_per_pixel, updated_at) "
		"VALUES ('%s','%s','%s','%s',32, now()) "
		"ON CONFLICT (name) DO UPDATE SET sitename=EXCLUDED.sitename,"
		" titletext=EXCLUDED.titletext, source_file=EXCLUDED.source_file, updated_at=now()",
		EscapeSqlLiteral(name).GetString(),
		EscapeSqlLiteral(tm->sitename()).GetString(),
		EscapeSqlLiteral(tm->titletext()).GetString(),
		EscapeSqlLiteral(tm->filepath()).GetString());
	if (!ExecCommand(sql)) { ExecCommand("ROLLBACK"); return ERR_SYNTAX; }

	long id = GetTablemapId(name);
	if (id < 0) { ExecCommand("ROLLBACK"); return ERR_SYNTAX; }

	// Wipe existing children so a re-save is a clean replace.
	const char *children[] = { "tm_sizes", "tm_symbols", "tm_regions", "tm_fonts",
		"tm_hash_points", "tm_hash_values", "tm_images", "tm_templates" };
	for (int k = 0; k < 8; ++k) {
		sql.Format("DELETE FROM %s WHERE tablemap_id=%ld", children[k], id);
		if (!ExecCommand(sql)) { ExecCommand("ROLLBACK"); return ERR_SYNTAX; }
	}

	// sizes (z$)
	{
		ZMap *z = tm->z$();
		for (ZMapCI it = z->begin(); it != z->end(); ++it) {
			sql.Format("INSERT INTO tm_sizes (tablemap_id,name,width,height) VALUES (%ld,'%s',%d,%d)",
				id, EscapeSqlLiteral(it->second.name).GetString(),
				it->second.width, it->second.height);
			if (!ExecCommand(sql)) { ExecCommand("ROLLBACK"); return ERR_SYNTAX; }
		}
	}

	// symbols (s$)
	{
		SMap *s = tm->s$();
		for (SMapCI it = s->begin(); it != s->end(); ++it) {
			sql.Format("INSERT INTO tm_symbols (tablemap_id,name,text) VALUES (%ld,'%s','%s')",
				id, EscapeSqlLiteral(it->second.name).GetString(),
				EscapeSqlLiteral(it->second.text).GetString());
			if (!ExecCommand(sql)) { ExecCommand("ROLLBACK"); return ERR_SYNTAX; }
		}
	}

	// regions (r$)
	{
		const RMap *r = tm->r$();
		for (RMapCI it = r->begin(); it != r->end(); ++it) {
			const STablemapRegion &g = it->second;
			sql.Format("INSERT INTO tm_regions (tablemap_id,name,rgn_left,rgn_top,rgn_right,rgn_bottom,"
				"color,radius,transform,use_default,threshold,use_cropping,crop_size,match_mode,sharpen,"
				"color2,color2_enabled) "
				"VALUES (%ld,'%s',%u,%u,%u,%u,%lu,%d,'%s',%s,%d,%s,%d,%d,%d,%lu,%s)",
				id, EscapeSqlLiteral(g.name).GetString(),
				g.left, g.top, g.right, g.bottom,
				(unsigned long)g.color, g.radius, EscapeSqlLiteral(g.transform).GetString(),
				g.use_default ? "true" : "false", g.threshold,
				g.use_cropping ? "true" : "false", g.crop_size, g.match_mode, g.sharpen,
				(unsigned long)g.color2, g.color2_enabled ? "true" : "false");
			if (!ExecCommand(sql)) { ExecCommand("ROLLBACK"); return ERR_SYNTAX; }
		}
	}

	// fonts (t$)
	for (int grp = 0; grp < k_max_number_of_font_groups_in_tablemap; ++grp) {
		TMap *t = tm->t$(grp);
		if (t == NULL) continue;
		for (TMapCI it = t->begin(); it != t->end(); ++it) {
			const STablemapFont &f = it->second;
			CString xvals, token;
			for (int j = 0; j < f.x_count; ++j) {
				token.Format("%x", f.x[j]);
				if (j > 0) xvals += " ";
				xvals += token;
			}
			CString ch;
			ch += f.ch;
			sql.Format("INSERT INTO tm_fonts (tablemap_id,font_group,ch,hexmash,x_values) "
				"VALUES (%ld,%d,'%s','%s','%s')",
				id, grp, EscapeSqlLiteral(ch).GetString(),
				EscapeSqlLiteral(f.hexmash).GetString(), EscapeSqlLiteral(xvals).GetString());
			if (!ExecCommand(sql)) { ExecCommand("ROLLBACK"); return ERR_SYNTAX; }
		}
	}

	// hash points (p$)
	for (int grp = 0; grp < k_max_number_of_hash_groups_in_tablemap; ++grp) {
		PMap *p = tm->p$(grp);
		if (p == NULL) continue;
		for (PMapCI it = p->begin(); it != p->end(); ++it) {
			sql.Format("INSERT INTO tm_hash_points (tablemap_id,hash_group,x,y) VALUES (%ld,%d,%d,%d)",
				id, grp, it->second.x, it->second.y);
			if (!ExecCommand(sql)) { ExecCommand("ROLLBACK"); return ERR_SYNTAX; }
		}
	}

	// hash values (h$)
	for (int grp = 0; grp < k_max_number_of_hash_groups_in_tablemap; ++grp) {
		HMap *h = tm->h$(grp);
		if (h == NULL) continue;
		for (HMapCI it = h->begin(); it != h->end(); ++it) {
			sql.Format("INSERT INTO tm_hash_values (tablemap_id,hash_group,name,hash) VALUES (%ld,%d,'%s',%lu)",
				id, grp, EscapeSqlLiteral(it->second.name).GetString(),
				(unsigned long)it->second.hash);
			if (!ExecCommand(sql)) { ExecCommand("ROLLBACK"); return ERR_SYNTAX; }
		}
	}

	// images (i$)
	{
		IMap *im = tm->i$();
		for (IMapCI it = im->begin(); it != im->end(); ++it) {
			const STablemapImage &g = it->second;
			CString pixels, row, px;
			for (int y = 0; y < g.height; ++y) {
				row = "";
				for (int x = 0; x < g.width; ++x) {
					px.Format("%08x", g.pixel[y * g.width + x]);
					row += px;
				}
				pixels += row;
				if (y < g.height - 1) pixels += "\n";
			}
			sql.Format("INSERT INTO tm_images (tablemap_id,name,width,height,pixels) VALUES (%ld,'%s',%d,%d,'%s')",
				id, EscapeSqlLiteral(g.name).GetString(), g.width, g.height,
				EscapeSqlLiteral(pixels).GetString());
			if (!ExecCommand(sql)) { ExecCommand("ROLLBACK"); return ERR_SYNTAX; }
		}
	}

	// templates (tpl$)
	{
		const TPLMap *tpl = tm->tpl$();
		for (TPLMapCI it = tpl->begin(); it != tpl->end(); ++it) {
			const STablemapTemplate &g = it->second;
			CString pixels, row, px;
			for (int y = 0; y < g.height; ++y) {
				row = "";
				for (int x = 0; x < g.width; ++x) {
					px.Format("%08x", g.pixel[y * g.width + x]);
					row += px;
				}
				pixels += row;
				if (y < g.height - 1) pixels += "\n";
			}
			sql.Format("INSERT INTO tm_templates (tablemap_id,name,rgn_left,rgn_top,rgn_right,rgn_bottom,"
				"width,height,use_default,match_mode,created,pixels) "
				"VALUES (%ld,'%s',%u,%u,%u,%u,%d,%d,%s,%u,%s,'%s')",
				id, EscapeSqlLiteral(g.name).GetString(),
				g.left, g.top, g.right, g.bottom, g.width, g.height,
				g.use_default ? "true" : "false", g.match_mode,
				g.created ? "true" : "false", EscapeSqlLiteral(pixels).GetString());
			if (!ExecCommand(sql)) { ExecCommand("ROLLBACK"); return ERR_SYNTAX; }
		}
	}

	if (!ExecCommand("COMMIT")) { ExecCommand("ROLLBACK"); return ERR_SYNTAX; }
	return SUCCESS;
}

//*******************************************************************************
// Import from legacy *.tm files
//*******************************************************************************

CString CTablemapDB::NameFromPath(const CString path) {
	int slash = path.ReverseFind('\\');
	int fslash = path.ReverseFind('/');
	int cut = (slash > fslash) ? slash : fslash;
	CString base = (cut >= 0) ? path.Mid(cut + 1) : path;
	int dot = base.ReverseFind('.');
	if (dot > 0) {
		base = base.Left(dot);
	}
	return base;
}

bool CTablemapDB::ImportTmFileToDB(const CString path, CString *out_name) {
	CTablemap temp;
	int ret = temp.LoadTablemap(path);
	if (ret != SUCCESS) {
		_last_error.Format("Failed to parse tablemap (code %d):\n%s", ret, path.GetString());
		return false;
	}
	CString name = NameFromPath(path);
	int sret = SaveTablemapToDB(name, &temp);
	if (sret != SUCCESS) {
		return false;
	}
	if (out_name != NULL) {
		*out_name = name;
	}
	return true;
}

// Recursive helper for ImportFolderToDB. `used` disambiguates same-stem files
// imported in the same run (a later file would otherwise overwrite an earlier
// one with the same name).
static void ImportFolderRecursive(CTablemapDB *db, const CString folder,
		std::set<CString> *used, int *imported, int *failed, CString *report) {
	CString wildcard;
	wildcard.Format("%s\\*", folder.GetString());
	CFileFind finder;
	BOOL found = finder.FindFile(wildcard);
	while (found) {
		found = finder.FindNextFile();
		if (finder.IsDots()) {
			continue;
		}
		if (finder.IsDirectory()) {
			ImportFolderRecursive(db, finder.GetFilePath(), used, imported, failed, report);
			continue;
		}
		CString path = finder.GetFilePath();
		if (path.Right(3).MakeLower() != ".tm") {
			continue;
		}
		CString name;
		bool ok = db->ImportTmFileToDB(path, &name);
		if (ok) {
			// Disambiguate duplicate stems within this run.
			if (used->find(name) != used->end()) {
				CString unique;
				int suffix = 2;
				do {
					unique.Format("%s__%d", name.GetString(), suffix++);
				} while (used->find(unique) != used->end());
				// Re-save under the unique name and drop the clobbered row.
				CTablemap temp;
				if (temp.LoadTablemap(path) == SUCCESS) {
					db->SaveTablemapToDB(unique, &temp);
					name = unique;
				}
			}
			used->insert(name);
			if (imported) (*imported)++;
			if (report) { report->AppendFormat("OK   %s  ->  %s\r\n", path.GetString(), name.GetString()); }
		} else {
			if (failed) (*failed)++;
			if (report) { report->AppendFormat("FAIL %s  (%s)\r\n", path.GetString(), db->LastError().GetString()); }
		}
	}
}

int CTablemapDB::ImportFolderToDB(const CString folder, int *imported, int *failed, CString *report) {
	if (imported) *imported = 0;
	if (failed) *failed = 0;
	if (report) *report = "";
	if (!Connect()) {
		return ERR_NOTMASTER;
	}
	std::set<CString> used;
	ImportFolderRecursive(this, folder, &used, imported, failed, report);
	return SUCCESS;
}
