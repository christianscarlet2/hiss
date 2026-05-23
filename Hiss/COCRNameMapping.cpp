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

COCRNameMapping::COCRNameMapping()
	: _pgconn(NULL)
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
		ocr_detected_name, id_site);

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

bool COCRNameMapping::SaveMapping(const char *actual_username, const char *ocr_detected_name, int id_site, bool verified)
{
	if (_pgconn == NULL || PQstatus(_pgconn) != CONNECTION_OK)
		return false;

	if (actual_username == NULL || ocr_detected_name == NULL)
		return false;

	CString query;
	query.Format(
		"INSERT INTO ocr_name_mappings (actual_username, ocr_detected_name, id_site, verified) "
		"VALUES ('%s', '%s', %d, %s) "
		"ON CONFLICT (ocr_detected_name, id_site) DO UPDATE SET "
		"actual_username = '%s', verified = %s, last_updated = CURRENT_TIMESTAMP",
		actual_username, ocr_detected_name, id_site, verified ? "true" : "false",
		actual_username, verified ? "true" : "false");

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
