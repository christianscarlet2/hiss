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
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg void OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags);
	afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);

	CPen		black_pen, green_pen, red_pen, blue_pen, white_dot_pen, black_dot_pen, null_pen;
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
	CString loaded_group_filename;

	HCURSOR		hCurDrawRect, hCurStandard;
	bool IsRegionSelected(CString name);
	void ToggleRegionSelection(CString name);
	void SelectRegionsInsideRect(RECT rect);
	void ClearRegionSelection();
	void DrawRegionGroups(CDC *pDC);
	void RebuildGroupBounds();
	CString RegionNameAtPoint(CPoint point);
	CString GroupNameAtPoint(CPoint point);
	void MoveGroupBy(CString group_name, int dx, int dy);
	COLORREF SelectedGroupColor();
	CString PromptForGroupName();

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
	void MoveRegionWithGroup(CString name, int dx, int dy);
	void LoadGroupsFromTablemap(bool force = false);
	void SaveGroupsToTablemap();
	void PurgeMissingRegionsFromGroups();
	bool DeleteRegionGroup(CString group_name);
	bool DuplicateRegionGroupToPlayer(CString group_name, CString target_player, CString *error_message);
	bool GetGroupColorForRegion(CString name, COLORREF *color);

	bool		drawing_rect, drawing_started;
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

