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
	void ApplyBetMemory(int chair);   // bet memory: restore last-good if bet > stack
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
	// On a validator error (g_capture_suspect_request), save the money region images
	// (balance/bet/pot) + a .gt.txt placeholder to logs\capture\<ms>\ so Claude can
	// clarify them (post via /api/set-region-value -> memory) and they become labelled
	// real training samples (exactly the hard cases that fail validation).
	void CaptureSuspectScrapeIfRequested();
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
  CString _last_capture_sig;  // Advanced Replay: hand|street|active-mask|ismyturn|gametype of the last CAPTURED
                              // frame -> only ship a new replay frame when this changes (event-driven). [Emrald]
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
	// OCR memory cache (g_ocr_memory). Survives CPlayer::Reset(), which set_seated(false)
	// triggers on a flickering pXseated read: an OUT player (sitting out, still in the
	// chair) keeps showing their last-good stack/name instead of flashing 0/empty. Cleared
	// once the seat has been unseated continuously past kMaxOutMemoryFrames (truly gone).
	double  _mem_balance[kMaxNumberOfPlayers];
	double  _mem_bet[kMaxNumberOfPlayers];   // last-good bet per chair (bet memory)
	CString _mem_name[kMaxNumberOfPlayers];
	int     _mem_out_frames[kMaxNumberOfPlayers];
	// p3 observer-state memory: keep observing while the seat-3 name is unchanged even if
	// the p3observer scrape flickers to false (unless that name is one of the user's).
	bool    _mem_p3observer;
	CString _mem_p3observer_name;
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
// Set by the validator when it detects a bad money value; the next heartbeat saves the
// suspect money-region images for Claude clarification + tesseract training capture.
extern bool g_capture_suspect_request;
extern CString g_capture_suspect_reason;
// MCP/API control, consumed by the heartbeat thread. -1 = nothing pending.
extern int g_mcp_autoplayer_request;   // 0 = off, 1 = on
extern int g_mcp_nn_driver_request;    // -1 idle, 0 = disengage, 1 = engage (applied by heartbeat)
extern bool g_nn_driver_engaged;       // current NN-driver state (read by /api/nn-driver + the UIs)
extern int g_mcp_ultra_request;        // -1 idle, 0 = disengage, 1 = engage ULTRA (applied by heartbeat)
extern bool g_ultra_engaged;           // current ULTRA-mode state (read by /api/ultra + the UIs)
extern int g_mcp_superstition_request; // -1 idle, 0 = disengage, 1 = engage superstition (heartbeat)
extern bool g_superstition_engaged;    // current superstition/omen state (read by /api/superstition + UIs)
extern double g_beast_favor;           // 666 Card Oracle resonance 0..1 (pushed via /api/beast)
extern unsigned long g_beast_favor_tick;  // GetTickCount when last set; goes stale (->0) after ~15s
extern int g_terminal_port;
void TerminateInstanceHelpers();   // kill this instance's helper daemons (port-keyed)            // the bound ChatTerminalServer port (for the NN driver's URL)
extern int g_mcp_action_request;       // a k_autoplayer_function_* code (FCKRA)
extern double g_mcp_action_amount;     // bet/raise size in big blinds (<0 = plain click)
extern unsigned long g_mcp_action_set_tick;  // tick when set (wait-for-turn expiry)
extern bool g_mcp_action_force;        // true: manual learner click -> bypass the ismyturn gate
extern bool g_mcp_reload_ohf_request;  // set by /api/reload-ohf; heartbeat reloads the strategy folder
extern CString g_mcp_click_region;     // /api/click-region: heartbeat clicks this tablemap region's rect (lobby nav)
// HUD overlay recalibration: user right-clicks "Recalibrate all HUDs (Claude)" -> request flag.
// Claude/MCP reads the table screenshot and posts per-seat anchors as JSON (fractions of the
// client area); the heartbeat hands g_hud_positions_json to the overlay to apply + persist.
extern bool    g_hud_calibrate_request;  // set by /api/hud-calibrate; MCP polls /api/hud-calibrate-status
extern bool    g_hud_positions_apply;    // set by /api/hud-positions; heartbeat applies g_hud_positions_json
extern CString g_hud_positions_json;     // per-seat anchor fractions: {"c0":{"x":fx,"y":fy},...}
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
extern CString g_table_identity;     // tourney_id|table_name; changes on a table switch (phantom guard)
extern bool    g_table_is_omaha;     // scraped text says Omaha/PLO/Hi-Lo -> drives the tablemap auto-switch
// On-table RED decision overlay (HudOverlayWindow): the autoplayer publishes its locked action here on
// the heartbeat; the overlay paints it big/bold/red above the hero's cards. Plain char buffer (no CString
// ref-counting) since it crosses the heartbeat->UI thread boundary. [Emrald]
extern char    g_hero_decision_text[48];   // e.g. "RAISE 2.50" / "CALL" / "FOLD" (empty = no decision)
extern volatile bool g_hero_decision_active; // true only while ismyturn && isfinalanswer
extern DWORD   g_hero_decision_tick;        // GetTickCount() at decision lock; RED overlay trails ~10s then fades
// Brain detail for the RED overlay: extra context lines (exploit / branch / vs-villain / confidence / mischief),
// '\n'-separated, pushed by the Python brain via /api/decision-detail. Drawn small+red under the action on scrcpy.
extern char    g_hero_decision_detail[256]; // [Emrald: more lines on the RED decision in scrcpy]
extern DWORD   g_hero_decision_detail_tick; // freshness of the detail (independent of the action lock)
extern volatile bool g_reset_detection_request; // React badge backup: clear per-table game-type cache + identity -> re-detect
extern char g_fckra_indicator[8];   // lit primary buttons F/C/K/R/A (mirrors the main view's bottom-corner indicators)
extern char g_tiolp_indicator[8];   // lit secondary buttons T/I/O/L/P
extern CString g_tgi_gametype;       // e.g. "No Limit"
// table_game_info_2: current + previous hand numbers (ACR shows "Current: n  Previous: n").
// Claude reads them from the frame and posts via /api/table-game-info-2.
extern double g_tgi2_handnumber;
extern double g_tgi2_prev_handnumber;

// OCR memory: when true, a seated player's name/balance is NOT overwritten by a
// status-indicator mis-scrape (SITTING OUT, FOLD, ...) or a bogus 0 balance; the
// last-good table-state value is kept (and so shown by the React table view).
// Loaded from the shared settings table ("ocr_memory"/"enabled") in
// CAutoOcr::LoadModelSettings(), edited on the Hiss OCR preferences page.
extern bool g_ocr_memory;

// The user's own ACR usernames (comma/;/| separated). Loaded from the settings table
// ("my_usernames"/"list") in CAutoOcr::LoadModelSettings(); edited on the OCR prefs page.
// Used to tell "I am playing this seat" from "I am observing it" -- defaults always
// include scarletchrist + christianbeast even if the setting is empty.
extern CString g_my_usernames;

#endif // INC_CSCRAPER_H


