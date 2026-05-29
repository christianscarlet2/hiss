//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: OCR Name Mapping Implementation
//
//******************************************************************************

#include "stdafx.h"
#include "COCRNameMapping.h"

// Escape a string for safe inclusion inside a single-quoted SQL literal
// by doubling any embedded single quotes.
static CString EscapeSqlLiteral(const char *value)
{
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

COCRNameMapping::COCRNameMapping()
	: _pgconn(NULL),
	  _invalidate_pending(0)
{
}

COCRNameMapping::~COCRNameMapping()
{
	ClearCache();
}

void COCRNameMapping::SetConnection(PGconn *pgconn)
{
	_pgconn = pgconn;
}

void COCRNameMapping::ClearCache()
{
	_cache.clear();
}

bool COCRNameMapping::_ExecuteMappingQuery(const char *ocr_detected_name, int id_site, SOCRNameMapping *mapping)
{
	if (_pgconn == NULL || PQstatus(_pgconn) != CONNECTION_OK)
		return false;

	if (ocr_detected_name == NULL || strlen(ocr_detected_name) == 0)
		return false;

	if (mapping == NULL)
		return false;

	// Query the ocr_name_mappings table for verified mappings
	CString query;
	query.Format(
		"SELECT actual_username, verified FROM ocr_name_mappings "
		"WHERE ocr_detected_name = '%s' AND id_site = %d AND verified = true "
		"LIMIT 1",
		EscapeSqlLiteral(ocr_detected_name).GetString(), id_site);

	PGresult *res = PQexec(_pgconn, query);

	if (PQstatus(_pgconn) != CONNECTION_OK)
	{
		if (res) PQclear(res);
		return false;
	}

	memset(mapping->actual_username, 0, kMaxLengthOfPlayername);
	mapping->verified = false;
	mapping->found = false;

	// If we found a verified mapping, use it
	if (PQntuples(res) > 0)
	{
		char *actual_name = PQgetvalue(res, 0, 0);
		char *verified_str = PQgetvalue(res, 0, 1);

		strncpy_s(mapping->actual_username, kMaxLengthOfPlayername, actual_name, _TRUNCATE);
		mapping->verified = (strcmp(verified_str, "t") == 0);
		mapping->found = true;

		write_log(Preferences()->debug_pokertracker(),
			"[COCRNameMapping] Found mapping for OCR name [%s]: actual [%s] verified [%d]\n",
			ocr_detected_name, actual_name, mapping->verified);

		PQclear(res);
		return true;
	}

	PQclear(res);
	return false;
}

bool COCRNameMapping::LookupActualName(const char *ocr_detected_name, int id_site, SOCRNameMapping *mapping)
{
	if (ocr_detected_name == NULL || strlen(ocr_detected_name) == 0)
		return false;

	if (mapping == NULL)
		return false;

	// If the admin UI changed a mapping, drop our cache so the new state is
	// picked up immediately. Cache mutation happens only on this (PT) thread.
	if (InterlockedExchange(&_invalidate_pending, 0) != 0) {
		_cache.clear();
	}

	// Build cache key
	CString cache_key;
	cache_key.Format("%s|%d", ocr_detected_name, id_site);
	std::string key = (LPCSTR)cache_key;

	// Check cache first
	if (_cache.find(key) != _cache.end())
	{
		*mapping = _cache[key];
		return mapping->found;
	}

	// Query database
	if (_ExecuteMappingQuery(ocr_detected_name, id_site, mapping))
	{
		// Cache the result
		_cache[key] = *mapping;
		return true;
	}

	// No mapping found - mark as not found but cache it
	mapping->found = false;
	mapping->verified = false;
	memset(mapping->actual_username, 0, kMaxLengthOfPlayername);
	_cache[key] = *mapping;

	return false;
}

// Opens a fresh PostgreSQL connection using the saved PT4 preferences.
// Used by the admin/verification UI so its queries don't share libpq state
// with the PokerTracker thread's connection (libpq is not thread-safe per-conn).
// Returns NULL on failure. Caller must PQfinish() the result.
PGconn *COCRNameMapping::_OpenAdminConnection()
{
	CString conn_str;
	conn_str.Format("host=%s port=%s user=%s password=%s dbname='%s'",
		Preferences()->pt_ip_addr(),
		Preferences()->pt_port(),
		Preferences()->pt_user(),
		Preferences()->pt_pass(),
		Preferences()->pt_dbname());
	PGconn *conn = PQconnectdb(conn_str.GetString());
	if (conn == NULL || PQstatus(conn) != CONNECTION_OK) {
		if (conn != NULL) {
			PQfinish(conn);
		}
		return NULL;
	}
	return conn;
}

bool COCRNameMapping::ListMappings(bool only_unverified, int limit, std::vector<SOCRNameMappingRow> *out)
{
	if (out == NULL) {
		return false;
	}
	out->clear();

	PGconn *conn = _OpenAdminConnection();
	if (conn == NULL) {
		return false;
	}

	CString query;
	query.Format(
		"SELECT id, actual_username, ocr_detected_name, id_site, verified, "
		"COALESCE(confidence, 0), COALESCE(to_char(last_updated, 'YYYY-MM-DD HH24:MI:SS'), '') "
		"FROM ocr_name_mappings %s "
		"ORDER BY verified ASC, last_updated DESC NULLS LAST, id DESC "
		"LIMIT %d",
		only_unverified ? "WHERE verified = false" : "",
		limit > 0 ? limit : 500);

	PGresult *res = PQexec(conn, query.GetString());
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		write_log(Preferences()->debug_pokertracker(),
			"[COCRNameMapping] ListMappings query failed: %s\n", PQerrorMessage(conn));
		if (res) PQclear(res);
		PQfinish(conn);
		return false;
	}

	int rows = PQntuples(res);
	out->reserve(rows);
	for (int i = 0; i < rows; ++i) {
		SOCRNameMappingRow row;
		row.id = atoi(PQgetvalue(res, i, 0));
		row.actual_username = PQgetvalue(res, i, 1);
		row.ocr_detected_name = PQgetvalue(res, i, 2);
		row.id_site = atoi(PQgetvalue(res, i, 3));
		row.verified = (strcmp(PQgetvalue(res, i, 4), "t") == 0);
		row.confidence = atof(PQgetvalue(res, i, 5));
		row.last_updated = PQgetvalue(res, i, 6);
		out->push_back(row);
	}

	PQclear(res);
	PQfinish(conn);
	return true;
}

bool COCRNameMapping::SetVerified(int id, bool verified)
{
	if (id <= 0) {
		return false;
	}
	PGconn *conn = _OpenAdminConnection();
	if (conn == NULL) {
		return false;
	}

	CString query;
	query.Format(
		"UPDATE ocr_name_mappings SET verified = %s, last_updated = CURRENT_TIMESTAMP WHERE id = %d",
		verified ? "true" : "false", id);
	PGresult *res = PQexec(conn, query.GetString());
	bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
	if (!ok) {
		write_log(Preferences()->debug_pokertracker(),
			"[COCRNameMapping] SetVerified failed for id=%d: %s\n", id, PQerrorMessage(conn));
	}
	if (res) PQclear(res);
	PQfinish(conn);

	// Signal the PT thread to drop its lookup cache so the new verified state
	// is picked up on the next scrape.
	InterlockedExchange(&_invalidate_pending, 1);
	return ok;
}

bool COCRNameMapping::DeleteMapping(int id)
{
	if (id <= 0) {
		return false;
	}
	PGconn *conn = _OpenAdminConnection();
	if (conn == NULL) {
		return false;
	}

	CString query;
	query.Format("DELETE FROM ocr_name_mappings WHERE id = %d", id);
	PGresult *res = PQexec(conn, query.GetString());
	bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
	if (!ok) {
		write_log(Preferences()->debug_pokertracker(),
			"[COCRNameMapping] DeleteMapping failed for id=%d: %s\n", id, PQerrorMessage(conn));
	}
	if (res) PQclear(res);
	PQfinish(conn);

	InterlockedExchange(&_invalidate_pending, 1);
	return ok;
}

bool COCRNameMapping::SaveMapping(const char *actual_username, const char *ocr_detected_name, int id_site, bool verified)
{
	if (_pgconn == NULL || PQstatus(_pgconn) != CONNECTION_OK)
		return false;

	if (actual_username == NULL || ocr_detected_name == NULL)
		return false;

	CString escaped_actual = EscapeSqlLiteral(actual_username);
	CString escaped_ocr = EscapeSqlLiteral(ocr_detected_name);
	CString query;
	// On conflict, refresh the mapping but never downgrade an already-verified
	// row back to unverified (a later fuzzy re-match must not undo a confirmation).
	query.Format(
		"INSERT INTO ocr_name_mappings (actual_username, ocr_detected_name, id_site, verified) "
		"VALUES ('%s', '%s', %d, %s) "
		"ON CONFLICT (ocr_detected_name, id_site) DO UPDATE SET "
		"actual_username = EXCLUDED.actual_username, "
		"verified = ocr_name_mappings.verified OR EXCLUDED.verified, "
		"last_updated = CURRENT_TIMESTAMP",
		escaped_actual.GetString(), escaped_ocr.GetString(), id_site, verified ? "true" : "false");

	PGresult *res = PQexec(_pgconn, query);

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		write_log(Preferences()->debug_pokertracker(),
			"[COCRNameMapping] Failed to save mapping for [%s] -> [%s]\n",
			ocr_detected_name, actual_username);
		PQclear(res);
		return false;
	}

	PQclear(res);

	// Clear cache entry so it gets reloaded
	CString cache_key;
	cache_key.Format("%s|%d", ocr_detected_name, id_site);
	_cache.erase((LPCSTR)cache_key);

	return true;
}
