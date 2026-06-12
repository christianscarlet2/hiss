//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: see CTwoSuccessiveClicks.h
//
//******************************************************************************

#include "stdafx.h"
#include "CTwoSuccessiveClicks.h"

#include "CCasinoInterface.h"
#include "CMyMutex.h"
#include "CPreferences.h"
#include "CScraper.h"
#include "..\CTablemap\CTablemap.h"
#include "..\CTablemap\CTableMapAccess.h"
#include "..\CTablemap\CTablemapDB.h"

CTwoSuccessiveClicks *p_two_successive_clicks = NULL;

#define ENT CSLock lock(m_critsec);

// Per-tablemap values live under these setting-keys, fielded by the loaded
// tablemap's filename (mirrors how table-window placement is stored).
static const CString kKeyText  = "two_successive_clicks_text";
static const CString kKeyDelay = "two_successive_clicks_delay";
static const int     kDefaultDelayMs = 200;

CTwoSuccessiveClicks::CTwoSuccessiveClicks() {
  _match_text   = "";
  _delay_ms     = kDefaultDelayMs;
  _was_matching = false;
}

CTwoSuccessiveClicks::~CTwoSuccessiveClicks() {
}

CString CTwoSuccessiveClicks::TablemapField() {
  CString f = (p_tablemap != NULL) ? p_tablemap->filename() : CString("");
  f.Trim();
  if (f.IsEmpty()) f = "default";
  return f;
}

void CTwoSuccessiveClicks::SetConfig(CString match_text, int delay_ms, bool persist_to_tablemap) {
  match_text.Trim();
  if (delay_ms < 0) delay_ms = 0;
  {
    ENT
    _match_text = match_text;
    _delay_ms   = delay_ms;
  }
  if (persist_to_tablemap && p_tablemap_db != NULL) {
    CString field = TablemapField();
    CString delay_string;
    delay_string.Format("%d", delay_ms);
    p_tablemap_db->SetSettingString(kKeyText,  field, match_text);
    p_tablemap_db->SetSettingString(kKeyDelay, field, delay_string);
  }
}

void CTwoSuccessiveClicks::LoadForCurrentTablemap() {
  if (p_tablemap_db == NULL) return;
  CString field = TablemapField();
  CString text  = p_tablemap_db->GetSettingString(kKeyText,  field);
  CString delay = p_tablemap_db->GetSettingString(kKeyDelay, field);
  int d = atoi(delay.GetString());
  if (d <= 0) d = kDefaultDelayMs;
  ENT
  _match_text   = text;
  _delay_ms     = d;
  _was_matching = false;
}

CString CTwoSuccessiveClicks::MatchText() {
  ENT
  return _match_text;
}

int CTwoSuccessiveClicks::DelayMs() {
  ENT
  return _delay_ms;
}

bool CTwoSuccessiveClicks::HandleCycle() {
  CString match_text;
  int     delay_ms;
  {
    ENT
    match_text = _match_text;
    delay_ms   = _delay_ms;
  }
  // Disabled when no match-text is configured.
  if (match_text.IsEmpty()) {
    _was_matching = false;
    return false;
  }
  if (p_tablemap == NULL || p_scraper == NULL
      || p_casino_interface == NULL || p_tablemap_access == NULL) {
    return false;
  }
  // The trigger region must exist and define BOTH rectangles.
  RECT rect1;
  if (!p_tablemap_access->GetTableMapRect("two_successive_clicks", &rect1)) {
    return false;
  }
  RMapCI region = p_tablemap->r$()->find("two_successive_clicks");
  if (region == p_tablemap->r$()->end()) {
    return false;
  }
  if (!region->second.rect2_enabled) {
    write_log(Preferences()->debug_autoplayer(),
      "[TwoClicks] Region two_successive_clicks has no second rectangle (rect2_enabled=false).\n");
    return false;
  }
  RECT rect2;
  rect2.left   = (LONG)region->second.left2;
  rect2.top    = (LONG)region->second.top2;
  rect2.right  = (LONG)region->second.right2;
  rect2.bottom = (LONG)region->second.bottom2;

  // OCR the label region and compare (trimmed, case-insensitive).
  CString ocr;
  p_scraper->EvaluateRegion("two_successive_clicks_label", &ocr);
  ocr.Trim();
  bool matching = (ocr.CompareNoCase(match_text) == 0);

  bool clicked = false;
  // Edge-triggered: fire once when the label starts matching.
  if (matching && !_was_matching) {
    CMyMutex mutex;
    if (!mutex.IsLocked()) {
      return false;
    }
    write_log(Preferences()->debug_autoplayer(),
      "[TwoClicks] Label \"%s\" == \"%s\": click rect1, wait %d ms, click rect2.\n",
      ocr.GetString(), match_text.GetString(), delay_ms);
    p_casino_interface->ClickRect(rect1);
    Sleep(delay_ms);
    if (p_casino_interface->TableLostFocus()) {
      _was_matching = matching;
      return false;
    }
    p_casino_interface->ClickRect(rect2);
    clicked = true;
  }
  _was_matching = matching;
  return clicked;
}
