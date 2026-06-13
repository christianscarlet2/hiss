//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************

#include "stdafx.h"
#include "CSymbolEngineTimingTells.h"

#include "CScraper.h"
#include "CStringMatch.h"
#include "CTableState.h"
#include "ChatTerminalWindow.h"
#include "CEngineContainer.h"
#include "CSymbolEngineRaisers.h"
#include "CSymbolEngineUserchair.h"
#include "..\CTablemap\CTablemap.h"

CSymbolEngineTimingTells::CSymbolEngineTimingTells() {
  _last_chair = -1;
  _last_tick = 0;
  for (int i = 0; i < kMaxNumberOfPlayers; ++i) {
    _timing_seconds[i] = 0.0;
    _was_active[i] = false;
  }
}

CSymbolEngineTimingTells::~CSymbolEngineTimingTells() {}

void CSymbolEngineTimingTells::InitOnStartup()   { UpdateOnConnection(); }

void CSymbolEngineTimingTells::UpdateOnConnection() {
  _last_chair = -1;
  _last_tick = 0;
  for (int i = 0; i < kMaxNumberOfPlayers; ++i) {
    _timing_seconds[i] = 0.0;
    _was_active[i] = false;
  }
  _last_published = "";
  _last_updated_str = "\x1b[33m-- (last updated)\x1b[0m";
  _last_publish_tick = 0;
}

void CSymbolEngineTimingTells::UpdateOnHandreset() {
  // Don't carry a dwell across hands: the next activation must not be timed
  // against a chair that went active in the previous hand. Keep the recorded
  // per-chair values (they're useful reads) but drop the "currently active" anchor.
  _last_chair = -1;
}

void CSymbolEngineTimingTells::UpdateOnNewRound() {}
void CSymbolEngineTimingTells::UpdateOnMyTurn() {}

bool CSymbolEngineTimingTells::Rect1Active(int chair) {
  if (p_tablemap == NULL || p_scraper == NULL || p_string_match == NULL) return false;
  CString name;
  name.Format("p%dactive", chair);
  RMap::iterator it = p_tablemap->set_r$()->find(name.GetString());
  if (it == p_tablemap->set_r$()->end()) return false;
  // Force rectangle-1-only detection: the timing tell must ignore the optional
  // rect2 OR-match. Toggle it off for this read, then restore.
  bool saved = it->second.rect2_enabled;
  it->second.rect2_enabled = false;
  CString result;
  bool got = p_scraper->EvaluateRegion(name, &result);
  it->second.rect2_enabled = saved;
  return got && p_string_match->IsStringActive(result);
}

void CSymbolEngineTimingTells::UpdateOnHeartbeat() {
  if (p_tablemap == NULL) return;
  unsigned long now = GetTickCount();
  int n = p_tablemap->nchairs();
  if (n > kMaxNumberOfPlayers) n = kMaxNumberOfPlayers;

  bool changed = false;
  for (int chair = 0; chair < n; ++chair) {
    bool active1 = Rect1Active(chair);
    bool rising = active1 && !_was_active[chair];
    _was_active[chair] = active1;
    if (rising) {
      // A new chair just lit up: the previously-active chair's dwell (how long
      // until this player became active) is its action-time timing tell.
      if (_last_chair >= 0 && _last_chair != chair) {
        _timing_seconds[_last_chair] = (double)(now - _last_tick) / 1000.0;
        changed = true;
      }
      _last_chair = chair;
      _last_tick = now;
      changed = true;   // active anchor moved -> refresh the State box
    }
  }
  // Publish when the timing values changed, OR at least once per second so the
  // "current time" clock line in the State box keeps ticking even when no chair
  // has become active.
  if (changed || (now - _last_publish_tick) >= 1000) {
    PublishStateBox();
  }
}

void CSymbolEngineTimingTells::PublishStateBox() {
  if (p_tablemap == NULL || p_chat_terminal == NULL) return;
  int n = p_tablemap->nchairs();
  if (n > kMaxNumberOfPlayers) n = kMaxNumberOfPlayers;
  // FIXED in-place block (pinned at the top of the State section, replaced -- not
  // appended -- each update, like a terminal progress bar). The LABELS ("pN=")
  // stay in the default colour; only the VALUES are bold white (ANSI 1;37), so
  // visually only the numbers change as the action passes around the table.
  CString txt = "timing tells (sec, rect1):\r\n";
  for (int chair = 0; chair < n; ++chair) {
    CString val;
    if (_timing_seconds[chair] > 0.0) val.Format("%.2f", _timing_seconds[chair]);
    else                              val = "--";
    if (chair == _last_chair) val += "*";    // currently the active chair
    CString cell;
    // label (default colour) + bold-white value (padded to a fixed width) + reset.
    // The fixed-width value keeps every cell the same length so the trailing TAB
    // always lands on the same tab stop -> the columns line up vertically in both
    // the libvterm pane and the browser (tab-size 8 in each).
    cell.Format("p%d=\x1b[1;37m%-6s\x1b[0m", chair, val.GetString());
    txt += cell;
    txt += ((chair % 3) == 2) ? "\r\n" : "\t";
  }
  SYSTEMTIME st; GetLocalTime(&st);
  int hr12 = st.wHour % 12; if (hr12 == 0) hr12 = 12;
  const char *ampm = (st.wHour < 12) ? "AM" : "PM";

  // The "(last updated)" line FREEZES at the moment the VALUES last changed; the
  // "current time" line below it ticks every second. So only refresh the frozen
  // line when the timing values actually changed.
  if (txt != _last_published) {
    _last_published = txt;
    _last_updated_str.Format("\x1b[33m%d:%02d:%02d %s (last updated)\x1b[0m",
                             hr12, st.wMinute, st.wSecond, ampm);
  }
  // Yellow frozen "(last updated)" line, then the live white current-time line --
  // both 12-hour, time-first so the two times align vertically.
  CString stamp;
  stamp.Format("\r\n%s\r\n\x1b[1;37m%d:%02d:%02d %s\x1b[0m\r\n",
               _last_updated_str.GetString(),
               hr12, st.wMinute, st.wSecond, ampm);
  p_chat_terminal->SetPinnedStateAsync("main", txt + stamp);
  _last_publish_tick = GetTickCount();
}

bool CSymbolEngineTimingTells::EvaluateSymbol(const CString name, double *result, bool log) {
  // Derived timing tells that resolve the relevant chair automatically, so the
  // strategy does not have to switch on raischair/userchair itself. All return
  // SECONDS; 0.0 means "no read yet" (no dwell recorded for that chair this hand).
  if (name == "lastraiseractiontime") {
    // How long the player we must react to (the last raiser / the bettor) took.
    // By the time it is our turn, that chair's dwell has already been recorded.
    if (result != NULL) {
      int rc = -1;
      if (p_engine_container != NULL
          && p_engine_container->symbol_engine_raisers() != NULL) {
        rc = p_engine_container->symbol_engine_raisers()->raischair();
      }
      *result = (rc >= 0 && rc < kMaxNumberOfPlayers) ? _timing_seconds[rc] : 0.0;
    }
    return true;
  }
  if (name == "myactiontime") {
    if (result != NULL) {
      int uc = -1;
      if (p_engine_container != NULL
          && p_engine_container->symbol_engine_userchair() != NULL) {
        uc = p_engine_container->symbol_engine_userchair()->userchair();
      }
      *result = (uc >= 0 && uc < kMaxNumberOfPlayers) ? _timing_seconds[uc] : 0.0;
    }
    return true;
  }
  // pNtiming -> chair N's last recorded action time, in seconds.
  if (name.GetLength() < 7) return false;
  if (name.Left(1) != "p") return false;
  if (name.Right(6) != "timing") return false;
  CString mid = name.Mid(1, name.GetLength() - 7);   // digits between 'p' and 'timing'
  if (mid.IsEmpty()) return false;
  for (int i = 0; i < mid.GetLength(); ++i) {
    if (mid[i] < '0' || mid[i] > '9') return false;
  }
  int chair = atoi(mid.GetString());
  if (chair < 0 || chair >= kMaxNumberOfPlayers) return false;
  if (result != NULL) *result = _timing_seconds[chair];
  return true;
}

CString CSymbolEngineTimingTells::SymbolsProvided() {
  CString s = "lastraiseractiontime myactiontime ";
  for (int i = 0; i < kMaxNumberOfPlayers; ++i) {
    CString one;
    one.Format("p%dtiming ", i);
    s += one;
  }
  return s;
}
