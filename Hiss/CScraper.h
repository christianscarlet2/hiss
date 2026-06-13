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
 private:
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

#endif // INC_CSCRAPER_H


