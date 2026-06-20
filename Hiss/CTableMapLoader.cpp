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
#include "CTableMaploader.h"

#include "CAutoConnector.h"
#include "CAutoOcr.h"
#include "CFileSystemMonitor.h"
#include "CHeartbeatThread.h"
#include "CScraper.h"
#include "DialogScraperOutput.h"

#include "CTablemapCompletenessChecker.h"
#include "CTablePointChecker.h"
#include "..\CTablemap\CTablemapAccess.h"
#include "..\CTablemap\CTablemapDB.h"
#include "..\DLLs\Files_DLL\Files.h"
#include "..\DLLs\WindowFunctions_DLL\window_functions.h"
#include "..\Shared\WindowCapture.h"
#include "OpenHoldem.h"

CTableMapLoader *p_tablemap_loader = NULL;

std::map<int, t_tablemap_connection_data> tablemap_connection_data;

CTableMapLoader::CTableMapLoader() {
	write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] CTableMapLoader()\n");
	tablemaps_in_scraper_folder_already_parsed = false;
	// Parse all tablemaps once on startup.
	// We want to avoid heavy workload in the connect()-function.
	ParseAllTableMapsToLoadConnectionData();
	CheckForDuplicatedTablemaps(); 
}

CTableMapLoader::~CTableMapLoader() {
  tablemap_connection_data.clear();
}

void CTableMapLoader::ParseAllTableMapsToLoadConnectionData(CString scraper_directory) {
	write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] ParseAllTableMapsToLoadConnectionData: %s\n", scraper_directory);
  CString wildcard;
  wildcard.Format("%s\\*", scraper_directory);
  CFileFind	hFile;
	BOOL bFound = hFile.FindFile(wildcard);
	while (bFound) {
		bFound = hFile.FindNextFile();
    // Formerly there has been a check /hFile.GetFilePath() != p_tablemap->filepath()) 
    // but if we want to reload, then everything
    write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Looking at %s\n",
      hFile.GetFilePath());
    if (hFile.IsDots()) {
      write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Skipping dots\n");
      // Ignore link to current directory and to parent-directory
      continue;
    } else if (hFile.IsDirectory()) {
      // Traverse sub-directories recursively
      write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Diving into directory\n");
      ParseAllTableMapsToLoadConnectionData(hFile.GetFilePath());
      continue;
    } else if (hFile.GetFilePath().Right(3).MakeLower() != ".tm") {
      // Not a tablemap
      write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Not a tablemap\n");
      continue;
    }
    write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Loading file\n");
		int ret = p_tablemap->LoadTablemap(hFile.GetFilePath().GetString());
		if (ret == SUCCESS)	{
			ExtractConnectionDataFromCurrentTablemap(p_tablemap);
      CTablemapCompletenessChecker tablemap_completeness_checker;
      tablemap_completeness_checker.VerifyMap();
			write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Number of TMs loaded: %d\n", _number_of_tablemaps_loaded);
		}
	}
}

void CTableMapLoader::ParseAllTableMapsToLoadConnectionData() {
	write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] ParseAllTableMapsToLoadConnectionData (from hiss database)\n");
  tablemap_connection_data.clear();
  _number_of_tablemaps_loaded = 0;
  // Tablemaps now live in the PostgreSQL "hiss" database, not in *.tm files.
  // Enumerate every tablemap, load each into p_tablemap and extract its
  // connection data exactly as before (the matching logic is unchanged).
  std::vector<STablemapDBInfo> tablemaps;
  if (!p_tablemap_db->ListTablemaps(&tablemaps)) {
    write_log(Preferences()->debug_tablemap_loader(),
      "[CTablemapLoader] Could not list tablemaps from the database: %s\n",
      p_tablemap_db->LastError().GetString());
  } else {
    for (size_t i = 0; i < tablemaps.size(); ++i) {
      write_log(Preferences()->debug_tablemap_loader(),
        "[CTablemapLoader] Loading tablemap [%s] from database\n", tablemaps[i].name.GetString());
      int ret = p_tablemap_db->LoadTablemapFromDB(tablemaps[i].name, p_tablemap);
      if (ret == SUCCESS) {
        ExtractConnectionDataFromCurrentTablemap(p_tablemap);
        CTablemapCompletenessChecker tablemap_completeness_checker;
        tablemap_completeness_checker.VerifyMap();
        write_log(Preferences()->debug_tablemap_loader(),
          "[CTablemapLoader] Number of TMs loaded: %d\n", _number_of_tablemaps_loaded);
      }
    }
  }
	tablemaps_in_scraper_folder_already_parsed = true;
}

bool CTableMapLoader::tablemap_connection_dataAlreadyStored(CString TablemapFilePath) {
  assert(TablemapFilePath != "");
	for (int i=0; i<_number_of_tablemaps_loaded; i++)	{
    assert(tablemap_connection_data[i].FilePath != "");
		if (tablemap_connection_data[i].FilePath == TablemapFilePath)	{
			write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] tablemap_connection_dataAlreadyStored [%s] [true]\n", TablemapFilePath);
			return true;
		}
	}
	write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] tablemap_connection_dataAlreadyStored [%s] [false]\n", TablemapFilePath);
	return false;
}

void CTableMapLoader::CheckForDuplicatedTablemaps() {
	CString error_message = "";
	for (int i=0; i<_number_of_tablemaps_loaded; i++)	{
		for (int j=i+1; j<_number_of_tablemaps_loaded; j++)	{
			if	((tablemap_connection_data[i].SiteName == tablemap_connection_data[j].SiteName)
				&& (tablemap_connection_data[i].TitleText == tablemap_connection_data[j].TitleText))
			{
				write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] tablemap_connection_dataDuplicated [%s, %s]  [true]\n", 
					tablemap_connection_data[i].SiteName, tablemap_connection_data[i].TitleText);
				error_message.Format("It seems you have multiple tablemaps with the same SiteName and TitleText in the 'hiss' database.\n\n"\
					"SiteName = %s\n"\
					"TitleText = %s\n\n"\
					"Conflicting tablemaps: %s  and  %s\n\n"\
					"This will cause problems as the autoconnector won't be able to decide which one to use.\n"\
					"Delete one of these tablemaps in Vision (File > Open from Database), or give them different "\
					"SiteName/TitleText values.\n",
					tablemap_connection_data[i].SiteName, tablemap_connection_data[i].TitleText,
					tablemap_connection_data[i].FilePath, tablemap_connection_data[j].FilePath);
				MessageBox_Error_Warning((LPCTSTR) error_message, 
					"Warning! Duplicate SiteName and TitleText");
			}
		}
	}
}

void CTableMapLoader::ExtractConnectionDataFromCurrentTablemap(CTablemap *cmap) {
	write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] ExtractConnectionDataFromCurrentTablemap(): %s\n", cmap->filepath());
	write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] number_of_tablemaps_loaded: %d\n", _number_of_tablemaps_loaded);
	// Avoiding to store the data twice, e.g. when we load a known TM manually
	if (tablemap_connection_dataAlreadyStored(cmap->filepath())) 	{
		write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] ExtractConnectionDataFromCurrentTablemap(): already stored; early exit\n");
		return;
	}
	tablemap_connection_data[_number_of_tablemaps_loaded].FilePath = cmap->filepath();
	tablemap_connection_data[_number_of_tablemaps_loaded].SiteName = cmap->sitename();
	tablemap_connection_data[_number_of_tablemaps_loaded].TitleText = cmap->titletext();
	// Get clientsize info through TM-access-class
	p_tablemap_access->GetClientSize("clientsizemin", &tablemap_connection_data[_number_of_tablemaps_loaded].ClientSizeMinX, &tablemap_connection_data[_number_of_tablemaps_loaded].ClientSizeMinY);
	p_tablemap_access->GetClientSize("clientsizemax", &tablemap_connection_data[_number_of_tablemaps_loaded].ClientSizeMaxX, &tablemap_connection_data[_number_of_tablemaps_loaded].ClientSizeMaxY);
	// Extract title text information
	p_tablemap_access->SetTitleText("titletext", tablemap_connection_data[_number_of_tablemaps_loaded].TitleText);
	// Extract negative title texs
	p_tablemap_access->SetTitleText("!titletext", tablemap_connection_data[_number_of_tablemaps_loaded].NegativeTitleText);
	CString s = "";
	for (int i=0; i<k_max_number_of_titletexts; i++) {
		s.Format("titletext%d", i);
		p_tablemap_access->SetTitleText(s, tablemap_connection_data[_number_of_tablemaps_loaded].TitleText_0_9[i]);
		s.Format("!titletext%d", i);
		p_tablemap_access->SetTitleText(s, tablemap_connection_data[_number_of_tablemaps_loaded].NegativeTitleText_0_9[i]);		
	}
	// Extract tablepoints r$tablepoint0..9
	s = "";
	int tpcount = 0;
	for (int i=0; i<k_max_number_of_tablepoints; ++i) {
		s.Format("tablepoint%d", i);
		if (p_tablemap_access->GetTableMapRegion(s, &tablemap_connection_data[_number_of_tablemaps_loaded].TablePoint[i])) {
		  ++tpcount;
		} else {
      break;
    }
	}
	tablemap_connection_data[_number_of_tablemaps_loaded].TablePointCount = tpcount;
	++_number_of_tablemaps_loaded;
}

void CTableMapLoader::ReloadAllTablemapsIfChanged() {
  if (p_filesystem_monitor->AnyChanges()) {
    // Since OpenHoldem 4.0.0 there is no longer any possibility 
    // for manual connection; i.e. no possibility for re-connection.
    // Therefore the file-system-monitor does now dis-connect
    // if it finds a new/changed tablemap and the auto-connector-thread 
    // will thereafter reload the TMs and connect again.
    //
    // Note: This solution might lose some game-history (reset),
    // but that is perfectly acceptable for development
    // and hot-plugging of TMs won't happen in production.
    write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Tablemaps changed; going to reload.\n");
    p_autoconnector->Disconnect("tablemaps changed and need to be reloaded");
    ParseAllTableMapsToLoadConnectionData();
  }
  else {
    write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] All tablemaps unchanged; nothing to do.\n");
  }
}

void CTableMapLoader::ReloadConnectedTablemapIfSettingsChanged() {
  if (p_tablemap == NULL || p_tablemap_db == NULL) {
    return;
  }
  CString name = p_tablemap->filename();   // DB key of the connected map
  if (name.IsEmpty()) {
    return;
  }
  CString revision = p_tablemap_db->GetSettingsRevision();
  if (revision.IsEmpty()) {
    // DB probe failed; leave the cached settings in place and try again next beat.
    return;
  }
  if (_last_settings_revision.IsEmpty()) {
    // First probe of the session: the connect-time snapshot is already current,
    // so just remember the baseline without reloading.
    _last_settings_revision = revision;
    return;
  }
  if (revision == _last_settings_revision) {
    return;   // nothing changed
  }
  _last_settings_revision = revision;
  write_log(Preferences()->debug_tablemap_loader(),
    "[CTablemapLoader] DB revision changed; live-reloading connected map [%s]\n",
    name.GetString());
  // Reload the connected map's regions/transforms under the heartbeat lock so we
  // never swap the tablemap out mid-scrape. Then re-read the OCR settings (CAutoOcr
  // otherwise caches them once per session).
  //
  // IMPORTANT: LoadTablemapFromDB calls ClearTablemap(), which discards every region
  // (and thus the scraper's per-region GDI bitmaps cur_bmp/last_bmp that CreateBitmaps
  // allocated at connect). The rebuilt regions have NULL bitmaps, so we must free the
  // old ones first and re-allocate after the reload, or the next scrape dereferences
  // NULL HBITMAPs and crashes. All done inside the lock so no scrape runs in between.
  if (p_heartbeat_thread != NULL) {
    EnterCriticalSection(&p_heartbeat_thread->cs_update_in_progress);
    if (p_scraper != NULL) p_scraper->DeleteBitmaps();
    p_tablemap_db->LoadTablemapFromDB(name, p_tablemap);
    if (p_scraper != NULL) p_scraper->CreateBitmaps();
    LeaveCriticalSection(&p_heartbeat_thread->cs_update_in_progress);
  } else {
    if (p_scraper != NULL) p_scraper->DeleteBitmaps();
    p_tablemap_db->LoadTablemapFromDB(name, p_tablemap);
    if (p_scraper != NULL) p_scraper->CreateBitmaps();
  }
  AutoOcr()->LoadModelSettings();
  // Also drop the parallel-OCR worker engines so they reload the new models on the
  // next pre-pass (the serial p_auto_ocr pool was already flushed by LoadModelSettings).
  if (p_scraper != NULL) p_scraper->InvalidateParallelOcrEngines();
  // Refresh the Scraper Output dialog's region list if it is open.
  if (m_ScraperOutputDlg != NULL) {
    m_ScraperOutputDlg->UpdateDisplay();
  }
}

void CTableMapLoader::SwitchTablemapForGameTypeIfNeeded() {
  if (p_tablemap == NULL || p_tablemap_db == NULL || p_autoconnector == NULL) {
    return;
  }
  if (!p_autoconnector->IsConnectedToAnything()) {
    return;   // nothing connected -> nothing to swap
  }
  CString cur = p_tablemap->filename();   // DB key of the connected map
  if (cur.IsEmpty()) {
    return;
  }
  // base = connected name with any trailing "_omaha" stripped; the Omaha variant is base+"_omaha".
  CString base = cur;
  if (base.GetLength() > 6 && base.Right(6) == "_omaha") {
    base = base.Left(base.GetLength() - 6);
  }
  // ALWAYS load the 4-card Omaha (superset) map when one exists for this table, regardless of the
  // (mis-branded, flickering) game-type NAME. The Omaha map scrapes 4 hole cards on PLO/PLO8 and reads
  // nocard for cards 2-3 on NLH, so the game type is detected per-hand from the CARD COUNT (isomaha)
  // with NO map switching -- this eliminates the chicken-and-egg where the Hold'em map loaded at the
  // start of each PLO hand and preflop ran the Hold'em tree (PLO never raised). If no "_omaha" variant
  // exists for this table the existence check below leaves the base (Hold'em-only) map. [Emrald]
  CString want = base + "_omaha";
  if (want == cur) {
    return;   // already on the 4-card map
  }
  // The target map must actually exist in the DB, or LoadTablemapFromDB would blank the tablemap.
  bool exists = false;
  for (std::map<int, t_tablemap_connection_data>::const_iterator it = tablemap_connection_data.begin();
       it != tablemap_connection_data.end(); ++it) {
    if (it->second.FilePath == want) { exists = true; break; }
  }
  if (!exists) {
    return;   // no separate Omaha map for this table -> leave the connected map as is
  }
  // Debounce: OCR text can flicker; switch at most once every few seconds.
  static DWORD last_switch_tick = 0;
  if (GetTickCount() - last_switch_tick < 4000) {
    return;
  }
  last_switch_tick = GetTickCount();
  write_log(Preferences()->debug_tablemap_loader(),
    "[CTablemapLoader] game-type tablemap switch [%s] -> [%s] (omaha=%d)\n",
    cur.GetString(), want.GetString(), (int)g_table_is_omaha);
  // Hot-swap the connected map under the heartbeat lock, freeing/re-allocating the scraper's
  // per-region bitmaps exactly like ReloadConnectedTablemapIfSettingsChanged (LoadTablemapFromDB
  // calls ClearTablemap, so the old bitmaps must be freed first and re-created after).
  if (p_heartbeat_thread != NULL) {
    EnterCriticalSection(&p_heartbeat_thread->cs_update_in_progress);
    if (p_scraper != NULL) p_scraper->DeleteBitmaps();
    p_tablemap_db->LoadTablemapFromDB(want, p_tablemap);
    if (p_scraper != NULL) p_scraper->CreateBitmaps();
    LeaveCriticalSection(&p_heartbeat_thread->cs_update_in_progress);
  } else {
    if (p_scraper != NULL) p_scraper->DeleteBitmaps();
    p_tablemap_db->LoadTablemapFromDB(want, p_tablemap);
    if (p_scraper != NULL) p_scraper->CreateBitmaps();
  }
  AutoOcr()->LoadModelSettings();
  if (p_scraper != NULL) p_scraper->InvalidateParallelOcrEngines();
  if (m_ScraperOutputDlg != NULL) {
    m_ScraperOutputDlg->UpdateDisplay();
  }
}

CString CTableMapLoader::GetTablemapPathToLoad(int tablemap_index) {
  assert(tablemap_index >= 0);
  assert(tablemap_index < _number_of_tablemaps_loaded);
  return tablemap_connection_data[tablemap_index].FilePath;
}

// Forward declarations for the next function
// !! To be moved into an extra module one day
bool TablemapMatchesWindowBySize(int MapIndex, HWND window);
bool TablemapMatchesWindowByTitle(int MapIndex, HWND window);
bool TablemapMatchesWindowByNegativeTitles(int MapIndex, HWND window);
bool TablemapMatchesWindowByTablepoints(int MapIndex, HWND window);

// This function has to be global and can't be part of the class,
// as it has to be called by the callback-function 
// BOOL CALLBACK EnumProcTopLevelWindowList(HWND windowwnd, LPARAM lparam) 
bool Check_TM_Against_Single_Window(int MapIndex, HWND window) { 
	write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Check_TM_Against_Single_Window(..)\n");
  write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Checking map [%d] and window [%d]\n", 
    MapIndex, window);
  if (!TablemapMatchesWindowBySize(MapIndex, window)) {
    write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Size does not match\n");
    return false;
  }
  if (!TablemapMatchesWindowByTitle(MapIndex, window)) {
    write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Title does not match\n");
    return false;
  }
  if (!TablemapMatchesWindowByNegativeTitles(MapIndex, window)) {
    write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Megative title does not match\n");
    return false;
  }
  if (!TablemapMatchesWindowByTablepoints(MapIndex, window)) {
    write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Tablepoint does not match\n");
    return false;
  }
  write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Tablemap [%d] matches window [%d]\n", 
    MapIndex, window);
  return true;
}

bool TablemapMatchesWindowBySize(int MapIndex, HWND window) {
  RECT crect;
  if (!GetClientWindowCaptureRect(window, &crect)) {
    GetClientRect(window, &crect);
  }
  if (crect.right < tablemap_connection_data[MapIndex].ClientSizeMinX) return false;
  if (crect.right > tablemap_connection_data[MapIndex].ClientSizeMaxX) return false;
  if (crect.bottom < tablemap_connection_data[MapIndex].ClientSizeMinY) return false;
  if (crect.bottom > tablemap_connection_data[MapIndex].ClientSizeMaxY) return false;
  write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Good size: (%dpx, %dpx) in clientsizemin/max\n",
    crect.right, crect.bottom,
    tablemap_connection_data[MapIndex].ClientSizeMinX, tablemap_connection_data[MapIndex].ClientSizeMinY);
  return true;
}

bool TablemapMatchesWindowByTitle(int MapIndex, HWND window) {
  // Any good match counts as matching
  char text[MAX_WINDOW_TITLE] = { 0 };
  GetWindowText(window, text, sizeof(text));
  CString title = text;
  if ((tablemap_connection_data[MapIndex].TitleText != "")
    && title.Find(tablemap_connection_data[MapIndex].TitleText) != -1) {
    // Matching non-empty title-text
    write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Title [%s] matches.\n", title);
    return true;
  }
  if ((tablemap_connection_data[MapIndex].TitleText == "")
    && (title == "")) {
    // Matching empty title-text (since version 11.0.1)
    // Exact match, as partial matching makes no sense
    // This also requires tablepoints to make sure that we don't randomly
    // connect to crappy ghost-windows. The existence of tablepoints
    // gets verified by the TablemapcompletenessChecker.
    // http://www.maxinmontreal.com/forums/viewtopic.php?f=124&t=20382
    write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Empty title [%s] matches.\n", title);
    return true;
  }
  // titletext din't match
  // Check for titletext0..titletext9
  for (int i = 0; i<k_max_number_of_titletexts; i++) {
    if ((tablemap_connection_data[MapIndex].TitleText_0_9[i] != "")
      && (title.Find(tablemap_connection_data[MapIndex].TitleText_0_9[i]) != -1)) {
      write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Title [%s] matches.\n", title);
      return true;
    }
  }
  write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Title [%s] does not match.\n", title);
  return false;
}

bool TablemapMatchesWindowByNegativeTitles(int MapIndex, HWND window) {
  // Any bad match counts as not matching
  char text[MAX_WINDOW_TITLE] = { 0 };
  GetWindowText(window, text, sizeof(text));
  CString title = text;
  if ((tablemap_connection_data[MapIndex].NegativeTitleText != "")
    && title.Find(tablemap_connection_data[MapIndex].NegativeTitleText) != -1) {
    write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Failure. Title [%s] matches negative text.\n", title);
    return false;
  }
  // titletext din't match
  // Check for titletext0..titletext9
  for (int i = 0; i<k_max_number_of_titletexts; i++) {
    if ((tablemap_connection_data[MapIndex].NegativeTitleText_0_9[i] != "")
      && (title.Find(tablemap_connection_data[MapIndex].NegativeTitleText_0_9[i]) != -1)) {
      write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Failure. Title [%s] matches negative text.\n", title);
      return false;
    }
  }
  write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Success. Title [%s] does not match negative titles.\n", title);
  return true;
}

bool TablemapMatchesWindowByTablepoints(int MapIndex, HWND window) {
  // All tablepoints (if present must match
  CTablepointChecker tablepoint_checker;
  if (!tablepoint_checker.CheckTablepoints(window, MapIndex)) {
    write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Not all tablepoints match.\n");
    return false;
  }
  write_log(Preferences()->debug_tablemap_loader(), "[CTablemapLoader] Window ia a match\n");
  return true;
}
