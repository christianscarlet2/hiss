#include "stdafx.h"
#include "TrainerDB.h"

#include "libpq-fe.h"

static const char *kTrainerRegKey = "Software\\Hiss\\Trainer";

// Reads the optional "hiss_conn" registry override (HKCU\Software\Hiss\Trainer).
static CString ReadConnOverride()
{
	CString out;
	HKEY key;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, kTrainerRegKey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
		char buf[1024] = { 0 };
		DWORD size = sizeof(buf), type = 0;
		if (RegQueryValueEx(key, "hiss_conn", NULL, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS
				&& type == REG_SZ) {
			out = buf;
		}
		RegCloseKey(key);
	}
	return out;
}

pconn *TrainerDB_Connect()
{
	CString conn = ReadConnOverride();
	if (conn.IsEmpty()) {
		conn = "host=127.0.0.1 port=5432 user=postgres password='dbpass' dbname='hiss'";
	}
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
