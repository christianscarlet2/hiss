//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Checking tablepoints on connection,
//   deciding if OH should reconnect if table-theme changes
//
//******************************************************************************

#include "stdafx.h"
#include "CTablepointChecker.h"

#include "CAutoConnector.h"

#include "CScraper.h"
#include "CTableMapLoader.h"
#include "..\CTransform\CTransform.h"
#include "..\Shared\WindowCapture.h"

int CTablepointChecker::_number_of_mismatches_the_last_N_heartbeats = 0;

CTablepointChecker::CTablepointChecker() {
}

CTablepointChecker::~CTablepointChecker() {
}

bool CTablepointChecker::CheckTablepoints(HWND window_candidate, int tablemap_index) {
  write_log(Preferences()->debug_tablepoints(), "[CTablepointChecker] CheckTablepoints()\n");
  // Function for checking tablepoints of not connected tables.
  // taking an extra screenshot
  // !! Might be reafactored if we manage to change the scraper
  // to work for not connected tables too.
  BYTE *pBits = NULL;
  BYTE alpha = 0, red = 0, green = 0, blue = 0;
  int	width = 0, height = 0;
  int x = 0, y = 0;
  HDC	    hdcCompatible = NULL;
  HBITMAP	hbmScreen = NULL, hOldScreenBitmap = NULL;
  CTransform	trans;
  if (tablemap_connection_data[tablemap_index].TablePointCount > 0) {
    hbmScreen = CaptureCompositedClientBitmap(window_candidate, &width, &height);
    if (hbmScreen == NULL) {
      write_log(k_always_log_errors, "[CTablepointChecker] Could not capture candidate window\n");
      return false;
    }
    hdcCompatible = CreateCompatibleDC(NULL);
    hOldScreenBitmap = (HBITMAP)SelectObject(hdcCompatible, hbmScreen);

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmi.bmiHeader.biSizeImage = width * height * 4;
    pBits = new BYTE[bmi.bmiHeader.biSizeImage];
    if (GetDIBits(hdcCompatible, hbmScreen, 0, height, pBits, &bmi, DIB_RGB_COLORS) == 0) {
      delete[]pBits;
      SelectObject(hdcCompatible, hOldScreenBitmap);
      DeleteObject(hbmScreen);
      DeleteDC(hdcCompatible);
      write_log(k_always_log_errors, "[CTablepointChecker] Could not read candidate window pixels\n");
      return false;
    }

    for (int i = 0; i<tablemap_connection_data[tablemap_index].TablePointCount; i++) {
      bool good_table_points = true;
      x = tablemap_connection_data[tablemap_index].TablePoint[i].left;
      y = tablemap_connection_data[tablemap_index].TablePoint[i].top;
      if ((x < 0) || (y < 0) || (x >= width) || (y >= height)) {
        write_log(Preferences()->debug_tablepoints(), "[CTablepointChecker] tablepoint%i outside screenshot bounds\n", i);
        good_table_points = false;
      }
      if (!good_table_points) {
        delete[]pBits;
        SelectObject(hdcCompatible, hOldScreenBitmap);
        DeleteObject(hbmScreen);
        DeleteDC(hdcCompatible);
        write_log(Preferences()->debug_tablepoints(), "[CTablepointChecker] Not all tablepoints match.\n");
        return false;
      }
      int pbits_loc = y*width * 4 + x * 4;
      alpha = pBits[pbits_loc + 3];
      red = pBits[pbits_loc + 2];
      green = pBits[pbits_loc + 1];
      blue = pBits[pbits_loc + 0];
      COLORREF Color = tablemap_connection_data[tablemap_index].TablePoint[i].color;
      // positive radius
      if (tablemap_connection_data[tablemap_index].TablePoint[i].radius >= 0) {
        if (!trans.IsInARGBColorCube((Color >> 24) & 0xff, // function GetAValue() does not exist
          GetRValue(Color),
          GetGValue(Color),
          GetBValue(Color),
          tablemap_connection_data[tablemap_index].TablePoint[i].radius,
          alpha, red, green, blue)) 
        {
          write_log(Preferences()->debug_tablepoints(), "[CTablepointChecker] tablepoint%i failed\n", i);
          good_table_points = false;
        }
      }
      // negative radius (logical not)
      else {
        if (trans.IsInARGBColorCube((Color >> 24) & 0xff, // function GetAValue() does not exist
          GetRValue(Color),
          GetGValue(Color),
          GetBValue(Color),
          -tablemap_connection_data[tablemap_index].TablePoint[i].radius,
          alpha, red, green, blue))
        {
          write_log(Preferences()->debug_tablepoints(), "[CTablepointChecker] tablepoint%i failed\n", i);
          good_table_points = false;
        }
      }
      if (!good_table_points) {
        delete[]pBits;
        SelectObject(hdcCompatible, hOldScreenBitmap);
        DeleteObject(hbmScreen);
        DeleteDC(hdcCompatible);
        write_log(Preferences()->debug_tablepoints(), "[CTablepointChecker] Not all tablepoints match.\n");
        return false;
      }
    }
    delete[]pBits;
    SelectObject(hdcCompatible, hOldScreenBitmap);
    DeleteObject(hbmScreen);
    DeleteDC(hdcCompatible);
  }
  write_log(Preferences()->debug_tablepoints(), "[CTablepointChecker] All tablepoints match.\n");
  return true;
}

bool CTablepointChecker::CheckTablepointsOfCurrentTablemap() {
 write_log(Preferences()->debug_tablepoints(), "[CTablepointChecker] CheckTablepointsOfCurrentTablemap()\n");
  // Function for re-checking tablepoints of already connected tables.
  // Not using the function "CheckTablepoints()" above,
  // because that takes an extra screenshot
  // for each heartbeat and instance.
  // We use p_scraper->EvaluateRegion() to re-use
  // the already existing screenshot of the scraper.
  assert(p_autoconnector != NULL);
  write_log(Preferences()->debug_alltherest(), "[CTablePointChecker] location Johnny_4\n");
  assert(p_autoconnector->IsConnectedToAnything());
  for (int i = 0; i < k_max_number_of_tablepoints; ++i) {
    assert(p_tablemap != NULL);
    CString tablepointX;
    tablepointX.Format("tablepoint%i", i);
    CString tablepoint_result;
    if (p_tablemap->ItemExists(tablepointX)) {
      assert(p_scraper != NULL);
      p_scraper->EvaluateRegion(tablepointX, &tablepoint_result);
      if (tablepoint_result != "true") {
        write_log(Preferences()->debug_tablepoints(), "[CTablepointChecker] tablepoint%i failed\n", i);
        write_log(Preferences()->debug_tablepoints(), "[CTablepointChecker] Not all tablepoints match.\n");
        return false;
      }
    }
  }
  write_log(Preferences()->debug_tablepoints(), "[CTablepointChecker] All tablepoints match.\n");
  return true;
}

bool CTablepointChecker::TablepointsMismatchedTheLastNHeartbeats() {
  if (CheckTablepointsOfCurrentTablemap()) {
    _number_of_mismatches_the_last_N_heartbeats = 0;
  } else {
    ++_number_of_mismatches_the_last_N_heartbeats;
  }
  write_log(Preferences()->debug_tablepoints(), "[CTablepointChecker] Tablepoints failed %i heartbeats in a row\n",
    _number_of_mismatches_the_last_N_heartbeats);
  if (_number_of_mismatches_the_last_N_heartbeats > kMaxToleratedHeartbeatsWithMismatchingTablepointsBeforeDisconnect) {
    return true;
  }
  return false;
}
