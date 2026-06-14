//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Reading the poker-table.
//  State-less class for future multi-table support.
//  All data is now in the CTable'state container.
//
//******************************************************************************

#ifndef INC_CSCRAPER_H
#define INC_CSCRAPER_H

#include <stdint.h>
#include "../CTablemap/CTablemap.h"
#include "CSpaceOptimizedGlobalObject.h"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <cctype>


class CScraper : public CSpaceOptimizedGlobalObject {
  friend class CLazyScraper;
  friend class CAutoConnector;
 public:
	// public functions and accessors
	CScraper(void);
	~CScraper(void);
 public:
  // For replay-frames
	const HBITMAP	entire_window_cur() { return _entire_window_cur; }
 public:
  // For scraping custom regions at the DLL-level
  bool EvaluateRegion(CString name, CString *result);
  void EvaluateTrueFalseRegion(bool *result, const CString name);
  // Observer mode: when the "p3observer" region scrapes "true", every p3<x>/u3<x>
  // region request is transparently served from "p3observer_<x>" (when that region
  // exists), so p3's scraped values come from the observer regions. Refreshed once
  // per scrape frame; ObserverActive() exposes the cached state to other code.
  void RefreshObserverState();
  bool ObserverActive() const { return _observer_active; }
 public:
  bool IsCommonAnimation();
  // Public so a live DB tablemap-reload can re-allocate per-region bitmaps after the
  // region map is rebuilt (ClearTablemap discards them).
  void CreateBitmaps(void);
  void DeleteBitmaps(void);
  // Optional parallel OCR pre-pass (OFF by default; "parallel_workers"/"hiss_ocr").
  // When enabled it OCRs all AutoOcr ("A") regions across worker threads up-front
  // into _ocr_cache, which EvaluateRegion then reads instead of OCRing serially.
  void PreOcrParallel();
  // Discard the parallel-OCR worker engines so they reload model settings on the
  // next pre-pass (used when OCR models change live, no restart needed).
  void InvalidateParallelOcrEngines();
  // Scarlet Beast server-scrape: pull the configured table's live state from
  // poker.scarletbeast.com and write players/cards/bets/board/pot directly into
  // CTableState (so the React display and the whole symbol pipeline see it),
  // instead of screen-scraping. Returns true if a payload was applied.
  bool ScrapeFromScarletBeastServer();
 protected:
	bool IsIdenticalScrape();
 protected:
	void ScrapeDealer();
	void ScrapeButtons(CString area_name, CString needed_buttons);
	void ScrapeActionButtons();
	void ScrapeActionButtonLabels();
	void ScrapeInterfaceButtons();
	void ScrapeBetpotButtons();
	void ClearAllPlayerNames();
	void ScrapeName(const int chair);
	void ScrapePlayerCards(int chair);
	void ScrapeSlider();
	void ScrapeCommonCards();
	void ScrapeSeatedActive();
	void ScrapeBetsAndBalances();
	void ScrapeAllPlayerCards();
	void ScrapeColourCodes();
	void ScrapeMTTRegions();
 private:
	void ScrapeSeated(int chair);
	void ScrapeActive(int chair);
 private:
	int ScrapeCard(CString name);
	int ScrapeCardback(CString base_name);
	int ScrapeCardByRankAndSuit(CString base_name);
	int ScrapeCardface(CString base_name);
	int ScrapeNoCard(CString base_name);
 private:
	int CardString2CardNumber(CString card);
 private:
	// private functions and variables - not available via accessors or mutators
  CString ScrapeUPBalance(int chair, char scrape_u_else_p);
	void ScrapeBalance(const int chair);
	void ScrapeBet(const int chair);
	void ScrapePots();
	void ScrapeLimits();
	const double DoChipScrape(RMapCI r_iter);
	// MCP feed: when g_dump_scrapes_once is set, dump the full-table bitmap plus each
	// region's raw scrape (<name>_raw.bmp) and its OCR/recognition result (<name>.txt)
	// to logs\scrapes\, then clear the flag. Called once per heartbeat scrape.
	void DumpScrapesIfRequested();
	// Rolling heartbeat-frame history: every heartbeat, save the captured table window
	// to logs\frames\<epoch_ms>.bmp and prune anything older than 10 minutes. Lets us
	// look back at exactly what was on screen at the time a question/command was asked.
	void SaveHeartbeatFrame();
 public:
	// Claude/MCP transform: a region whose value Claude parses from the image (not OCR).
	// Claude posts the value via /api/set-region-value; EvaluateRegion returns it instead
	// of scraping. Thread-safe (HTTP thread writes, heartbeat thread reads).
	void SetClaudeRegionValue(CString name, CString value);
	bool GetClaudeRegionValue(CString name, CString *out);
 private:
	void PruneFramesOlderThan(unsigned long long cutoff_epoch_ms);
	bool ProcessRegion(RMapCI r_iter);
	bool IsExtendedNumberic(CString text);
	BOOL SaveHBITMAPToFile(HBITMAP hBitmap, LPCTSTR lpszFileName);

  void ResetLimitInfo();
	
 private:
#define ENT CSLock lock(m_critsec);
  void delete_entire_window_cur() { ENT DeleteObject(_entire_window_cur);}
#undef ENT
 private:
	// private variables - use public accessors and public mutators to address these
  CCritSec		m_critsec;
  // Counter of GDI objects (potential memorz leak)
  // Should be 0 at end of program -- will be checked.
  int         _leaking_GDI_objects;
  // Used for potential optimizations
  int total_region_counter;
  int identical_region_counter;
  int _frame_prune_counter;   // throttles the 10-min frame-history prune scan
  std::map<CString, CString> _claude_region_values;   // Claude-transform region values
  CCritSec _claude_critsec;
 public:
  // Per-scrape-cycle OCR profiling (reset/read by CLazyScraper::DoScrape):
  // how many AutoOcr regions were freshly recognised this frame vs reused
  // unchanged from the previous frame. The dominant scrape cost is OCR, so
  // reusing unchanged regions is the main speed lever for live phone play.
  long _ocr_recognitions;
  long _ocr_reuses;
  void ResetOcrProfile() { _ocr_recognitions = 0; _ocr_reuses = 0; }
  // Change-detection tolerance for a jittery capture (phone mirror). A region is
  // "changed" only when more than _chg_min_pixels pixels differ by more than
  // _chg_pixel_delta per channel; below that it's treated as unchanged and its
  // previous OCR result is reused (skipping Tesseract). _chg_pixel_delta <= 0
  // means exact match (off). Tuned live via the `scrape_tuning` DB setting.
  int _chg_pixel_delta;
  int _chg_min_pixels;
  void LoadChangeThresholds();
 private:
	HBITMAP			_entire_window_last;
	HBITMAP			_entire_window_cur;
	// Parallel-OCR pre-pass results for this scrape cycle (region name -> text).
	std::map<CString, CString> _ocr_cache;
	// Last OCR result per AutoOcr region, kept ACROSS frames. When a region's
	// pixels are identical to the previous frame (ProcessRegion() == false) we
	// reuse this instead of re-running Tesseract - the big scrape-speed win.
	std::map<CString, CString> _last_ocr_result;
	// Cached per-frame result of the "p3observer" region (see RefreshObserverState).
	bool			_observer_active;
	// Map a p3<x>/u3<x> region name to "p3observer_<x>" when observer mode is active
	// and that observer region exists; otherwise returns the name unchanged.
	CString		RedirectObserverName(const CString &name);
};

extern CScraper *p_scraper;
// One-shot trigger: set true to make the next heartbeat scrape dump all region
// images + results to logs\scrapes\ (for the MCP server / Claude /improve).
extern bool g_dump_scrapes_once;
// MCP/API control, consumed by the heartbeat thread. -1 = nothing pending.
extern int g_mcp_autoplayer_request;   // 0 = off, 1 = on
extern int g_mcp_action_request;       // a k_autoplayer_function_* code (FCKRA)
extern double g_mcp_action_amount;     // bet/raise size in big blinds (<0 = plain click)
extern unsigned long g_mcp_action_set_tick;  // tick when set (wait-for-turn expiry)
extern bool g_mcp_reload_ohf_request;  // set by /api/reload-ohf; heartbeat reloads the strategy folder
extern bool g_frame_history_enabled;   // when true, save a heartbeat frame each scrape (10-min rolling)
// table_game_info: MCP/Claude-parsed game info. Claude reads the table image (heartbeat
// frame) and determines the blinds/ante/level/etc., then POSTs them to /api/table-game-info.
// The blind guesser uses these AUTHORITATIVE values instead of scraping/guessing. For a
// big-blind-denominated display the operating blinds are sb=0.5 bb=1.0, with chips_per_bb
// (e.g. 400) carried as the real level for hand-history/ICM. g_tgi_set gates use.
extern bool   g_tgi_set;             // true once Claude has populated table_game_info
extern double g_tgi_sblind;          // operating small blind (scraped unit)
extern double g_tgi_bblind;          // operating big blind (scraped unit; BB-display -> 1.0)
extern double g_tgi_ante;            // ante in the operating unit
extern double g_tgi_chips_per_bb;    // real chips per big blind (e.g. 400) -- metadata
extern double g_tgi_level;           // tournament level number
extern double g_tgi_players_remaining;
// table_game_info string fields (used by the hand-history writer header, which otherwise
// mis-scrapes them). Empty = not provided.
extern CString g_tgi_tourney_name;   // e.g. "$50 GTD Freeroll"
extern CString g_tgi_tourney_id;     // e.g. "35300198"
extern CString g_tgi_table_number;   // e.g. "1"
extern CString g_tgi_gametype;       // e.g. "No Limit"
// table_game_info_2: current + previous hand numbers (ACR shows "Current: n  Previous: n").
// Claude reads them from the frame and posts via /api/table-game-info-2.
extern double g_tgi2_handnumber;
extern double g_tgi2_prev_handnumber;

#endif // INC_CSCRAPER_H


