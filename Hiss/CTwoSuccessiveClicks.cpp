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
#include "CScraper.h"
#include "..\CTablemap\CTablemap.h"
#include "..\CTablemap\CTableMapAccess.h"
#include "..\CTablemap\CTablemapDB.h"

CTwoSuccessiveClicks *p_two_successive_clicks = NULL;

#define ENT CSLock lock(m_critsec);

// Per-tablemap values live under these setting-keys, fielded by the loaded
// tablemap's filename (mirrors how table-window placement is stored).
static const CString kKeyText1 = "two_successive_clicks_text";   // text box 1
static const CString kKeyText2 = "two_successive_clicks_text2";  // text box 2
static const CString kKeyEn1   = "two_successive_clicks_en1";    // enable 1 ("1"/"0")
static const CString kKeyEn2   = "two_successive_clicks_en2";    // enable 2 ("1"/"0")
static const CString kKeyDelay = "two_successive_clicks_delay";
static const int     kDefaultDelayMs = 200;

CTwoSuccessiveClicks::CTwoSuccessiveClicks() {
  _text1 = "";
  _text2 = "";
  _enable1 = false;
  _enable2 = false;
  _delay_ms = kDefaultDelayMs;
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

void CTwoSuccessiveClicks::SetConfig(CString text1, bool enable1, CString text2, bool enable2,
                                     int delay_ms, bool persist_to_tablemap) {
  text1.Trim();
  text2.Trim();
  if (delay_ms < 0) delay_ms = 0;
  {
    ENT
    _text1 = text1; _enable1 = enable1;
    _text2 = text2; _enable2 = enable2;
    _delay_ms = delay_ms;
  }
  if (persist_to_tablemap && p_tablemap_db != NULL) {
    CString field = TablemapField();
    CString delay_string;
    delay_string.Format("%d", delay_ms);
    p_tablemap_db->SetSettingString(kKeyText1, field, text1);
    p_tablemap_db->SetSettingString(kKeyText2, field, text2);
    p_tablemap_db->SetSettingString(kKeyEn1,   field, enable1 ? "1" : "0");
    p_tablemap_db->SetSettingString(kKeyEn2,   field, enable2 ? "1" : "0");
    p_tablemap_db->SetSettingString(kKeyDelay, field, delay_string);
  }
}

void CTwoSuccessiveClicks::LoadForCurrentTablemap() {
  if (p_tablemap_db == NULL) return;
  CString field = TablemapField();
  CString text1 = p_tablemap_db->GetSettingString(kKeyText1, field);
  CString text2 = p_tablemap_db->GetSettingString(kKeyText2, field);
  CString en1s  = p_tablemap_db->GetSettingString(kKeyEn1,   field);
  CString en2s  = p_tablemap_db->GetSettingString(kKeyEn2,   field);
  CString delay = p_tablemap_db->GetSettingString(kKeyDelay, field);
  // Default enable to ON when there is saved text but the enable flag was never
  // stored (back-compat with the original single-text version).
  bool en1 = en1s.IsEmpty() ? !text1.IsEmpty() : (en1s == "1");
  bool en2 = en2s.IsEmpty() ? !text2.IsEmpty() : (en2s == "1");
  int d = atoi(delay.GetString());
  if (d <= 0) d = kDefaultDelayMs;
  bool changed;
  {
    ENT
    changed = (_text1 != text1) || (_enable1 != en1)
           || (_text2 != text2) || (_enable2 != en2) || (_delay_ms != d);
    _text1 = text1; _enable1 = en1;
    _text2 = text2; _enable2 = en2;
    _delay_ms = d;
    // NOTE: do NOT reset _was_matching here -- this is called periodically to pick
    // up edits made in Vision, and resetting it would re-fire while still matching.
  }
  if (changed) {
    write_log(k_always_log_basic_information,
      "[TwoClicks] Config for tablemap \"%s\": box1 en=%d \"%s\", box2 en=%d \"%s\", delay=%d ms\n",
      field.GetString(), (int)en1, text1.GetString(), (int)en2, text2.GetString(), d);
  }
}

CString CTwoSuccessiveClicks::Text1() { ENT return _text1; }
CString CTwoSuccessiveClicks::Text2() { ENT return _text2; }
bool    CTwoSuccessiveClicks::Enable1() { ENT return _enable1; }
bool    CTwoSuccessiveClicks::Enable2() { ENT return _enable2; }
int     CTwoSuccessiveClicks::DelayMs() { ENT return _delay_ms; }

bool CTwoSuccessiveClicks::HandleCycle(bool decision_is_raise) {
  CString text1, text2;
  bool    enable1, enable2;
  int     delay_ms;
  {
    ENT
    text1 = _text1; enable1 = _enable1;
    text2 = _text2; enable2 = _enable2;
    delay_ms = _delay_ms;
  }
  bool box1_active = enable1 && !text1.IsEmpty();
  bool box2_active = enable2 && !text2.IsEmpty();
  // Disabled when neither text box is enabled with text.
  if (!box1_active && !box2_active) {
    _was_matching = false;
    return false;
  }
  // Only act when the .ohf decision is to RAISE.
  if (!decision_is_raise) {
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

  // OCR the label region and compare (trimmed, case-insensitive) against either box.
  CString ocr;
  p_scraper->EvaluateRegion("two_successive_clicks_label", &ocr);
  ocr.Trim();
  bool matching = (box1_active && ocr.CompareNoCase(text1) == 0)
               || (box2_active && ocr.CompareNoCase(text2) == 0);

  bool clicked = false;
  // Edge-triggered: fire once when the label starts matching.
  if (matching && !_was_matching) {
    CMyMutex mutex;
    if (!mutex.IsLocked()) {
      return false;
    }
    write_log(k_always_log_basic_information,
      "[TwoClicks] RAISE + label \"%s\" matched: click rect1 (%d,%d-%d,%d), wait %d ms, click rect2 (%d,%d-%d,%d).\n",
      ocr.GetString(), rect1.left, rect1.top, rect1.right, rect1.bottom, delay_ms,
      rect2.left, rect2.top, rect2.right, rect2.bottom);
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
