#include "stdafx.h"
#include "TrainerDB.h"

#include "libpq-fe.h"

// Bootstrap value for the hiss-DB connection. No registry / INI: a hardcoded localhost
// default, overridable only via the standard PG* environment variables (matches CTablemapDB).
static CString EnvOrDefault(const char *name, const char *def)
{
	char buf[512] = { 0 };
	size_t n = 0;
	if (getenv_s(&n, buf, sizeof(buf), name) == 0 && n > 0 && buf[0] != '\0') {
		return CString(buf);
	}
	return CString(def);
}

pconn *TrainerDB_Connect()
{
	CString conn;
	conn.Format("host=%s port=%s user=%s password='%s' dbname='%s'",
		EnvOrDefault("PGHOST", "127.0.0.1").GetString(),
		EnvOrDefault("PGPORT", "5432").GetString(),
		EnvOrDefault("PGUSER", "postgres").GetString(),
		EnvOrDefault("PGPASSWORD", "dbpass").GetString(),
		EnvOrDefault("PGDATABASE", "hiss").GetString());
	PGconn *c = PQconnectdb(conn.GetString());
	if (c == NULL || PQstatus(c) != CONNECTION_OK) {
		if (c) PQfinish(c);
		return NULL;
	}
	return c;
}

bool TrainerDB_ListTablemaps(std::vector<CString> *names)
{
	if (names == NULL) {
		return false;
	}
	names->clear();
	PGconn *conn = TrainerDB_Connect();
	if (conn == NULL) {
		return false;
	}
	PGresult *res = PQexec(conn, "SELECT name FROM tablemaps ORDER BY name");
	bool ok = (PQresultStatus(res) == PGRES_TUPLES_OK);
	if (ok) {
		int rows = PQntuples(res);
		for (int i = 0; i < rows; ++i) {
			names->push_back(CString(PQgetvalue(res, i, 0)));
		}
	}
	if (res) PQclear(res);
	PQfinish(conn);
	return ok;
}

// Doubles single quotes for safe inclusion in a SQL literal.
static CString EscapeSqlLiteral(const CString &s)
{
	CString out;
	for (int i = 0; i < s.GetLength(); ++i) {
		TCHAR c = s[i];
		if (c == '\'') out += "''"; else out += c;
	}
	return out;
}

CString TrainerDB_GetSetting(const CString &key, const CString &field)
{
	CString result;
	PGconn *conn = TrainerDB_Connect();
	if (conn == NULL) {
		return result;
	}
	CString sql;
	sql.Format("SELECT value->>'%s' FROM settings WHERE key='%s'",
		EscapeSqlLiteral(field).GetString(), EscapeSqlLiteral(key).GetString());
	PGresult *res = PQexec(conn, sql.GetString());
	if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0 && !PQgetisnull(res, 0, 0)) {
		result = PQgetvalue(res, 0, 0);
	}
	if (res) PQclear(res);
	PQfinish(conn);
	return result;
}

bool TrainerDB_SetSetting(const CString &key, const CString &field, const CString &value)
{
	PGconn *conn = TrainerDB_Connect();
	if (conn == NULL) {
		return false;
	}
	// Upsert: create the row with this single field, or merge the field into the
	// existing JSON value (mirrors CTablemapDB::SetSettingString).
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
	PGresult *res = PQexec(conn, sql.GetString());
	bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
	if (res) PQclear(res);
	PQfinish(conn);
	return ok;
}

bool TrainerDB_SetSettingArray(const CString &key, const CString &field, const std::vector<CString> &values)
{
	PGconn *conn = TrainerDB_Connect();
	if (conn == NULL) {
		return false;
	}
	// Build a JSON array literal: ["a","b",...]. Escape backslash and quote for JSON,
	// then EscapeSqlLiteral the whole thing for the SQL string literal.
	CString arr = "[";
	for (size_t i = 0; i < values.size(); ++i) {
		if (i > 0) arr += ",";
		CString v;
		for (int j = 0; j < values[i].GetLength(); ++j) {
			TCHAR c = values[i][j];
			if (c == '\\' || c == '"') { v += '\\'; v += c; }
			else v += c;
		}
		arr += "\""; arr += v; arr += "\"";
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
	PGresult *res = PQexec(conn, sql.GetString());
	bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
	if (res) PQclear(res);
	PQfinish(conn);
	return ok;
}

static const char *kWindowSettingsKey = "trainer_windows";

void TrainerDB_SaveWindowRect(HWND hwnd, const char *field)
{
	if (hwnd == NULL || !::IsWindow(hwnd) || field == NULL) {
		return;
	}
	RECT r;
	if (!::GetWindowRect(hwnd, &r)) {
		return;
	}
	CString v;
	v.Format("%d,%d,%d,%d", (int)r.left, (int)r.top, (int)(r.right - r.left), (int)(r.bottom - r.top));
	TrainerDB_SetSetting(CString(kWindowSettingsKey), CString(field), v);
}

bool TrainerDB_RestoreWindowRect(HWND hwnd, const char *field)
{
	if (hwnd == NULL || !::IsWindow(hwnd) || field == NULL) {
		return false;
	}
	CString v = TrainerDB_GetSetting(CString(kWindowSettingsKey), CString(field));
	if (v.IsEmpty()) {
		return false;
	}
	int parts[4] = { 0, 0, 0, 0 };
	int n = 0, pos = 0;
	while (n < 4) {
		int comma = v.Find(',', pos);
		CString tok = (comma < 0) ? v.Mid(pos) : v.Mid(pos, comma - pos);
		parts[n++] = atoi(CStringA(tok));
		if (comma < 0) break;
		pos = comma + 1;
	}
	if (n != 4) {
		return false;
	}
	int x = parts[0], y = parts[1], w = parts[2], h = parts[3];
	if (w < 50 || h < 50) {
		return false;   // sanity: never restore a collapsed window
	}
	// Drop placements that no longer fall on any monitor (e.g. an unplugged screen).
	RECT want = { x, y, x + w, y + h };
	if (MonitorFromRect(&want, MONITOR_DEFAULTTONULL) == NULL) {
		return false;
	}
	::SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
	return true;
}

long TrainerDB_TablemapId(pconn *conn, const CString &name)
{
	if (conn == NULL) {
		return -1;
	}
	// Escape single quotes for the SQL literal.
	CString esc;
	for (int i = 0; i < name.GetLength(); ++i) {
		TCHAR c = name[i];
		if (c == '\'') esc += "''"; else esc += c;
	}
	CString sql;
	sql.Format("SELECT id FROM tablemaps WHERE name='%s'", esc.GetString());
	PGresult *res = PQexec(conn, sql.GetString());
	long id = -1;
	if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
		id = atol(PQgetvalue(res, 0, 0));
	}
	if (res) PQclear(res);
	return id;
}

void TrainerDB_TouchTablemap(pconn *conn, long id)
{
	if (conn == NULL || id < 0) {
		return;
	}
	CString sql;
	sql.Format("UPDATE tablemaps SET updated_at=now() WHERE id=%ld", id);
	PGresult *res = PQexec(conn, sql.GetString());
	if (res) PQclear(res);
}
