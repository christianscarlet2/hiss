#include "stdafx.h"
#include "TablemapRegions.h"
#include "TrainerDB.h"
#include "libpq-fe.h"
#include <map>

// True for region names p0balance .. p8balance (exactly that shape).
static bool IsBalanceRegionName(const CString &name)
{
	// p<digit>balance, digit 0..8
	if (name.GetLength() < 9) {
		return false;
	}
	if (name[0] != 'p') {
		return false;
	}
	if (name[1] < '0' || name[1] > '8') {
		return false;
	}
	CString rest = name.Mid(2);
	return rest.CompareNoCase("balance") == 0;
}

bool LoadBalanceRegions(const CString &tm_path, std::vector<STrainerRegion> *out)
{
	if (out == NULL) {
		return false;
	}
	out->clear();

	CStdioFile file;
	if (!file.Open(tm_path, CFile::modeRead | CFile::typeText)) {
		return false;
	}

	CString line;
	while (file.ReadString(line)) {
		// Region lines start with "r$".
		if (line.Left(2) != "r$") {
			continue;
		}
		// Strip the "r$" prefix, then tokenize on whitespace.
		CString body = line.Mid(2);
		int pos = 0;
		CString name = body.Tokenize(" \t", pos);
		CString sleft = body.Tokenize(" \t", pos);
		CString stop = body.Tokenize(" \t", pos);
		CString sright = body.Tokenize(" \t", pos);
		CString sbottom = body.Tokenize(" \t", pos);
		if (name.IsEmpty() || sleft.IsEmpty() || stop.IsEmpty()
			|| sright.IsEmpty() || sbottom.IsEmpty()) {
			continue;
		}
		if (!IsBalanceRegionName(name)) {
			continue;
		}
		// Optional trailing fields: colour, radius, transform.
		CString scolor = (pos >= 0) ? body.Tokenize(" \t", pos) : CString("");
		CString sradius = (pos >= 0) ? body.Tokenize(" \t", pos) : CString("");
		CString stransform = (pos >= 0) ? body.Tokenize(" \t", pos) : CString("");

		STrainerRegion region;
		region.name = name;
		region.rect.left = atol(sleft.GetString());
		region.rect.top = atol(stop.GetString());
		region.rect.right = atol(sright.GetString());
		region.rect.bottom = atol(sbottom.GetString());
		region.color = (COLORREF)strtoul(CStringA(scolor).GetString(), NULL, 16);
		region.radius = sradius.IsEmpty() ? 0 : atol(sradius.GetString());
		region.transform = stransform;
		out->push_back(region);
	}
	file.Close();
	return true;
}

// Escape single quotes for a SQL string literal.
static CString SqlEsc(const CString &s)
{
	CString out;
	for (int i = 0; i < s.GetLength(); ++i) {
		TCHAR c = s[i];
		if (c == '\'') out += "''"; else out += c;
	}
	return out;
}

bool LoadBalanceRegionsFromDB(const CString &tm_name, std::vector<STrainerRegion> *out)
{
	if (out == NULL) {
		return false;
	}
	out->clear();
	PGconn *conn = TrainerDB_Connect();
	if (conn == NULL) {
		return false;
	}
	long id = TrainerDB_TablemapId(conn, tm_name);
	if (id < 0) {
		PQfinish(conn);
		return false;
	}
	CString sql;
	sql.Format("SELECT name, rgn_left, rgn_top, rgn_right, rgn_bottom, color, radius,"
		" COALESCE(transform,'') FROM tm_regions WHERE tablemap_id=%ld", id);
	PGresult *res = PQexec(conn, sql.GetString());
	bool ok = (PQresultStatus(res) == PGRES_TUPLES_OK);
	if (ok) {
		int rows = PQntuples(res);
		for (int i = 0; i < rows; ++i) {
			CString name = PQgetvalue(res, i, 0);
			if (!IsBalanceRegionName(name)) {
				continue;
			}
			STrainerRegion region;
			region.name = name;
			region.rect.left = atol(PQgetvalue(res, i, 1));
			region.rect.top = atol(PQgetvalue(res, i, 2));
			region.rect.right = atol(PQgetvalue(res, i, 3));
			region.rect.bottom = atol(PQgetvalue(res, i, 4));
			region.color = (COLORREF)strtoul(PQgetvalue(res, i, 5), NULL, 10);
			region.radius = atol(PQgetvalue(res, i, 6));
			region.transform = PQgetvalue(res, i, 7);
			out->push_back(region);
		}
	}
	if (res) PQclear(res);
	PQfinish(conn);
	return ok;
}

// Persists a region's colour/radius back to the database. The first argument is
// the tablemap NAME (DB key), not a file path.
bool SaveRegionColorRadius(const CString &tm_name, const CString &name, COLORREF color, int radius)
{
	if (tm_name.IsEmpty() || name.IsEmpty()) {
		return false;
	}
	PGconn *conn = TrainerDB_Connect();
	if (conn == NULL) {
		return false;
	}
	long id = TrainerDB_TablemapId(conn, tm_name);
	if (id < 0) {
		PQfinish(conn);
		return false;
	}
	CString sql;
	sql.Format("UPDATE tm_regions SET color=%lu, radius=%d WHERE tablemap_id=%ld AND name='%s'",
		(unsigned long)color, radius, id, SqlEsc(name).GetString());
	PGresult *res = PQexec(conn, sql.GetString());
	bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
	if (res) PQclear(res);
	PQfinish(conn);
	return ok;
}

// ---------------------------------------------------------------------------
// Thread-safe region colour cache
// ---------------------------------------------------------------------------
namespace {
	struct SRegionColor { COLORREF color; int radius; CString transform; };

	CRITICAL_SECTION g_rc_cs;
	bool g_rc_init = false;
	std::map<CString, SRegionColor> g_rc;   // keyed by region name
	CString g_rc_tm_path;

	void RC_EnsureInit() {
		if (!g_rc_init) { InitializeCriticalSection(&g_rc_cs); g_rc_init = true; }
	}
}

void RegionColors_Reset()
{
	RC_EnsureInit();
	EnterCriticalSection(&g_rc_cs);
	g_rc.clear();
	LeaveCriticalSection(&g_rc_cs);
}

void RegionColors_SetTmPath(const CString &tm_path)
{
	RC_EnsureInit();
	EnterCriticalSection(&g_rc_cs);
	g_rc_tm_path = tm_path;
	LeaveCriticalSection(&g_rc_cs);
}

void RegionColors_Add(const CString &name, COLORREF color, int radius, const CString &transform)
{
	RC_EnsureInit();
	EnterCriticalSection(&g_rc_cs);
	SRegionColor rc; rc.color = color; rc.radius = radius; rc.transform = transform;
	g_rc[name] = rc;
	LeaveCriticalSection(&g_rc_cs);
}

bool RegionColors_GetByName(const CString &name, COLORREF *color, int *radius)
{
	RC_EnsureInit();
	bool found = false;
	EnterCriticalSection(&g_rc_cs);
	std::map<CString, SRegionColor>::const_iterator it = g_rc.find(name);
	if (it != g_rc.end()) {
		if (color) *color = it->second.color;
		if (radius) *radius = it->second.radius;
		found = true;
	}
	LeaveCriticalSection(&g_rc_cs);
	return found;
}

bool RegionColors_GetByTransform(const CString &transform, COLORREF *color, int *radius)
{
	RC_EnsureInit();
	bool found = false;
	EnterCriticalSection(&g_rc_cs);
	for (std::map<CString, SRegionColor>::const_iterator it = g_rc.begin(); it != g_rc.end(); ++it) {
		if (it->second.transform.CompareNoCase(transform) == 0) {
			if (color) *color = it->second.color;
			if (radius) *radius = it->second.radius;
			found = true;
			break;
		}
	}
	LeaveCriticalSection(&g_rc_cs);
	return found;
}

void RegionColors_UpdateByName(const CString &name, COLORREF color, int radius)
{
	RC_EnsureInit();
	CString tm_path;
	EnterCriticalSection(&g_rc_cs);
	std::map<CString, SRegionColor>::iterator it = g_rc.find(name);
	if (it != g_rc.end()) { it->second.color = color; it->second.radius = radius; }
	tm_path = g_rc_tm_path;
	LeaveCriticalSection(&g_rc_cs);
	// File IO outside the lock.
	if (!tm_path.IsEmpty()) SaveRegionColorRadius(tm_path, name, color, radius);
}

int RegionColors_UpdateAll(COLORREF color, int radius)
{
	RC_EnsureInit();
	// Snapshot the region names + tm path under the lock, update the cache, then do
	// the file rewrites outside the lock.
	std::vector<CString> names;
	CString tm_path;
	EnterCriticalSection(&g_rc_cs);
	for (std::map<CString, SRegionColor>::iterator it = g_rc.begin(); it != g_rc.end(); ++it) {
		it->second.color = color;
		it->second.radius = radius;
		names.push_back(it->first);
	}
	tm_path = g_rc_tm_path;
	LeaveCriticalSection(&g_rc_cs);
	if (!tm_path.IsEmpty()) {
		for (size_t i = 0; i < names.size(); ++i)
			SaveRegionColorRadius(tm_path, names[i], color, radius);
	}
	return (int)names.size();
}
