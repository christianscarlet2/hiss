//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Clean interface for scraper, casino-interface amd autoplayer
//
//******************************************************************************

#include "stdafx.h"
#include "CAutoplayerButton.h"

#include "CCasinoHotkey.h"
#include "CCasinoInterface.h"
#include "CAutoOcr.h"

#include "CStringMatch.h"
#include "..\CTablemap\CTablemap.h"
#include "..\DLLs\Files_DLL\Files.h"
#include "CAutoconnector.h"

// Always-on one-liner into logs\button_debug.log so we can see, for every
// autoplayer button click, the exact rect/method/hwnd that was used
// (diagnoses "isfinalanswer is true but nothing happens on the phone").
static void ButtonDebugLog(const char *fmt, ...) {
  if (!Preferences()->debug_autoplayer()) return;
  CString path = LogsDirectory() + "button_debug.log";
  FILE *f = fopen(path.GetString(), "a");
  if (f == NULL) return;
  SYSTEMTIME st; GetLocalTime(&st);
  fprintf(f, "  [click] %02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  va_list args;
  va_start(args, fmt);
  vfprintf(f, fmt, args);
  va_end(args);
  fprintf(f, "\n");
  fclose(f);
}


CAutoplayerButton::CAutoplayerButton() {
  Reset();
}

CAutoplayerButton::~CAutoplayerButton() {
}

void CAutoplayerButton::SetTechnicalName(const CString name) {
  _technical_name = name;
  CString hotkey_name = name + "hotkey";
  _hotkey.SetName(hotkey_name);

  _default_label = p_tablemap->GetTMSymbol(_technical_name + "defaultlabel").MakeLower();

  if (p_tablemap->GetTMSymbol(_technical_name + "clickmethod").MakeLower() == "double") {
    _click_method = BUTTON_DOUBLECLICK;
  } else if (p_tablemap->GetTMSymbol(_technical_name + "clickmethod").MakeLower() == "nothing") {
    _click_method = BUTTON_NOTHING;
  } else {
    _click_method = BUTTON_SINGLECLICK;
  }
}

void CAutoplayerButton::Reset() {
  _label = "";
  _technical_name = "";
  _button_type = kUndefined;
  SetClickable(false);
}

bool CAutoplayerButton::Click() {
  if (_clickable) {
    // Try to send a hotkey first, if specified in tablemap
    if (_hotkey.PressHotkey()) {
      write_log(Preferences()->debug_autoplayer(), "[CasinoInterface] Pressed hotkey for button button %s\n", _label);
      return true;
    }
    // Lookup the region or template
	RECT button_region, zero_rect = RECT{ 0 };
	CString area_name;
	bool area_found = false;
	RMapCI	r_iter = p_tablemap->r$()->end();
	for (int i = 0; i < k_max_area_buttons_zone; ++i) {
		area_name.Format("area_buttons_zone%c", HexadecimalChar(i));
		r_iter = p_tablemap->r$()->find(area_name);
		if (r_iter != p_tablemap->r$()->end()) {
			int r_width = r_iter->second.right - r_iter->second.left;
			int r_height = r_iter->second.bottom - r_iter->second.top;
			if (r_width > 0 && r_height > 0) {
				area_found = true;
				AutoOcr()->GetDetectTemplateResult(area_name, _technical_name, &button_region);
				if (!EqualRect(&button_region, &zero_rect))
					break;
			}
		}
	}
	if (!area_found)
		p_tablemap->GetTMRegion(_technical_name, &button_region);
    HWND hwnd = (p_autoconnector != NULL) ? p_autoconnector->attached_hwnd() : NULL;
    bool empty_rect = EqualRect(&button_region, &zero_rect);
    const char *method = (BUTTON_NOTHING == _click_method) ? "NOTHING"
                       : (BUTTON_DOUBLECLICK == _click_method) ? "DOUBLE" : "SINGLE";
    ButtonDebugLog("name=%s label=\"%s\" method=%s rect=(%d,%d,%d,%d) center=(%d,%d) empty=%d hwnd=0x%p",
      _technical_name.GetString(), _label.GetString(), method,
      button_region.left, button_region.top, button_region.right, button_region.bottom,
      (button_region.left + button_region.right) / 2, (button_region.top + button_region.bottom) / 2,
      empty_rect ? 1 : 0, hwnd);
    if (BUTTON_NOTHING == _click_method) {
      write_log(Preferences()->debug_autoplayer(), "[CAutoplayerButton] Doing nothing on this button [%s] [%s]\n", _label, _technical_name);
    } else if (BUTTON_DOUBLECLICK == _click_method) {
      // double click the button if needed
      p_casino_interface->DoubleClickRect(button_region);
      write_log(Preferences()->debug_autoplayer(), "[CAutoplayerButton] Clicked button [%s] [%s]\n", _label, _technical_name);
    } else {
      // Otherwise: click the button the normal way
      p_casino_interface->ClickRect(button_region);
      write_log(Preferences()->debug_autoplayer(), "[CAutoplayerButton] Clicked button [%s] [%s]\n", _label, _technical_name);
    }
    return true;
  } else {
    ButtonDebugLog("name=%s label=\"%s\" NOT CLICKABLE -> Click() returns false",
      _technical_name.GetString(), _label.GetString());
    write_log(Preferences()->debug_autoplayer(), "[CAutoplayerButton] Could not click button %s. Either undefined or not visible.\n", _label);
    return false;
  }
}

void CAutoplayerButton::SetClickable(bool clickable) {
  _clickable = clickable;
}

void CAutoplayerButton::SetState(const CString state) {
  CString button_state_lower_case = state;
  button_state_lower_case.MakeLower();
  if (button_state_lower_case.Left(4) == "true"
      || button_state_lower_case.Left(2) == "on"
      || button_state_lower_case.Left(3) == "yes"
      || button_state_lower_case.Left(7) == "checked"
      || button_state_lower_case.Left(3) == "lit") {
    SetClickable(true);
  } else {
    SetClickable(false);
  }
}

void CAutoplayerButton::SetLabel(const CString label) {
  _label = label;
  PrecomputeButtonType();
}

bool CAutoplayerButton::IsLabelAllin() {
  return p_string_match->IsStringAllin(_label);
}

bool CAutoplayerButton::IsLabelRaise() {
  CString s_lower_case = _label.MakeLower();
  s_lower_case = s_lower_case.Left(5);
  return (s_lower_case == "raise"
    || s_lower_case == "ra1se"
    || s_lower_case == "ralse"
    || s_lower_case.Left(3) == "bet"
    // Last occurence of swag for backward compatibility
    || s_lower_case.Left(4) == "swag");
}

bool CAutoplayerButton::IsLabelCall() {
  CString s_lower_case = _label.MakeLower();
  s_lower_case = s_lower_case.Left(4);
  return (s_lower_case == "call" || s_lower_case == "caii" || s_lower_case == "ca11");
}

bool CAutoplayerButton::IsLabelCheck() {
  CString s_lower_case = _label.MakeLower();
  s_lower_case = s_lower_case.Left(5);
  return (s_lower_case == "check" || s_lower_case == "cheok");
}

bool CAutoplayerButton::IsLabelFold() {
  CString s_lower_case = _label.MakeLower();
  s_lower_case = s_lower_case.Left(4);
  return (s_lower_case == "fold" || s_lower_case == "fo1d" || s_lower_case == "foid");
}

bool CAutoplayerButton::IsLabelAutopost() {
  CString s_lower_case = _label;
  s_lower_case.Remove(' ');
  s_lower_case.Remove('-');
  s_lower_case.MakeLower();
  s_lower_case = s_lower_case.Left(8);
  return (s_lower_case == "autopost" || s_lower_case == "aut0p0st");
}

bool CAutoplayerButton::IsLabelSitin() {
  CString s_lower_case = _label;
  s_lower_case.MakeLower();
  s_lower_case.Remove(' ');
  s_lower_case.Remove('-');
  s_lower_case.Remove('\'');
  // Strip the OCR junk that the leading capital "I" of "I Am Back" turns into. Tesseract renders that
  // lone upright stroke as a pipe / bracket / exclamation far more often than as an "I", and none of
  // those were removed here -- so the label came through as "|amback", matched nothing, the button was
  // never classified as Sit-In, and a sat-out bot blinded off with the button right there on screen.
  s_lower_case.Remove('|');
  s_lower_case.Remove('!');
  s_lower_case.Remove('[');
  s_lower_case.Remove(']');
  s_lower_case.Remove('(');
  s_lower_case.Remove(')');
  s_lower_case.Remove('.');
  s_lower_case.Remove(',');
  s_lower_case.Remove(':');
  // The standalone capital "I" of "I Am Back" is a bare vertical stroke, and Tesseract
  // reads it as a pipe (measured: the i9label region OCRs as "| Am Back") or, less often,
  // as an exclamation mark. Drop those so the leading glyph can't decide the match.
  s_lower_case.Remove('|');
  s_lower_case.Remove('!');
  // "I am back" / "I'm back" is this client's Sit-In button. OCR often reads the
  // leading "I" as "1" (seen as "1AmBack"), so accept that garble too.
  // OCR often drops the leading "I " of "I am back" (seen scraped as "AmBack"), and the
  // leading I<->1 garble, so accept those variants too -- otherwise the Sit-In button isn't
  // recognized and a sat-out bot can never click "I Am Back" and just blinds out.
  if (s_lower_case == "iamback" || s_lower_case == "1amback"
      || s_lower_case == "imback" || s_lower_case == "1mback"
      || s_lower_case == "amback" || s_lower_case == "mback"
      || s_lower_case == "lamback") {
    return true;
  }
  // "Rejoin" is the SAME control under a different caption: ACR shows "I Am Back" on some tables and
  // "Rejoin" on others (observed on the s10 PLO tables, hero seat reading SITTING OUT). Recognising
  // it here means any map that draws the region gets Sit-In behaviour without also needing an
  // i<N>buttondefaultlabel, and the 'I' indicator lights either way. "rejo1n" is the usual OCR
  // garble of the second i.
  if (s_lower_case == "rejoin" || s_lower_case == "rejo1n" || s_lower_case == "rej0in") {
    return true;
  }
  s_lower_case = s_lower_case.Left(5);
  return (s_lower_case == "sitin" || s_lower_case == "s1t1n");
}

bool CAutoplayerButton::IsLabelSitout() {
  CString s_lower_case = _label;
  s_lower_case.MakeLower();
  s_lower_case.Remove(' ');
  s_lower_case.Remove('-');
  s_lower_case = s_lower_case.Left(6);
  return (s_lower_case == "sitout" || s_lower_case == "s1tout" || s_lower_case == "sit0ut" || s_lower_case == "s1t0ut");
}

bool CAutoplayerButton::IsLabelLeave() {
  return (_label.MakeLower().Left(5) == "leave");
}

bool CAutoplayerButton::IsLabelRematch() {
  return (_label.MakeLower().Left(7) == "rematch");
}

bool CAutoplayerButton::IsLabelPrefold() {
  return (_label.MakeLower().Left(7) == "prefold");
}

bool CAutoplayerButton::IsNameI86() {
  return (_technical_name.Left(3).MakeLower() == "i86" || _technical_name.Left(4).MakeLower() == "spam");
}


// We precompute the button-type from the label, because their was a raise-condition
// when COpenHoldemView::UpdateDisplay(), triggered by a timer,
// accessed the button-labels (non-elementary data)
// which could at the same time be changed by the scraper (part of the heart-beat-).
// We could have added mutexes, but were not sure about performance,
// so we switched to an atomic data-type without pointers instead.
// True when the label region produced REAL TEXT that matches no button we recognise -- i.e. OCR
// gave us something and it is meaningless. An EMPTY (or near-empty) label is different and must not
// land here: a map can legitimately define a button with no label region at all and rely on
// i<N>buttondefaultlabel (that is how the Rejoin/sit-in button is wired), and a momentarily blank
// scrape of a real button must keep working too.
bool CAutoplayerButton::LabelIsUnreadable() {
  CString s = _label;
  s.Trim();
  if (s.GetLength() < 2) return false;      // blank / single stray glyph -> use the default label
  int alnum = 0;
  for (int i = 0; i < s.GetLength(); ++i) {
    if (isalnum((unsigned char)s[i])) ++alnum;
  }
  return (alnum >= 2);                      // real characters, yet nothing above matched it
}

void CAutoplayerButton::PrecomputeButtonType() {
  if (IsLabelAllin()) {
    _button_type = k_autoplayer_function_allin;
  } else if (IsLabelRaise()) {
    _button_type = k_autoplayer_function_raise;
  } else if (IsLabelCall()) {
    _button_type = k_autoplayer_function_call;
  } else if (IsLabelCheck()) {
    _button_type = k_autoplayer_function_check;
  } else if (IsLabelFold()) {
    _button_type = k_autoplayer_function_fold;
  } else if (IsLabelAutopost()) {
    _button_type = k_hopper_function_autopost;
  } else if (IsLabelSitin()) {
    _button_type = k_hopper_function_sitin;
  } else if (IsLabelSitout()) {
    _button_type = k_hopper_function_sitout;
  } else if (IsLabelLeave()) {
    _button_type = k_hopper_function_leave;
  } else if (IsLabelRematch()) {
    _button_type = k_hopper_function_rematch;
  } else if (IsLabelPrefold()) {
    _button_type = k_standard_function_prefold;
  } else if (IsNameI86()) {
    _button_type = k_button_i86;
  } else {
    // NOTE: an unrecognised scraped label DOES fall through to the default label, deliberately.
    //
    // A guard was tried here that refused the fallback whenever the label scraped real-but-unknown
    // text, on the theory that we then do not know which control we are looking at. Measured against
    // a live session, that theory was wrong and the guard was harmful: EVERY unknown label came from
    // one region -- i4button, the Check button -- whose label OCRs badly and produced 25+ spellings
    // of the same word (meck, heck, neck, sneck, hack, shack, whack, angcit, angchk, ahgcit, ...).
    // Refusing the fallback did not prevent a mis-press; it stopped the bot CHECKING at all, and it
    // sat out hands until they timed out.
    //
    // The default label is the map author's statement of what a region is. Noisy OCR on top of it is
    // evidence about the glyphs, not about the button's identity.
    /* No or wrongly scraped value, apply default label, if any */
    if (_default_label == "allin" || _default_label == "max") {
      _button_type = k_autoplayer_function_allin;
    } else if (_default_label == "raise" || _default_label == "bet") {
      _button_type = k_autoplayer_function_raise;
    } else if (_default_label == "call") {
      _button_type = k_autoplayer_function_call;
    } else if (_default_label == "check") {
      _button_type = k_autoplayer_function_check;
    } else if (_default_label == "fold") {
      _button_type = k_autoplayer_function_fold;
    } else if (_default_label == "autopost") {
      _button_type = k_hopper_function_autopost;
    } else if (_default_label == "sitin") {
      _button_type = k_hopper_function_sitin;
    } else if (_default_label == "sitout") {
      _button_type = k_hopper_function_sitout;
    } else if (_default_label == "leave") {
      _button_type = k_hopper_function_leave;
    } else if (_default_label == "rematch") {
      _button_type = k_hopper_function_rematch;
    } else if (_default_label == "prefold") {
      _button_type = k_standard_function_prefold;
    } else {
      _button_type = kUndefined;
      write_log(Preferences()->debug_autoplayer(), "[CasinoInterface] WARNING! Unknown button type [%s] [%s]\n", _label, _default_label);
    }
  }
}
