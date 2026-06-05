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


// OpenScrapeView.h : interface of the COpenScrapeView class
//
#pragma once
#include <map>
#include <vector>

struct SOpenScrapeRegionGroup {
	CString name;
	COLORREF color;
	std::vector<CString> members;
	RECT bounds;
	bool locked;   // persisted as osgroup<N>locked; a locked group can't be moved
};

struct SOpenScrapeRegionMoveState {
	CString name;
	RECT bounds;
};

struct SOpenScrapeRegionMoveAction {
	std::vector<SOpenScrapeRegionMoveState> before;
	std::vector<SOpenScrapeRegionMoveState> after;
};

//!  Displays Image of csino table, overlayed with regions.
/*!
  also allows movement of regions and calling of file->menu functions.
*/
class COpenScrapeView : public CView
{

protected: // create from serialization only
	COpenScrapeView();
	DECLARE_DYNCREATE(COpenScrapeView)
	DECLARE_MESSAGE_MAP()
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnRButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg void OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags);
	afx_msg void OnTimer(UINT_PTR nIDEvent);
	afx_msg void OnEditUndo();
	afx_msg void OnEditRedo();
	afx_msg void OnUpdateEditUndo(CCmdUI *pCmdUI);
	afx_msg void OnUpdateEditRedo(CCmdUI *pCmdUI);
	afx_msg void OnViewShowPreview();
	afx_msg void OnUpdateViewShowPreview(CCmdUI *pCmdUI);
	afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);

	CPen		black_pen, green_pen, red_pen, blue_pen, white_dot_pen, black_dot_pen, yellow_dot_pen, null_pen;
	CPen		yellow_pen;
	CBrush		white_brush, gray_brush, red_brush, yellow_brush;
	bool		dragging;
	CString		dragged_region;
	CString		dragged_group;
	int			drag_left_offset, drag_top_offset;
	int			group_color_index;
	bool		group_box_mode, group_box_started;
	CPoint		group_box_start, group_box_end;
	std::vector<CString> selected_regions;
	std::map<CString, SOpenScrapeRegionGroup> region_groups;
	std::vector<SOpenScrapeRegionMoveAction> undo_region_moves;
	std::vector<SOpenScrapeRegionMoveAction> redo_region_moves;
	std::vector<SOpenScrapeRegionMoveState> drag_move_before;
	CString loaded_group_filename;
	bool show_preview;
	CPoint preview_point;
	CRect preview_close_rect;

	HCURSOR		hCurDrawRect, hCurStandard;

	// Training-data capture mode (gear toolbar button): the user drags one box
	// over the screenshot, types the value, and a cropped image + .txt are saved.
	bool		_training_mode;
	bool		_training_dragging;
	CPoint		_training_start, _training_cur;
	void HandleTrainingCapture(CRect rect);

	bool IsRegionSelected(CString name);
	void ToggleRegionSelection(CString name);
	void SelectRegionsInsideRect(RECT rect);
	void ClearRegionSelection();
	void DrawRegionGroups(CDC *pDC);
	void DrawMagnifierPreview(CDC *pDC);
	void RebuildGroupBounds();
	CString RegionNameAtPoint(CPoint point);
	CString GroupNameAtPoint(CPoint point);
	void MoveGroupBy(CString group_name, int dx, int dy);
	void MoveGroupBy(CString group_name, int dx, int dy, bool record_undo);
	COLORREF SelectedGroupColor();
	CString PromptForGroupName();
	std::vector<SOpenScrapeRegionMoveState> CaptureRegionMoveState(const std::vector<CString> &region_names);
	std::vector<SOpenScrapeRegionMoveState> CaptureMoveStateForRegion(CString name);
	std::vector<SOpenScrapeRegionMoveState> CaptureMoveStateForGroup(CString group_name);
	void ApplyRegionMoveState(const std::vector<SOpenScrapeRegionMoveState> &states);
	void RecordRegionMove(const std::vector<SOpenScrapeRegionMoveState> &before, const std::vector<SOpenScrapeRegionMoveState> &after);

// Attributes
public:
	static COpenScrapeView * GetView(); 
	COpenScrapeDoc* GetDocument() const;
	virtual void OnDraw(CDC* pDC);  // overridden to draw this view
	virtual BOOL PreCreateWindow(CREATESTRUCT& cs);
	virtual ~COpenScrapeView();
	void blink_rect(void);
	void CreateGroupFromSelection();
	void SetGroupBoxMode(bool enabled);
	bool GroupBoxMode() const { return group_box_mode; }
	void SetSelectedGroupColor(int color_index);
	CString GroupNameForRegion(CString name);
	bool IsGroupLocked(CString group_name);
	// True if the region belongs to a group whose lock flag is set.
	bool IsRegionInLockedGroup(CString name);
	void SetGroupLocked(CString group_name, bool locked);
	void MoveRegionBy(CString name, int dx, int dy);
	void MoveRegionBy(CString name, int dx, int dy, bool record_undo);
	void MoveRegionWithGroup(CString name, int dx, int dy);
	void MoveRegionWithGroup(CString name, int dx, int dy, bool record_undo);
	void LoadGroupsFromTablemap(bool force = false);
	void SaveGroupsToTablemap();
	void PurgeMissingRegionsFromGroups();
	bool DeleteRegionGroup(CString group_name);
	bool DeleteAllRegionsInGroup(CString group_name);
	bool AddRegionToGroup(CString group_name, CString region_name);
	bool RemoveRegionFromGroup(CString name);
	bool DuplicateRegionGroupToPlayer(CString group_name, CString target_player, CString *error_message);
	bool GetGroupColorForRegion(CString name, COLORREF *color);
	void SetShowPreview(bool show);
	bool ShowPreview() const { return show_preview; }
	void SetTrainingMode(bool on);
	bool training_mode() const { return _training_mode; }
	// Most-recently selected region name (for the settings live preview), "" if none.
	CString PreviewRegionName() const {
		return selected_regions.empty() ? CString("") : selected_regions.back();
	}
	// Decimal-split overlay: a red line where the decimal separator sits, for
	// regions whose field type is configured for decimal splitting (settings).
	void ReloadDecimalFields() { _decimal_fields_loaded = false; }

	// --- Auto card capture -----------------------------------------------------
	// AUTO toolbar toggle: on a timer, grab the connected window, run card detection,
	// and buffer the frame whenever opponent ranks/suits (excluding hero p3) are seen
	// or all 5 community cards are present. The back/next arrows scrub the buffer.
	void ToggleAutoCapture();
	bool IsAutoCapture() const { return _auto_capture; }
	bool HasCaptures() const { return !_capture_buffer.empty(); }
	void NavigateCapture(int delta);
	void ClearCaptures();
	void OnAutoCaptureIntervalChanged(int interval_ms);
	// Force the card-result overlay cache to recompute on the next paint (call after
	// the tablemap's images/hashes/regions change, e.g. Create Image / Create Hash).
	void InvalidateCardResults() { _card_results_bmp = NULL; }
	// Re-run card detection on the currently displayed frame and repaint.
	void RefreshCurrentScreenshot();
	// "Debug this field" (region right-click): dump the scraped region image, the current
	// table screenshot, the matched/candidate transform images, the computed hash, and all
	// transform/OCR/colour settings + result to <appdir>\debug\<region>_<timestamp>\.
	void DebugRegionToFolder(const CString &region_name);
	// 0-based index of the currently-displayed buffered capture (-1 if none).
	int CaptureIndex() const { return _capture_index; }
	// Persist / restore the capture buffer across runs (PNGs under vision_captures\).
	void SaveCapturesToDisk();
	void LoadCapturesFromDisk();
private:
	bool                 _auto_capture;
	int                  _capture_index;
	std::vector<HBITMAP> _capture_buffer;     // copies of frames that matched
	std::vector<CSize>   _capture_sizes;      // pixel size of each buffered frame
	CString              _last_capture_sig;   // detected-values signature of the last STORED frame
	void AutoCaptureTick();
	bool CardCaptureConditionMet();
	CString CaptureSignature();               // signature of the current card results
	void RefreshWholeApp();
	void ClearCaptureBuffer();
private:
	std::vector<CString> _decimal_fields;          // field-type substrings from settings
	bool                 _decimal_fields_loaded;
	HBITMAP              _decimal_cached_bmp;       // frame the cache was computed for
	std::vector<std::pair<CString, int> > _decimal_lines;  // region name -> x offset
	bool RegionUsesDecimalSplit(const CString &name);
	void DrawDecimalSplitLines(CDC *pDC);

	// Live result overlays for card rank/suit/community regions: shows the currently
	// detected value inside each region box (semi-transparent, suit letters drawn as
	// pip symbols). Cached per captured frame so the image-compares only re-run when
	// the table bitmap changes.
	std::map<CString, CString> _card_results;          // region name -> detected text
	HBITMAP                    _card_results_bmp;       // frame the cache was computed for
	static bool IsCardResultRegion(const CString &name);
	CString ScrapeRegionResult(RMapCI r_iter);
	void RefreshCardResultsIfNeeded(COpenScrapeDoc *pDoc);
	void EnsureWorkerPool();      // (re)size the detection pool from settings
	void DrawCardResultOverlay(CDC *pDC, RMapCI r_iter);
public:
	int CaptureCount() const { return (int)_capture_buffer.size(); }
private:

public:
	bool		drawing_rect, drawing_started;
	bool		drawing_rect2;   // drawing the optional second rectangle
	bool		color_point_mode;
	bool		color_dropper_mode;
	CPoint		drawrect_start;
	CString		drawrect_region;

#ifdef _DEBUG
	virtual void AssertValid() const;
	virtual void Dump(CDumpContext& dc) const;
#endif
};

#ifndef _DEBUG  // debug version in OpenScrapeView.cpp
inline COpenScrapeDoc* COpenScrapeView::GetDocument() const
   { return reinterpret_cast<COpenScrapeDoc*>(m_pDocument); }
#endif

