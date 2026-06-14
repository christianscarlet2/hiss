//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: class for bets, balances and pots,
//  which also handles proprocessing of special number formats, ...
//    http://www.maxinmontreal.com/forums/viewtopic.php?f=117&t=20164
//    http://www.maxinmontreal.com/forums/viewtopic.php?f=156&t=19216
//    http://www.maxinmontreal.com/forums/viewtopic.php?f=156&t=19658
//    http://www.maxinmontreal.com/forums/viewtopic.php?f=156&t=20096
//    http://www.maxinmontreal.com/forums/viewtopic.php?f=156&t=20085
//
//******************************************************************************

#include "stdafx.h"
#include "CScrapedMoney.h"
#include "..\DLLs\StringFunctions_DLL\string_functions.h"

extern bool g_tgi_set;   // table_game_info set => BB-denominated table mode (declared in CScraper.cpp)

// In a big-blind-denominated table (table_game_info set), no real bet/balance/pot reaches
// thousands of big blinds, so a scraped value above this is OCR garbage (e.g. "20.25BB"
// misread as 20226). Reject it and keep the last-good value. Generous so legit big multiway
// pots pass. Not applied to chip-denominated tables (g_tgi_set false).
static const double kMaxPlausibleBBMoney = 8000.0;

CScrapedMoney::CScrapedMoney() {
	Reset();
}

CScrapedMoney::~CScrapedMoney() {
}

bool CScrapedMoney::SetValue(CString scraped_value) {
	if (scraped_value == "") {
		return false;
	}
	ReplaceKnownNonASCIICharacters(&scraped_value);
	WarnAboutNonASCIICharacters(&scraped_value);
	RemoveLeftWhiteSpace(&scraped_value);
	RemoveRightWhiteSpace(&scraped_value);
	RemoveMultipleWhiteSpaces(&scraped_value);
	RemoveSpacesInsideNumbers(&scraped_value);
	ReplaceOutlandischCurrencyByDollarsandCents(&scraped_value);
	RemoveSpacesInFrontOfCentMultipliers(&scraped_value);
	ReplaceCommasInNumbersByDots(&scraped_value);
	RemoveExtraDotsInNumbers(&scraped_value);
	//!!!KeepBalanceNumbersOnly(&scraped_value);
	if (scraped_value == "") {
		// Empty data (e.g. in the c0sblind) must not evaluated
		// otherwise we might overwrite known good data (e.g. from ttlimits)
		return false;
	}
	// Evaluate unauthorized chars: parenthesis and other brackets
	// Mainly for DDPoker
	if (scraped_value.Find("(") != -1 && scraped_value.Find(")") != -1) {
		return false;
	}
	if (scraped_value.Find("{") != -1 && scraped_value.Find("}") != -1) {
		return false;
	}
	if (scraped_value.Find("[") != -1 && scraped_value.Find("]") != -1) {
		return false;
	}
	if (scraped_value.Find("<") != -1 && scraped_value.Find(">") != -1) {
		return false;
	}
	if (!(scraped_value.Find("0123456789"))) {
		// Again: evaluate only meaningful input
		return false;
	}
	double result = StringToMoney(scraped_value);
	if (result >= 0.0) {
		// Root mis-scrape guard (BB-denominated mode). An absurdly large value is almost
		// always a DROPPED DECIMAL POINT in the OCR -- e.g. "202.26" scraped as "20226".
		// Recover it by restoring the 2-place decimal when that yields a plausible BB value;
		// only reject (keep last-good) if it is still nonsense after recovery.
		if (g_tgi_set && result > kMaxPlausibleBBMoney) {
			double recovered = result / 100.0;        // 20226 -> 202.26
			if (recovered <= kMaxPlausibleBBMoney) {
				result = recovered;
			} else {
				return false;                          // still absurd -> garbage
			}
		}
		return SetValue(result);
	}
	return false;
}

bool CScrapedMoney::SetValue(double new_value) {
	_value = new_value;
	return true;
}

void CScrapedMoney::Reset() {
	SetValue(0.0);
}