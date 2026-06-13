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

#include "StdAfx.h"
#include "CTablePositioner.h"

#include "CAutoConnector.h"

#include "CSessionCounter.h"
#include "CSharedMem.h"
#include "../CTablemap/CTableMapAccess.h"
#include "../CTablemap/CTablemapDB.h"

#include "WinDef.h"
#include "..\DLLs\WindowFunctions_DLL\window_functions.h"
#include "Winuser.h"

CTablePositioner *p_table_positioner = NULL;

CTablePositioner::CTablePositioner() {
  SystemParametersInfo(SPI_GETWORKAREA, NULL, &_desktop_rectangle, NULL);
}

CTablePositioner::~CTablePositioner() {
}

// To be called once after connection
void CTablePositioner::PositionMyWindow() {		
	// Build list of poker tables (child windows)
	// Use the shared memory (auto-connector) for that. 
	HWNDs_of_child_windows = p_sharedmem->GetDenseListOfConnectedPokerWindows();
	_number_of_tables = p_sharedmem->SizeOfDenseListOfAttachedPokerWindows();
  GetWindowSize(p_autoconnector->attached_hwnd(),
    &_table_size_x, &_table_size_y);
  if (_number_of_tables <= 0)	{
    write_log(Preferences()->debug_table_positioner(), "[CTablePositioner] PositionMyWindow() No connected tables. going to return.\n");
		// Do nothing if there are 0 tables connected.
		// Actually an empty list of tables consists of only NULLs,
		// but if MicroSofts table-arranging functions see a NULL
		// they will arrange all windows at the desktop.
		// That's not what we want.
		return;
	}
  if (p_tablemap->islobby()) { 
    write_log(Preferences()->debug_table_positioner(), "[CTablePositioner] PositionMyWindow() Going to handle the lobby...\n");
    MoveWindowToTopLeft(p_autoconnector->attached_hwnd());
  } else if (p_tablemap->ispopup()) { 
    write_log(Preferences()->debug_table_positioner(), "[CTablePositioner] PositionMyWindow() Ignoring connected popup...\n");
  } else if (Preferences()->table_positioner_options() == k_position_tables_tiled) {
		write_log(Preferences()->debug_table_positioner(), "[CTablePositioner] PositionMyWindow() Going to tile %d windows...\n", _number_of_tables);	
    TileSingleWindow(p_autoconnector->attached_hwnd(), HWNDs_of_child_windows);
	}	else if (Preferences()->table_positioner_options() == k_position_tables_cascaded) {
		write_log(Preferences()->debug_table_positioner(), "[CTablePositioner] PositionMyWindow() Going to cascade %d windows...\n", _number_of_tables);
    CascadeSingleWindow(p_autoconnector->attached_hwnd(), p_sessioncounter->session_id());
	}	else {
		// Preferences()->table_positioner_options() == k_position_tables_never
		write_log(Preferences()->debug_table_positioner(), "[CTablePositioner] PositionMyWindow() Not doing anything because of Preferences()->\n");
	}
}

// Key the saved placement on the loaded tablemap's name (so each table type keeps its
// own spot); fall back to "default" when unknown.
static CString TableWindowField() {
  CString f = (p_tablemap != NULL) ? p_tablemap->filename() : CString("");
  f.Trim();
  if (f.IsEmpty()) f = "default";
  return f;
}

bool CTablePositioner::RestoreSavedPlacement() {
  HWND hwnd = p_autoconnector->attached_hwnd();
  if (hwnd == NULL || !::IsWindow(hwnd) || p_tablemap_db == NULL) {
    return false;
  }
  CString v = p_tablemap_db->GetSettingString("table_window", TableWindowField());
  if (v.IsEmpty()) {
    return false;
  }
  int parts[4] = { 0, 0, 0, 0 };
  int n = 0, pos = 0;
  while (n < 4) {
    int comma = v.Find(',', pos);
    CString tok = (comma < 0) ? v.Mid(pos) : v.Mid(pos, comma - pos);
    parts[n++] = atoi(CStringA(tok));
    if (comma < 0) break;
    pos = comma + 1;
  }
  if (n != 4) {
    return false;
  }
  int x = parts[0], y = parts[1], w = parts[2], h = parts[3];
  if (w < 50 || h < 50) {
    return false;   // never restore a collapsed window
  }
  // Drop placements that no longer land on any monitor (e.g. an unplugged screen).
  RECT want = { x, y, x + w, y + h };
  if (MonitorFromRect(&want, MONITOR_DEFAULTTONULL) == NULL) {
    return false;
  }
  ::SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
  write_log(Preferences()->debug_table_positioner(),
    "[CTablePositioner] Restored table window to %d,%d %dx%d from DB.\n", x, y, w, h);
  return true;
}

void CTablePositioner::SaveCurrentPlacement() {
  HWND hwnd = p_autoconnector->attached_hwnd();
  if (hwnd == NULL || !::IsWindow(hwnd) || p_tablemap_db == NULL) {
    return;
  }
  RECT r;
  if (!::GetWindowRect(hwnd, &r)) {
    return;
  }
  CString v;
  v.Format("%d,%d,%d,%d", (int)r.left, (int)r.top, (int)(r.right - r.left), (int)(r.bottom - r.top));
  p_tablemap_db->SetSettingString("table_window", TableWindowField(), v);
}

// To be called once per heartbeat
void CTablePositioner::AlwaysKeepPositionIfEnabled() {
  // Disabled: the connected window is intentionally never repositioned (we don't
  // set a target position on connect, so re-pinning here would snap it to 0,0 /
  // monitor 1). Leave the scrcpy/phone-mirror window wherever the user put it.
  return;
  if (!Preferences()->table_positioner_always_keep_position()
    || (p_autoconnector->attached_hwnd() == NULL)) {
    write_log(Preferences()->debug_table_positioner(), "[CTablePositioner] AlwaysKeepPositionIfEnabled() disabled or not connected\n");
    return;
  }
  RECT current_position;
  GetWindowRect(p_autoconnector->attached_hwnd(), &current_position);
  if ((current_position.left == _table_position.left)
    && (current_position.top == _table_position.top)) {
    write_log(Preferences()->debug_table_positioner(), "[CTablePositioner] AlwaysKeepPositionIfEnabled() position is good\n");
  }
  else {
    write_log(Preferences()->debug_table_positioner(), "[CTablePositioner] AlwaysKeepPositionIfEnabled() restoring old position\n");
    MoveWindow(p_autoconnector->attached_hwnd(),
      _table_position.left, _table_position.top);
  }
}

void CTablePositioner::ResizeToTargetSize() {
  int width;
  int height;
  p_tablemap_access->GetClientSize("targetsize", &width, &height);
  if (width <= 0 || height <= 0) {
    write_log(Preferences()->debug_table_positioner(), "[CTablePositioner] target size <= 0\n");
    return;
  }
  ResizeToClientSize(p_autoconnector->attached_hwnd(), width, height);
}