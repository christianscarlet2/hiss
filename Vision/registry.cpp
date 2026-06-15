//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose:
//
//******************************************************************************



#include "stdafx.h"

#include "registry.h"
#include "stdio.h"
#include "stdlib.h"
#include "DialogTableMap.h"
#include "..\CTablemap\CTablemapDB.h"   // settings live in the postgres DB, not the registry

Registry::Registry(void) 
{
}

// Settings now live in the postgres `settings` table (key "vision_prefs"), not the registry.
static const char *kVisionPrefsKey = "vision_prefs";

void Registry::read_reg(void)
{
	// Defaults
	tablemap_x  = 0;
	tablemap_y  = 0;
	tablemap_dx = 650;
	tablemap_dy = 940;

	main_x  = tablemap_dx;
	main_y  = 0;
	main_dx = 400;
	main_dy = 300;

	grhash_x = grhash_y = 0;
	grhash_dx = 380;
	grhash_dy = 330;

	region_grouping = BY_TYPE; // None

	if (p_tablemap_db == NULL) p_tablemap_db = new CTablemapDB;
	CString v;
	v = p_tablemap_db->GetSettingString(kVisionPrefsKey, "grhash_x");        if (!v.IsEmpty()) grhash_x = atoi(v.GetString());
	v = p_tablemap_db->GetSettingString(kVisionPrefsKey, "grhash_y");        if (!v.IsEmpty()) grhash_y = atoi(v.GetString());
	v = p_tablemap_db->GetSettingString(kVisionPrefsKey, "grhash_dx");       if (!v.IsEmpty()) grhash_dx = atoi(v.GetString());
	v = p_tablemap_db->GetSettingString(kVisionPrefsKey, "grhash_dy");       if (!v.IsEmpty()) grhash_dy = atoi(v.GetString());
	v = p_tablemap_db->GetSettingString(kVisionPrefsKey, "region_grouping"); if (!v.IsEmpty()) region_grouping = atoi(v.GetString());
}

void Registry::write_reg(void)
{
	if (p_tablemap_db == NULL) p_tablemap_db = new CTablemapDB;
	CString s;
	s.Format("%d", grhash_x);        p_tablemap_db->SetSettingString(kVisionPrefsKey, "grhash_x", s);
	s.Format("%d", grhash_y);        p_tablemap_db->SetSettingString(kVisionPrefsKey, "grhash_y", s);
	s.Format("%d", grhash_dx);       p_tablemap_db->SetSettingString(kVisionPrefsKey, "grhash_dx", s);
	s.Format("%d", grhash_dy);       p_tablemap_db->SetSettingString(kVisionPrefsKey, "grhash_dy", s);
	s.Format("%d", region_grouping); p_tablemap_db->SetSettingString(kVisionPrefsKey, "region_grouping", s);
}
