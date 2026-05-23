//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: OCR Name Mapping - Maps OCR-detected names to actual player names
//
//******************************************************************************

#ifndef INC_COCRNAMEMAPPING_H
#define INC_COCRNAMEMAPPING_H

#include "libpq-fe.h"
#include <map>

struct SOCRNameMapping
{
	char	actual_username[kMaxLengthOfPlayername];
	bool	verified;
	bool	found;
};

class COCRNameMapping
{
public:
	COCRNameMapping();
	~COCRNameMapping();
	
	// Initialize with PostgreSQL connection (called by PokerTracker thread)
	void SetConnection(PGconn *pgconn);
	
	// Look up OCR-detected name and return actual name if mapping exists
	// Returns:
	//   - actual mapped name if verified mapping found
	//   - fallback name (original OCR) if no verified mapping
	//   - marks mapping.verified to indicate if result is verified from hand history
	bool LookupActualName(const char *ocr_detected_name, int id_site, SOCRNameMapping *mapping);
	
	// Add or update a mapping (used to learn from hand histories)
	bool SaveMapping(const char *actual_username, const char *ocr_detected_name, int id_site, bool verified);
	
	// Clear cache (e.g., on new session or table)
	void ClearCache();

private:
	PGconn *_pgconn;
	std::map<std::string, SOCRNameMapping> _cache;  // Key: "ocr_name|site_id"
	
	bool _ExecuteMappingQuery(const char *ocr_detected_name, int id_site, SOCRNameMapping *mapping);
};

#endif INC_COCRNAMEMAPPING_H
