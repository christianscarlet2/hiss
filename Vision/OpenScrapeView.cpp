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

// OpenScrapeView.cpp : implementation of the COpenScrapeView class
//
#include "stdafx.h"
#include "OpenScrape.h"
#include "OpenScrapeDoc.h"
#include "OpenScrapeView.h"
#include "MainFrm.h"
#include "DialogEdit.h"
#include "DecimalSplit.h"
#include "../CTablemap/CTablemapDB.h"
#include "../CTransform/CTransform.h"
#include "../Shared/ParallelWorkerPool.h"
#include <gdiplus.h>
#include <shlobj.h>
#include <algorithm>
#include <ctype.h>

// Shared in-process worker pool for parallel card detection (sized from the
// "Parallel Workers" settings, split across running Vision instances).
static ParallelWorkerPool g_card_pool;
static int g_card_pool_size = 0;

// Copy one region's pixels out of the source frame into its own small bitmap.
// MUST run on the UI thread (a source HBITMAP can be selected into one DC only).
static HBITMAP ExtractRegionBitmap(HBITMAP src, int left, int top, int w, int h) {
	if (src == NULL || w <= 0 || h <= 0) return NULL;
	HDC screen = CreateDC("DISPLAY", NULL, NULL, NULL);
	HDC sdc = CreateCompatibleDC(screen);
	HDC ddc = CreateCompatibleDC(screen);
	HBITMAP dst = CreateCompatibleBitmap(screen, w, h);
	HBITMAP os = (HBITMAP)SelectObject(sdc, src);
	HBITMAP od = (HBITMAP)SelectObject(ddc, dst);
	BitBlt(ddc, 0, 0, w, h, sdc, left, top, SRCCOPY);
	SelectObject(sdc, os);
	SelectObject(ddc, od);
	DeleteDC(sdc);
	DeleteDC(ddc);
	DeleteDC(screen);
	return dst;
}

// Worker-thread side: run the region's transform on its private bitmap. Each job
// owns its DC + bitmap (no shared GDI); only read-only p_tablemap data is shared.
static CString TransformRegionBitmap(RMapCI region, HBITMAP region_bmp) {
	CString result;
	if (region_bmp == NULL) return result;
	HDC screen = CreateDC("DISPLAY", NULL, NULL, NULL);
	HDC dc = CreateCompatibleDC(screen);
	HBITMAP old = (HBITMAP)SelectObject(dc, region_bmp);
	CTransform trans;
	trans.DoTransform(region, dc, &result);
	SelectObject(dc, old);
	DeleteDC(dc);
	DeleteDC(screen);
	return result;
}

#define AUTO_CAPTURE_TIMER 0xA100

// Reads the auto-card-capture poll interval (ms) from the shared settings, default 2000.
static int AutoCaptureIntervalMs() {
	int ms = 2000;
	if (p_tablemap_db != NULL) {
		CString v = p_tablemap_db->GetSettingString("auto_card_capture", "interval_ms");
		if (!v.IsEmpty()) ms = atoi(v);
	}
	if (ms < 250) ms = 250;
	return ms;
}

// Duplicates an HBITMAP into a fresh independent bitmap (for the capture buffer).
static HBITMAP DuplicateHBitmap(HBITMAP src, int w, int h) {
	if (src == NULL || w <= 0 || h <= 0) return NULL;
	HDC screen = CreateDC("DISPLAY", NULL, NULL, NULL);
	HDC sdc = CreateCompatibleDC(screen);
	HDC ddc = CreateCompatibleDC(screen);
	HBITMAP dst = CreateCompatibleBitmap(screen, w, h);
	HBITMAP os = (HBITMAP)SelectObject(sdc, src);
	HBITMAP od = (HBITMAP)SelectObject(ddc, dst);
	BitBlt(ddc, 0, 0, w, h, sdc, 0, 0, SRCCOPY);
	SelectObject(sdc, os);
	SelectObject(ddc, od);
	DeleteDC(sdc);
	DeleteDC(ddc);
	DeleteDC(screen);
	return dst;
}

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

static bool RegionNameContainsPlayer(CString region_name, CString player)
{
	int start = 0;
	while ((start = region_name.Find(player, start)) != -1) {
		int end = start + player.GetLength();
		bool previous_ok = start == 0 || !isalnum((unsigned char)region_name[start - 1]);
		bool next_ok = end >= region_name.GetLength() || !isdigit((unsigned char)region_name[end]);
		if (previous_ok && next_ok) {
			return true;
		}
		start = end;
	}
	return false;
}

static CString FirstPlayerInRegionName(CString region_name)
{
	for (int player = 0; player <= 8; ++player) {
		CString player_text;
		player_text.Format("p%d", player);
		if (RegionNameContainsPlayer(region_name, player_text)) {
			return player_text;
		}
	}
	return "";
}

static COLORREF NextGroupColor(COLORREF current_color)
{
	COLORREF colors[] = {
		RGB(255, 0, 0),
		RGB(0, 80, 255),
		RGB(0, 180, 0),
		RGB(255, 220, 0),
		RGB(0, 200, 200),
		RGB(220, 0, 220)
	};
	const int color_count = sizeof(colors) / sizeof(colors[0]);
	for (int i = 0; i < color_count; ++i) {
		if (colors[i] == current_color) {
			return colors[(i + 1) % color_count];
		}
	}
	return colors[0];
}

static CString ColorPresetKeyForIndex(int index, const char *field)
{
	CString key;
	if (index < 0) {
		key.Format("oscoloripdefault%s", field);
	} else {
		key.Format("oscolorip%d%s", index, field);
	}
	return key;
}

static void UpsertSymbol(CString key, CString text)
{
	STablemapSymbol symbol;
	p_tablemap->s$_erase(key);
	symbol.name = key;
	symbol.text = text;
	p_tablemap->s$_insert(symbol);
}

// COpenScrapeView

IMPLEMENT_DYNCREATE(COpenScrapeView, CView)

BEGIN_MESSAGE_MAP(COpenScrapeView, CView)
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONUP()
	ON_WM_RBUTTONDOWN()
	ON_WM_MOUSEMOVE()
	ON_WM_SETCURSOR()
	ON_WM_KEYDOWN()
	ON_WM_TIMER()
	ON_COMMAND(ID_EDIT_UNDO, &COpenScrapeView::OnEditUndo)
	ON_COMMAND(ID_EDIT_REDO, &COpenScrapeView::OnEditRedo)
	ON_UPDATE_COMMAND_UI(ID_EDIT_UNDO, &COpenScrapeView::OnUpdateEditUndo)
	ON_UPDATE_COMMAND_UI(ID_EDIT_REDO, &COpenScrapeView::OnUpdateEditRedo)
	ON_COMMAND(ID_VIEW_SHOW_PREVIEW, &COpenScrapeView::OnViewShowPreview)
	ON_UPDATE_COMMAND_UI(ID_VIEW_SHOW_PREVIEW, &COpenScrapeView::OnUpdateViewShowPreview)
END_MESSAGE_MAP()

// COpenScrapeView construction/destruction

COpenScrapeView::COpenScrapeView()
{
	black_pen.CreatePen(PS_SOLID, 1, COLOR_BLACK);
	green_pen.CreatePen(PS_SOLID, 1, COLOR_GREEN);
	red_pen.CreatePen(PS_SOLID, 1, COLOR_RED);
	blue_pen.CreatePen(PS_SOLID, 1, COLOR_BLUE);
	yellow_pen.CreatePen(PS_SOLID, 2, COLOR_YELLOW);
	white_dot_pen.CreatePen(PS_DOT, 1, COLOR_WHITE);
	black_dot_pen.CreatePen(PS_DOT, 1, COLOR_BLACK);
	yellow_dot_pen.CreatePen(PS_DOT, 1, COLOR_YELLOW);
	null_pen.CreatePen(PS_NULL, 0, COLOR_BLACK);

	white_brush.CreateSolidBrush(COLOR_WHITE);
	gray_brush.CreateSolidBrush(COLOR_GRAY);
	red_brush.CreateSolidBrush(COLOR_RED);
	yellow_brush.CreateSolidBrush(COLOR_YELLOW);

	_decimal_fields_loaded = false;
	_decimal_cached_bmp = NULL;
	_card_results_bmp = NULL;
	_auto_capture = false;
	_capture_index = -1;

	dragging = false;
	dragged_region = "";
	dragged_group = "";
	group_color_index = 3;

	drawing_rect = drawing_started = false;
	drawing_rect2 = false;
	color_point_mode = false;
	color_dropper_mode = false;
	group_box_mode = group_box_started = false;
	_training_mode = false;
	_training_dragging = false;
	show_preview = true;
	preview_point = CPoint(0, 0);
	preview_close_rect.SetRectEmpty();
	hCurDrawRect = ::LoadCursor(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDC_DRAWRECTCURSOR));
	hCurStandard = ::LoadCursor(NULL, IDC_ARROW);
}

COpenScrapeView::~COpenScrapeView()
{
	ClearCaptureBuffer();
}

BOOL COpenScrapeView::PreCreateWindow(CREATESTRUCT& cs)
{
	return CView::PreCreateWindow(cs);
}

bool COpenScrapeView::IsRegionSelected(CString name)
{
	for (size_t i = 0; i < selected_regions.size(); ++i) {
		if (selected_regions[i] == name) {
			return true;
		}
	}
	return false;
}

void COpenScrapeView::ToggleRegionSelection(CString name)
{
	for (std::vector<CString>::iterator it = selected_regions.begin(); it != selected_regions.end(); ++it) {
		if (*it == name) {
			selected_regions.erase(it);
			Invalidate(false);
			return;
		}
	}
	selected_regions.push_back(name);
	Invalidate(false);
}

void COpenScrapeView::ClearRegionSelection()
{
	selected_regions.clear();
	Invalidate(false);
}

COLORREF COpenScrapeView::SelectedGroupColor()
{
	int color_index = group_color_index;
	switch (color_index) {
	case 0: return RGB(255, 0, 0);
	case 1: return RGB(0, 80, 255);
	case 2: return RGB(0, 180, 0);
	case 4: return RGB(0, 200, 200);
	case 5: return RGB(220, 0, 220);
	case 3:
	default: return RGB(255, 220, 0);
	}
}

CString COpenScrapeView::PromptForGroupName()
{
	CDlgEdit dlg;
	CString default_name;
	default_name.Format("Group%d", (int)region_groups.size() + 1);
	dlg.m_titletext = "Group name";
	dlg.m_result = default_name;
	if (dlg.DoModal() != IDOK) {
		return "";
	}
	dlg.m_result.Trim();
	return dlg.m_result;
}

void COpenScrapeView::RebuildGroupBounds()
{
	for (std::map<CString, SOpenScrapeRegionGroup>::iterator g = region_groups.begin(); g != region_groups.end(); ++g) {
		bool initialized = false;
		RECT bounds = { 0, 0, 0, 0 };
		for (size_t i = 0; i < g->second.members.size(); ++i) {
			RMapCI r_iter = p_tablemap->r$()->find(g->second.members[i].GetString());
			if (r_iter == p_tablemap->r$()->end()) {
				continue;
			}
			if (!initialized) {
				bounds.left = r_iter->second.left;
				bounds.top = r_iter->second.top;
				bounds.right = r_iter->second.right;
				bounds.bottom = r_iter->second.bottom;
				initialized = true;
			} else {
				bounds.left = min(bounds.left, (LONG)r_iter->second.left);
				bounds.top = min(bounds.top, (LONG)r_iter->second.top);
				bounds.right = max(bounds.right, (LONG)r_iter->second.right);
				bounds.bottom = max(bounds.bottom, (LONG)r_iter->second.bottom);
			}
		}
		g->second.bounds = bounds;
	}
}

CString COpenScrapeView::RegionNameAtPoint(CPoint point)
{
	for (RMapCI r_iter = p_tablemap->r$()->begin(); r_iter != p_tablemap->r$()->end(); r_iter++) {
		if (point.x >= (LONG)r_iter->second.left - 1
				&& point.x <= (LONG)r_iter->second.right + 1
				&& point.y >= (LONG)r_iter->second.top - 1
				&& point.y <= (LONG)r_iter->second.bottom + 1) {
			return r_iter->second.name;
		}
	}
	return "";
}

CString COpenScrapeView::GroupNameAtPoint(CPoint point)
{
	LoadGroupsFromTablemap();
	RebuildGroupBounds();

	CString matched_group = "";
	LONG matched_area = 0x7fffffff;
	for (std::map<CString, SOpenScrapeRegionGroup>::iterator g = region_groups.begin(); g != region_groups.end(); ++g) {
		RECT bounds = g->second.bounds;
		bounds.left -= 4;
		bounds.top -= 18;
		bounds.right += 5;
		bounds.bottom += 5;
		if (point.x >= bounds.left
				&& point.x <= bounds.right
				&& point.y >= bounds.top
				&& point.y <= bounds.bottom) {
			LONG area = (bounds.right - bounds.left) * (bounds.bottom - bounds.top);
			if (matched_group.IsEmpty() || area < matched_area) {
				matched_group = g->first;
				matched_area = area;
			}
		}
	}
	return matched_group;
}

std::vector<SOpenScrapeRegionMoveState> COpenScrapeView::CaptureRegionMoveState(const std::vector<CString> &region_names)
{
	std::vector<SOpenScrapeRegionMoveState> states;
	for (size_t i = 0; i < region_names.size(); ++i) {
		RMapCI r_iter = p_tablemap->r$()->find(region_names[i].GetString());
		if (r_iter == p_tablemap->r$()->end()) {
			continue;
		}

		SOpenScrapeRegionMoveState state;
		state.name = r_iter->second.name;
		state.bounds.left = r_iter->second.left;
		state.bounds.top = r_iter->second.top;
		state.bounds.right = r_iter->second.right;
		state.bounds.bottom = r_iter->second.bottom;
		states.push_back(state);
	}
	return states;
}

std::vector<SOpenScrapeRegionMoveState> COpenScrapeView::CaptureMoveStateForRegion(CString name)
{
	std::vector<CString> region_names;
	region_names.push_back(name);
	return CaptureRegionMoveState(region_names);
}

std::vector<SOpenScrapeRegionMoveState> COpenScrapeView::CaptureMoveStateForGroup(CString group_name)
{
	LoadGroupsFromTablemap();
	std::map<CString, SOpenScrapeRegionGroup>::iterator group = region_groups.find(group_name);
	if (group == region_groups.end()) {
		return std::vector<SOpenScrapeRegionMoveState>();
	}
	return CaptureRegionMoveState(group->second.members);
}

void COpenScrapeView::ApplyRegionMoveState(const std::vector<SOpenScrapeRegionMoveState> &states)
{
	for (size_t i = 0; i < states.size(); ++i) {
		RMapI r_iter = p_tablemap->set_r$()->find(states[i].name.GetString());
		if (r_iter == p_tablemap->r$()->end()) {
			continue;
		}
		r_iter->second.left = states[i].bounds.left;
		r_iter->second.top = states[i].bounds.top;
		r_iter->second.right = states[i].bounds.right;
		r_iter->second.bottom = states[i].bounds.bottom;
	}

	RebuildGroupBounds();
	if (theApp.m_TableMapDlg != NULL) {
		theApp.m_TableMapDlg->update_display();
		theApp.m_TableMapDlg->Invalidate(false);
	}
	Invalidate(false);
	GetDocument()->SetModifiedFlag(true);
}

void COpenScrapeView::RecordRegionMove(const std::vector<SOpenScrapeRegionMoveState> &before, const std::vector<SOpenScrapeRegionMoveState> &after)
{
	if (before.empty() || before.size() != after.size()) {
		return;
	}

	bool changed = false;
	for (size_t i = 0; i < before.size(); ++i) {
		if (before[i].name != after[i].name
				|| before[i].bounds.left != after[i].bounds.left
				|| before[i].bounds.top != after[i].bounds.top
				|| before[i].bounds.right != after[i].bounds.right
				|| before[i].bounds.bottom != after[i].bounds.bottom) {
			changed = true;
			break;
		}
	}
	if (!changed) {
		return;
	}

	SOpenScrapeRegionMoveAction action;
	action.before = before;
	action.after = after;
	undo_region_moves.push_back(action);
	redo_region_moves.clear();
}

void COpenScrapeView::MoveGroupBy(CString group_name, int dx, int dy)
{
	MoveGroupBy(group_name, dx, dy, true);
}

void COpenScrapeView::MoveGroupBy(CString group_name, int dx, int dy, bool record_undo)
{
	LoadGroupsFromTablemap();
	std::map<CString, SOpenScrapeRegionGroup>::iterator group = region_groups.find(group_name);
	if (group == region_groups.end()) {
		return;
	}
	// Locked groups can't be moved interactively (record_undo==true). Undo/redo
	// replay (record_undo==false) is still allowed.
	if (group->second.locked && record_undo) {
		return;
	}

	std::vector<SOpenScrapeRegionMoveState> before;
	if (record_undo) {
		before = CaptureMoveStateForGroup(group_name);
	}

	for (size_t i = 0; i < group->second.members.size(); ++i) {
		RMapI r_iter = p_tablemap->set_r$()->find(group->second.members[i].GetString());
		if (r_iter != p_tablemap->r$()->end()) {
			r_iter->second.left += dx;
			r_iter->second.right += dx;
			r_iter->second.top += dy;
			r_iter->second.bottom += dy;
		}
	}
	RebuildGroupBounds();

	if (record_undo) {
		RecordRegionMove(before, CaptureMoveStateForGroup(group_name));
	}
}

CString COpenScrapeView::GroupNameForRegion(CString name)
{
	LoadGroupsFromTablemap();
	for (std::map<CString, SOpenScrapeRegionGroup>::iterator g = region_groups.begin(); g != region_groups.end(); ++g) {
		for (size_t i = 0; i < g->second.members.size(); ++i) {
			if (g->second.members[i] == name) {
				return g->first;
			}
		}
	}
	return "";
}

bool COpenScrapeView::IsGroupLocked(CString group_name)
{
	LoadGroupsFromTablemap();
	std::map<CString, SOpenScrapeRegionGroup>::iterator g = region_groups.find(group_name);
	return (g != region_groups.end()) && g->second.locked;
}

bool COpenScrapeView::IsRegionInLockedGroup(CString name)
{
	CString group = GroupNameForRegion(name);
	return !group.IsEmpty() && IsGroupLocked(group);
}

// Set/clear a group's locked flag and persist it (osgroup<N>locked symbol -> DB
// on the next tablemap save).
void COpenScrapeView::SetGroupLocked(CString group_name, bool locked)
{
	LoadGroupsFromTablemap();
	std::map<CString, SOpenScrapeRegionGroup>::iterator g = region_groups.find(group_name);
	if (g == region_groups.end()) {
		return;
	}
	g->second.locked = locked;
	SaveGroupsToTablemap();
	GetDocument()->SetModifiedFlag(true);
	Invalidate(false);
}

bool COpenScrapeView::GetGroupColorForRegion(CString name, COLORREF *color)
{
	LoadGroupsFromTablemap();
	for (std::map<CString, SOpenScrapeRegionGroup>::iterator g = region_groups.begin(); g != region_groups.end(); ++g) {
		for (size_t i = 0; i < g->second.members.size(); ++i) {
			if (g->second.members[i] == name) {
				*color = g->second.color;
				return true;
			}
		}
	}
	return false;
}

void COpenScrapeView::MoveRegionBy(CString name, int dx, int dy)
{
	MoveRegionBy(name, dx, dy, true);
}

void COpenScrapeView::MoveRegionBy(CString name, int dx, int dy, bool record_undo)
{
	std::vector<SOpenScrapeRegionMoveState> before;
	if (record_undo) {
		before = CaptureMoveStateForRegion(name);
	}

	RMapI r_iter = p_tablemap->set_r$()->find(name.GetString());
	if (r_iter != p_tablemap->r$()->end()) {
		r_iter->second.left += dx;
		r_iter->second.right += dx;
		r_iter->second.top += dy;
		r_iter->second.bottom += dy;
	}
	RebuildGroupBounds();

	if (record_undo) {
		RecordRegionMove(before, CaptureMoveStateForRegion(name));
	}
}

void COpenScrapeView::MoveRegionWithGroup(CString name, int dx, int dy)
{
	MoveRegionWithGroup(name, dx, dy, true);
}

void COpenScrapeView::MoveRegionWithGroup(CString name, int dx, int dy, bool record_undo)
{
	LoadGroupsFromTablemap();
	CString group_name = GroupNameForRegion(name);
	if (group_name == "") {
		MoveRegionBy(name, dx, dy, record_undo);
		return;
	}

	std::vector<SOpenScrapeRegionMoveState> before;
	if (record_undo) {
		before = CaptureMoveStateForGroup(group_name);
	}

	SOpenScrapeRegionGroup &group = region_groups[group_name];
	for (size_t i = 0; i < group.members.size(); ++i) {
		RMapI r_iter = p_tablemap->set_r$()->find(group.members[i].GetString());
		if (r_iter != p_tablemap->r$()->end()) {
			r_iter->second.left += dx;
			r_iter->second.right += dx;
			r_iter->second.top += dy;
			r_iter->second.bottom += dy;
		}
	}
	RebuildGroupBounds();

	if (record_undo) {
		RecordRegionMove(before, CaptureMoveStateForGroup(group_name));
	}
}

void COpenScrapeView::PurgeMissingRegionsFromGroups()
{
	LoadGroupsFromTablemap(true);
	bool changed = false;
	for (std::map<CString, SOpenScrapeRegionGroup>::iterator g = region_groups.begin(); g != region_groups.end();) {
		std::vector<CString> remaining_members;
		for (size_t i = 0; i < g->second.members.size(); ++i) {
			if (p_tablemap->r$()->find(g->second.members[i].GetString()) != p_tablemap->r$()->end()) {
				remaining_members.push_back(g->second.members[i]);
			}
		}
		if (remaining_members.size() != g->second.members.size()) {
			changed = true;
		}
		g->second.members = remaining_members;
		if (g->second.members.size() < 2) {
			std::map<CString, SOpenScrapeRegionGroup>::iterator erase_it = g++;
			region_groups.erase(erase_it);
			changed = true;
		} else {
			++g;
		}
	}
	if (changed) {
		RebuildGroupBounds();
		SaveGroupsToTablemap();
	}
}

bool COpenScrapeView::DeleteRegionGroup(CString group_name)
{
	LoadGroupsFromTablemap(true);
	std::map<CString, SOpenScrapeRegionGroup>::iterator group = region_groups.find(group_name);
	if (group == region_groups.end()) {
		return false;
	}

	region_groups.erase(group);
	SaveGroupsToTablemap();
	ClearRegionSelection();
	GetDocument()->SetModifiedFlag(true);
	Invalidate(false);
	return true;
}

bool COpenScrapeView::DeleteAllRegionsInGroup(CString group_name)
{
	LoadGroupsFromTablemap(true);
	std::map<CString, SOpenScrapeRegionGroup>::iterator group = region_groups.find(group_name);
	if (group == region_groups.end()) {
		return false;
	}

	std::vector<CString> members = group->second.members;
	for (size_t i = 0; i < members.size(); ++i) {
		p_tablemap->r$_erase(members[i]);
	}

	region_groups.erase(group);
	SaveGroupsToTablemap();
	ClearRegionSelection();
	GetDocument()->SetModifiedFlag(true);
	Invalidate(false);
	return true;
}

bool COpenScrapeView::AddRegionToGroup(CString group_name, CString region_name)
{
	LoadGroupsFromTablemap(true);
	std::map<CString, SOpenScrapeRegionGroup>::iterator group = region_groups.find(group_name);
	if (group == region_groups.end()) {
		return false;
	}
	for (size_t i = 0; i < group->second.members.size(); ++i) {
		if (group->second.members[i] == region_name) {
			return true;
		}
	}
	group->second.members.push_back(region_name);
	RebuildGroupBounds();
	SaveGroupsToTablemap();
	GetDocument()->SetModifiedFlag(true);
	Invalidate(false);
	return true;
}

bool COpenScrapeView::RemoveRegionFromGroup(CString name)
{
	LoadGroupsFromTablemap(true);
	for (std::map<CString, SOpenScrapeRegionGroup>::iterator g = region_groups.begin(); g != region_groups.end(); ++g) {
		for (std::vector<CString>::iterator member = g->second.members.begin(); member != g->second.members.end(); ++member) {
			if (*member != name) {
				continue;
			}

			g->second.members.erase(member);
			if (g->second.members.size() < 2) {
				region_groups.erase(g);
			}
			RebuildGroupBounds();
			SaveGroupsToTablemap();
			ClearRegionSelection();
			GetDocument()->SetModifiedFlag(true);
			Invalidate(false);
			return true;
		}
	}
	return false;
}

bool COpenScrapeView::DuplicateRegionGroupToPlayer(CString group_name, CString target_player, CString *error_message)
{
	LoadGroupsFromTablemap(true);
	std::map<CString, SOpenScrapeRegionGroup>::iterator source_group = region_groups.find(group_name);
	if (source_group == region_groups.end()) {
		if (error_message != NULL) {
			error_message->Format("Group '%s' was not found.", group_name);
		}
		return false;
	}

	CString source_player = "";
	for (size_t i = 0; i < source_group->second.members.size(); ++i) {
		source_player = FirstPlayerInRegionName(source_group->second.members[i]);
		if (!source_player.IsEmpty()) {
			break;
		}
	}

	if (source_player.IsEmpty()) {
		if (error_message != NULL) {
			error_message->Format("Could not find a player token like p0 in group '%s'.", group_name);
		}
		return false;
	}

	if (source_player == target_player) {
		if (error_message != NULL) {
			error_message->Format("Choose a different player than %s.", source_player);
		}
		return false;
	}

	CString new_group_name = PromptForGroupName();
	if (new_group_name.IsEmpty()) {
		return false;
	}
	if (region_groups.find(new_group_name) != region_groups.end()) {
		if (error_message != NULL) {
			error_message->Format("Group '%s' already exists.", new_group_name);
		}
		return false;
	}

	std::vector<CString> new_members;
	for (size_t i = 0; i < source_group->second.members.size(); ++i) {
		if (p_tablemap->r$()->find(source_group->second.members[i].GetString()) == p_tablemap->r$()->end()) {
			if (error_message != NULL) {
				error_message->Format("Source region '%s' no longer exists.", source_group->second.members[i]);
			}
			return false;
		}
		CString new_name = source_group->second.members[i];
		new_name.Replace(source_player, target_player);
		if (p_tablemap->r$()->find(new_name.GetString()) != p_tablemap->r$()->end()) {
			if (error_message != NULL) {
				error_message->Format("Region '%s' already exists. Rename or remove it before duplicating.", new_name);
			}
			return false;
		}
		new_members.push_back(new_name);
	}

	RebuildGroupBounds();
	RECT bounds = source_group->second.bounds;
	int dx = (bounds.right - bounds.left) + 16;
	int dy = (bounds.bottom - bounds.top) + 16;
	COLORREF new_color = NextGroupColor(source_group->second.color);

	for (size_t i = 0; i < source_group->second.members.size(); ++i) {
		RMapCI source_region = p_tablemap->r$()->find(source_group->second.members[i].GetString());
		if (source_region == p_tablemap->r$()->end()) {
			continue;
		}

		STablemapRegion new_region = source_region->second;
		new_region.name = new_members[i];
		new_region.left += dx;
		new_region.right += dx;
		new_region.top += dy;
		new_region.bottom += dy;
		if (!p_tablemap->r$_insert(new_region)) {
			if (error_message != NULL) {
				error_message->Format("Failed to create region '%s'.", new_region.name);
			}
			return false;
		}
	}

	const char *color_fields[] = { "name", "color", "a", "r", "g", "b", "pointx", "pointy", "pointrelx", "pointrely", "use_default", "threshold", "use_crop", "crop", "box", "psm" };
	for (int f = 0; f < (int)(sizeof(color_fields) / sizeof(color_fields[0])); ++f) {
		CString key = ColorPresetKeyForIndex(-1, color_fields[f]);
		CString value = p_tablemap->GetTMSymbol(key);
		if (!value.IsEmpty()) {
			UpsertSymbol(key, value);
		}
	}

	CString count_text = p_tablemap->GetTMSymbol("oscoloripcount");
	int original_color_preset_count = count_text.IsEmpty() ? 0 : atoi(count_text.GetString());
	int next_color_preset = original_color_preset_count;
	for (int preset = 0; preset < original_color_preset_count; ++preset) {
		CString preset_name = p_tablemap->GetTMSymbol(ColorPresetKeyForIndex(preset, "name"));
		if (preset_name.CompareNoCase("Default") == 0) {
			for (int f = 0; f < (int)(sizeof(color_fields) / sizeof(color_fields[0])); ++f) {
				CString value = p_tablemap->GetTMSymbol(ColorPresetKeyForIndex(preset, color_fields[f]));
				if (!value.IsEmpty()) {
					UpsertSymbol(ColorPresetKeyForIndex(-1, color_fields[f]), value);
				}
			}
			continue;
		}

		int new_preset = next_color_preset++;
		for (int f = 0; f < (int)(sizeof(color_fields) / sizeof(color_fields[0])); ++f) {
			CString field = color_fields[f];
			CString value = p_tablemap->GetTMSymbol(ColorPresetKeyForIndex(preset, color_fields[f]));
			if (field == "name") {
				if (value.IsEmpty()) {
					value.Format("Color preset %s", target_player);
				} else {
					value.Format("%s %s", value, target_player);
				}
			}
			UpsertSymbol(ColorPresetKeyForIndex(new_preset, color_fields[f]), value);
		}

		CString old_x_text = p_tablemap->GetTMSymbol(ColorPresetKeyForIndex(preset, "pointx"));
		CString old_y_text = p_tablemap->GetTMSymbol(ColorPresetKeyForIndex(preset, "pointy"));
		CString old_rel_x_text = p_tablemap->GetTMSymbol(ColorPresetKeyForIndex(preset, "pointrelx"));
		CString old_rel_y_text = p_tablemap->GetTMSymbol(ColorPresetKeyForIndex(preset, "pointrely"));
		if ((!old_x_text.IsEmpty() && !old_y_text.IsEmpty()) || (!old_rel_x_text.IsEmpty() && !old_rel_y_text.IsEmpty())) {
			int old_x = old_x_text.IsEmpty() ? 0 : atoi(old_x_text.GetString());
			int old_y = old_y_text.IsEmpty() ? 0 : atoi(old_y_text.GetString());
			int new_x = old_x + dx;
			int new_y = old_y + dy;
			int new_rel_x = old_rel_x_text.IsEmpty() ? 0 : atoi(old_rel_x_text.GetString());
			int new_rel_y = old_rel_y_text.IsEmpty() ? 0 : atoi(old_rel_y_text.GetString());
			for (size_t member_index = 0; member_index < source_group->second.members.size(); ++member_index) {
				RMapCI source_region = p_tablemap->r$()->find(source_group->second.members[member_index].GetString());
				RMapCI new_region = p_tablemap->r$()->find(new_members[member_index].GetString());
				if (source_region == p_tablemap->r$()->end() || new_region == p_tablemap->r$()->end()) {
					continue;
				}
				if (!old_rel_x_text.IsEmpty() && !old_rel_y_text.IsEmpty()
						&& (old_x_text.IsEmpty() || old_y_text.IsEmpty())) {
					new_x = (int)new_region->second.left + new_rel_x;
					new_y = (int)new_region->second.top + new_rel_y;
					break;
				}
				if (!old_x_text.IsEmpty() && !old_y_text.IsEmpty()
						&& old_x >= (int)source_region->second.left && old_x <= (int)source_region->second.right
						&& old_y >= (int)source_region->second.top && old_y <= (int)source_region->second.bottom) {
					if (old_rel_x_text.IsEmpty() || old_rel_y_text.IsEmpty()) {
						new_rel_x = old_x - (int)source_region->second.left;
						new_rel_y = old_y - (int)source_region->second.top;
					}
					new_x = (int)new_region->second.left + new_rel_x;
					new_y = (int)new_region->second.top + new_rel_y;
					break;
				}
			}

			CString point_text;
			point_text.Format("%d", new_x);
			UpsertSymbol(ColorPresetKeyForIndex(new_preset, "pointx"), point_text);
			point_text.Format("%d", new_y);
			UpsertSymbol(ColorPresetKeyForIndex(new_preset, "pointy"), point_text);
			point_text.Format("%d", new_rel_x);
			UpsertSymbol(ColorPresetKeyForIndex(new_preset, "pointrelx"), point_text);
			point_text.Format("%d", new_rel_y);
			UpsertSymbol(ColorPresetKeyForIndex(new_preset, "pointrely"), point_text);

			CString point_region_name;
			point_region_name.Format("oscolorip%dpoint", new_preset);
			if (p_tablemap->r$()->find(point_region_name.GetString()) == p_tablemap->r$()->end()) {
				STablemapRegion point_region;
				point_region.name = point_region_name;
				point_region.left = new_x > 1 ? new_x - 1 : 0;
				point_region.top = new_y > 1 ? new_y - 1 : 0;
				point_region.right = point_region.left + 3;
				point_region.bottom = point_region.top + 3;
				point_region.color = RGB(255, 0, 255);
				point_region.radius = 0;
				point_region.transform = "N";
				point_region.use_default = true;
				point_region.threshold = -1;
				point_region.use_cropping = false;
				point_region.crop_size = -1;
				p_tablemap->r$_insert(point_region);
				new_members.push_back(point_region_name);
			}
		}
	}
	if (next_color_preset != original_color_preset_count) {
		CString new_count;
		new_count.Format("%d", next_color_preset);
		UpsertSymbol("oscoloripcount", new_count);
	}

	SOpenScrapeRegionGroup new_group;
	new_group.name = new_group_name;
	new_group.color = new_color;
	new_group.members = new_members;
	new_group.locked = false;
	region_groups[new_group_name] = new_group;
	RebuildGroupBounds();
	SaveGroupsToTablemap();
	ClearRegionSelection();
	GetDocument()->SetModifiedFlag(true);
	Invalidate(false);
	return true;
}

void COpenScrapeView::SelectRegionsInsideRect(RECT rect)
{
	selected_regions.clear();
	for (RMapCI r_iter = p_tablemap->r$()->begin(); r_iter != p_tablemap->r$()->end(); r_iter++) {
		if ((LONG)r_iter->second.left >= rect.left
				&& (LONG)r_iter->second.top >= rect.top
				&& (LONG)r_iter->second.right <= rect.right
				&& (LONG)r_iter->second.bottom <= rect.bottom) {
			selected_regions.push_back(r_iter->second.name);
		}
	}
	Invalidate(false);
}

void COpenScrapeView::SaveGroupsToTablemap()
{
	SMap *symbols = p_tablemap->s$();
	std::vector<CString> keys_to_delete;
	for (SMapCI s_iter = symbols->begin(); s_iter != symbols->end(); s_iter++) {
		if (s_iter->second.name.Left(7) == "osgroup") {
			keys_to_delete.push_back(s_iter->second.name);
		}
	}
	for (size_t i = 0; i < keys_to_delete.size(); ++i) {
		p_tablemap->s$_erase(keys_to_delete[i]);
	}

	int group_index = 0;
	for (std::map<CString, SOpenScrapeRegionGroup>::iterator g = region_groups.begin(); g != region_groups.end(); ++g, ++group_index) {
		CString key;
		STablemapSymbol symbol;
		key.Format("osgroup%dname", group_index);
		symbol.name = key;
		symbol.text = g->second.name;
		p_tablemap->s$_insert(symbol);

		key.Format("osgroup%dcolor", group_index);
		symbol.name = key;
		symbol.text.Format("%06x", g->second.color & 0x00ffffff);
		p_tablemap->s$_insert(symbol);

		key.Format("osgroup%dmembers", group_index);
		symbol.name = key;
		symbol.text = "";
		for (size_t i = 0; i < g->second.members.size(); ++i) {
			if (i > 0) {
				symbol.text.Append(",");
			}
			symbol.text.Append(g->second.members[i]);
		}
		p_tablemap->s$_insert(symbol);

		key.Format("osgroup%dlocked", group_index);
		symbol.name = key;
		symbol.text = g->second.locked ? "1" : "0";
		p_tablemap->s$_insert(symbol);
	}

	STablemapSymbol count_symbol;
	count_symbol.name = "osgroupcount";
	count_symbol.text.Format("%d", group_index);
	p_tablemap->s$_insert(count_symbol);
}

void COpenScrapeView::LoadGroupsFromTablemap(bool force)
{
	if (!force && loaded_group_filename == p_tablemap->filename()) {
		return;
	}
	loaded_group_filename = p_tablemap->filename();
	region_groups.clear();
	selected_regions.clear();

	CString count_text = p_tablemap->GetTMSymbol("osgroupcount");
	int group_count = atoi(count_text.GetString());
	for (int i = 0; i < group_count; ++i) {
		CString key;
		key.Format("osgroup%dname", i);
		CString name = p_tablemap->GetTMSymbol(key);
		key.Format("osgroup%dcolor", i);
		CString color = p_tablemap->GetTMSymbol(key);
		key.Format("osgroup%dmembers", i);
		CString members = p_tablemap->GetTMSymbol(key);
		if (name == "" || members == "") {
			continue;
		}
		key.Format("osgroup%dlocked", i);
		CString locked = p_tablemap->GetTMSymbol(key);

		SOpenScrapeRegionGroup group;
		group.name = name;
		group.color = strtoul(color.GetString(), NULL, 16);
		group.locked = (locked == "1");
		int pos = 0;
		CString member = members.Tokenize(",", pos);
		while (member != "") {
			group.members.push_back(member);
			member = members.Tokenize(",", pos);
		}
		region_groups[name] = group;
	}
	RebuildGroupBounds();
}

void COpenScrapeView::CreateGroupFromSelection()
{
	if (selected_regions.size() < 2) {
		AfxMessageBox("Select at least two regions first.");
		return;
	}
	CString group_name = PromptForGroupName();
	if (group_name == "") {
		return;
	}

	SOpenScrapeRegionGroup group;
	group.name = group_name;
	group.color = SelectedGroupColor();
	group.members = selected_regions;
	group.locked = false;
	region_groups[group_name] = group;
	RebuildGroupBounds();
	SaveGroupsToTablemap();
	GetDocument()->SetModifiedFlag(true);
	if (theApp.m_TableMapDlg != NULL) {
		theApp.m_TableMapDlg->update_tree("Groups");
	}
	Invalidate(false);
}

void COpenScrapeView::SetGroupBoxMode(bool enabled)
{
	group_box_mode = enabled;
	group_box_started = false;
	Invalidate(false);
}

void COpenScrapeView::SetSelectedGroupColor(int color_index)
{
	group_color_index = color_index;
	COLORREF color = SelectedGroupColor();
	for (std::map<CString, SOpenScrapeRegionGroup>::iterator g = region_groups.begin(); g != region_groups.end(); ++g) {
		bool selected_member = false;
		for (size_t i = 0; i < g->second.members.size(); ++i) {
			if (IsRegionSelected(g->second.members[i])) {
				selected_member = true;
				break;
			}
		}
		if (selected_member) {
			g->second.color = color;
		}
	}
	SaveGroupsToTablemap();
	GetDocument()->SetModifiedFlag(true);
	Invalidate(false);
}

// A region uses decimal splitting if its name contains one of the configured
// field-type substrings (e.g. region "p0balance" matches field type "balance").
bool COpenScrapeView::RegionUsesDecimalSplit(const CString &name)
{
	CString lower = name;
	lower.MakeLower();
	for (size_t i = 0; i < _decimal_fields.size(); ++i) {
		CString f = _decimal_fields[i];
		f.MakeLower();
		if (!f.IsEmpty() && lower.Find(f) != -1) {
			return true;
		}
	}
	return false;
}

// Draw a red vertical line at the decimal separator for every decimal-split
// region, mirroring the red split-marker shown in trainer.exe. Results are
// cached per frame (keyed on the attached bitmap handle) so OnDraw stays cheap.
void COpenScrapeView::DrawDecimalSplitLines(CDC *pDC)
{
	COpenScrapeDoc *pDoc = GetDocument();
	if (pDoc == NULL || pDoc->attached_bitmap == NULL || p_tablemap == NULL) {
		return;
	}

	// Refresh the configured field-type list (cheap; once per dialog change).
	if (!_decimal_fields_loaded) {
		_decimal_fields.clear();
		if (p_tablemap_db != NULL) {
			p_tablemap_db->GetSettingArray("decimal_split_fields", "fields", &_decimal_fields);
		}
		_decimal_fields_loaded = true;
		_decimal_cached_bmp = NULL;   // force recompute
	}
	if (_decimal_fields.empty()) {
		return;
	}

	// Recompute split offsets only when the frame (bitmap) changes.
	if (pDoc->attached_bitmap != _decimal_cached_bmp) {
		_decimal_cached_bmp = pDoc->attached_bitmap;
		_decimal_lines.clear();

		int fw = pDoc->attached_rect.right - pDoc->attached_rect.left;
		int fh = pDoc->attached_rect.bottom - pDoc->attached_rect.top;
		if (fw > 0 && fh > 0) {
			int stride = ((fw * 3 + 3) & ~3);
			std::vector<BYTE> buf((size_t)stride * fh);
			BITMAPINFO bi;
			ZeroMemory(&bi, sizeof(bi));
			bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bi.bmiHeader.biWidth = fw;
			bi.bmiHeader.biHeight = -fh;   // top-down
			bi.bmiHeader.biPlanes = 1;
			bi.bmiHeader.biBitCount = 24;
			bi.bmiHeader.biCompression = BI_RGB;
			HDC hdcScreen = CreateDC("DISPLAY", NULL, NULL, NULL);
			int got = GetDIBits(hdcScreen, pDoc->attached_bitmap, 0, fh, &buf[0], &bi, DIB_RGB_COLORS);
			DeleteDC(hdcScreen);
			if (got == fh) {
				for (RMapCI r = p_tablemap->r$()->begin(); r != p_tablemap->r$()->end(); ++r) {
					if (!RegionUsesDecimalSplit(r->second.name)) continue;
					int rl = (int)r->second.left, rt = (int)r->second.top;
					int rr = (int)r->second.right, rb = (int)r->second.bottom;
					int rw = rr - rl + 1, rh = rb - rt + 1;
					if (rl < 0 || rt < 0 || rr >= fw || rb >= fh || rw < 3 || rh < 3) continue;
					const BYTE *p = &buf[(size_t)rt * stride + (size_t)rl * 3];
					int sx = FindDecimalSplitX(p, rw, rh, stride);
					if (sx >= 0) {
						_decimal_lines.push_back(std::make_pair(r->second.name, sx));
					}
				}
			}
		}
	}

	// Draw the cached lines at each region's current position (so a dragged
	// region's marker follows it without recomputing).
	CPen split_pen;
	split_pen.CreatePen(PS_SOLID, 1, COLOR_RED);
	CPen *old = pDC->SelectObject(&split_pen);
	for (size_t i = 0; i < _decimal_lines.size(); ++i) {
		RMapCI r = p_tablemap->r$()->find(_decimal_lines[i].first.GetString());
		if (r == p_tablemap->r$()->end()) continue;
		int x = (int)r->second.left + _decimal_lines[i].second;
		pDC->MoveTo(x, (int)r->second.top);
		pDC->LineTo(x, (int)r->second.bottom + 1);
	}
	pDC->SelectObject(old);
}

void COpenScrapeView::DrawRegionGroups(CDC *pDC)
{
	RebuildGroupBounds();
	for (std::map<CString, SOpenScrapeRegionGroup>::iterator g = region_groups.begin(); g != region_groups.end(); ++g) {
		if (g->second.members.empty()) {
			continue;
		}
		CPen group_pen;
		// Locked groups get a dashed border so they're visually distinct.
		// (PS_DASH only renders dashes at width 1.)
		group_pen.CreatePen(g->second.locked ? PS_DASH : PS_SOLID,
			g->second.locked ? 1 : 2, g->second.color);
		CPen *old_pen = pDC->SelectObject(&group_pen);
		CGdiObject *old_brush = pDC->SelectStockObject(NULL_BRUSH);
		pDC->Rectangle(g->second.bounds.left - 4, g->second.bounds.top - 18,
			g->second.bounds.right + 5, g->second.bounds.bottom + 5);
		pDC->SetTextColor(g->second.color);
		pDC->SetBkMode(TRANSPARENT);
		CString label = g->second.locked ? (g->second.name + " [locked]") : g->second.name;
		pDC->TextOut(g->second.bounds.left - 2, g->second.bounds.top - 17, label);
		pDC->SelectObject(old_pen);
		pDC->SelectObject(old_brush);
	}
}

void COpenScrapeView::DrawMagnifierPreview(CDC *pDC)
{
	COpenScrapeDoc* pDoc = GetDocument();
	if (!show_preview || pDoc->attached_bitmap == NULL) {
		preview_close_rect.SetRectEmpty();
		return;
	}

	CRect client;
	GetClientRect(&client);
	const int source_size = 24;
	const int scale = 4;
	const int preview_size = source_size * scale;
	const int pad = 6;
	CRect preview_rect(client.right - preview_size - pad, client.top + pad,
		client.right - pad, client.top + pad + preview_size);
	preview_close_rect.SetRect(preview_rect.right - 16, preview_rect.top,
		preview_rect.right, preview_rect.top + 16);

	HDC hdcScreen = CreateDC("DISPLAY", NULL, NULL, NULL);
	HDC hdcBitmap = CreateCompatibleDC(hdcScreen);
	HBITMAP oldBitmap = (HBITMAP)SelectObject(hdcBitmap, pDoc->attached_bitmap);

	CBrush background(RGB(32, 32, 32));
	pDC->FillRect(&preview_rect, &background);
	for (int y = 0; y < source_size; ++y) {
		for (int x = 0; x < source_size; ++x) {
			int source_x = preview_point.x - source_size / 2 + x;
			int source_y = preview_point.y - source_size / 2 + y;
			COLORREF pixel = RGB(0, 0, 0);
			if (source_x >= 0 && source_y >= 0
					&& source_x < pDoc->attached_rect.right - pDoc->attached_rect.left
					&& source_y < pDoc->attached_rect.bottom - pDoc->attached_rect.top) {
				COLORREF sampled = GetPixel(hdcBitmap, source_x, source_y);
				if (sampled != CLR_INVALID) {
					pixel = sampled;
				}
			}
			CBrush pixel_brush(pixel);
			CRect pixel_rect(preview_rect.left + x * scale, preview_rect.top + y * scale,
				preview_rect.left + (x + 1) * scale, preview_rect.top + (y + 1) * scale);
			pDC->FillRect(&pixel_rect, &pixel_brush);
		}
	}

	int source_left = preview_point.x - source_size / 2;
	int source_top = preview_point.y - source_size / 2;
	int saved_dc = pDC->SaveDC();
	pDC->IntersectClipRect(&preview_rect);
	for (RMapCI r_iter = p_tablemap->r$()->begin(); r_iter != p_tablemap->r$()->end(); ++r_iter) {
		CRect source_rect((int)r_iter->second.left - 1, (int)r_iter->second.top - 1,
			(int)r_iter->second.right + 2, (int)r_iter->second.bottom + 2);
		CRect visible_rect(source_left, source_top, source_left + source_size, source_top + source_size);

		// Orange overlay for the optional second rectangle (Color transform only).
		// Drawn BEFORE the rect-1 visibility check so it still appears when the first
		// rectangle is outside the magnifier window (rect 2 is a separate area).
		if (r_iter->second.rect2_enabled
			&& r_iter->second.transform.GetLength() > 0
			&& r_iter->second.transform[0] == 'C') {
			CRect source_rect2((int)r_iter->second.left2 - 1, (int)r_iter->second.top2 - 1,
				(int)r_iter->second.right2 + 2, (int)r_iter->second.bottom2 + 2);
			CRect intersection2;
			if (intersection2.IntersectRect(&source_rect2, &visible_rect)) {
				CPen region_pen2(PS_SOLID, 1, RGB(255, 140, 0));   // orange
				CPen *old_region_pen2 = pDC->SelectObject(&region_pen2);
				CGdiObject *old_region_brush2 = pDC->SelectStockObject(NULL_BRUSH);
				CRect draw_rect2(
					preview_rect.left + (source_rect2.left - source_left) * scale,
					preview_rect.top + (source_rect2.top - source_top) * scale,
					preview_rect.left + (source_rect2.right - source_left) * scale,
					preview_rect.top + (source_rect2.bottom - source_top) * scale);
				pDC->Rectangle(&draw_rect2);
				pDC->SelectObject(old_region_pen2);
				pDC->SelectObject(old_region_brush2);
			}
		}

		CRect intersection;
		if (!intersection.IntersectRect(&source_rect, &visible_rect)) {
			continue;
		}

		COLORREF region_color = COLOR_RED;
		GetGroupColorForRegion(r_iter->second.name, &region_color);
		if (IsRegionSelected(r_iter->second.name)) {
			region_color = COLOR_YELLOW;
		}
		CPen region_pen(PS_SOLID, IsRegionSelected(r_iter->second.name) ? 2 : 1, region_color);
		CPen *old_region_pen = pDC->SelectObject(&region_pen);
		CGdiObject *old_region_brush = pDC->SelectStockObject(NULL_BRUSH);
		CRect draw_rect(
			preview_rect.left + (source_rect.left - source_left) * scale,
			preview_rect.top + (source_rect.top - source_top) * scale,
			preview_rect.left + (source_rect.right - source_left) * scale,
			preview_rect.top + (source_rect.bottom - source_top) * scale);
		pDC->Rectangle(&draw_rect);
		pDC->SelectObject(old_region_pen);
		pDC->SelectObject(old_region_brush);
	}

	RebuildGroupBounds();
	for (std::map<CString, SOpenScrapeRegionGroup>::iterator g = region_groups.begin(); g != region_groups.end(); ++g) {
		if (g->second.members.empty()) {
			continue;
		}
		CRect source_rect(g->second.bounds.left - 4, g->second.bounds.top - 18,
			g->second.bounds.right + 5, g->second.bounds.bottom + 5);
		CRect visible_rect(source_left, source_top, source_left + source_size, source_top + source_size);
		CRect intersection;
		if (!intersection.IntersectRect(&source_rect, &visible_rect)) {
			continue;
		}

		CPen group_pen(PS_SOLID, 2, g->second.color);
		CPen *old_group_pen = pDC->SelectObject(&group_pen);
		CGdiObject *old_group_brush = pDC->SelectStockObject(NULL_BRUSH);
		CRect draw_rect(
			preview_rect.left + (source_rect.left - source_left) * scale,
			preview_rect.top + (source_rect.top - source_top) * scale,
			preview_rect.left + (source_rect.right - source_left) * scale,
			preview_rect.top + (source_rect.bottom - source_top) * scale);
		pDC->Rectangle(&draw_rect);
		pDC->SelectObject(old_group_pen);
		pDC->SelectObject(old_group_brush);
	}
	pDC->RestoreDC(saved_dc);

	int cursor_center_x = preview_rect.left + (source_size / 2) * scale + scale / 2;
	int cursor_center_y = preview_rect.top + (source_size / 2) * scale + scale / 2;
	CPen cursor_shadow_pen(PS_SOLID, 3, RGB(0, 0, 0));
	CPen *old_cursor_pen = pDC->SelectObject(&cursor_shadow_pen);
	pDC->MoveTo(cursor_center_x - 9, cursor_center_y);
	pDC->LineTo(cursor_center_x - 3, cursor_center_y);
	pDC->MoveTo(cursor_center_x + 3, cursor_center_y);
	pDC->LineTo(cursor_center_x + 9, cursor_center_y);
	pDC->MoveTo(cursor_center_x, cursor_center_y - 9);
	pDC->LineTo(cursor_center_x, cursor_center_y - 3);
	pDC->MoveTo(cursor_center_x, cursor_center_y + 3);
	pDC->LineTo(cursor_center_x, cursor_center_y + 9);
	pDC->SelectObject(old_cursor_pen);

	CPen cursor_pen(PS_SOLID, 1, RGB(255, 255, 255));
	old_cursor_pen = pDC->SelectObject(&cursor_pen);
	pDC->MoveTo(cursor_center_x - 9, cursor_center_y);
	pDC->LineTo(cursor_center_x - 3, cursor_center_y);
	pDC->MoveTo(cursor_center_x + 3, cursor_center_y);
	pDC->LineTo(cursor_center_x + 9, cursor_center_y);
	pDC->MoveTo(cursor_center_x, cursor_center_y - 9);
	pDC->LineTo(cursor_center_x, cursor_center_y - 3);
	pDC->MoveTo(cursor_center_x, cursor_center_y + 3);
	pDC->LineTo(cursor_center_x, cursor_center_y + 9);
	pDC->SelectObject(old_cursor_pen);

	CPen border_pen(PS_SOLID, 1, RGB(255, 255, 255));
	CPen *old_pen = pDC->SelectObject(&border_pen);
	CGdiObject *old_brush = pDC->SelectStockObject(NULL_BRUSH);
	pDC->Rectangle(&preview_rect);
	pDC->SelectObject(old_pen);
	pDC->SelectObject(old_brush);

	CBrush close_brush(RGB(40, 40, 40));
	pDC->FillRect(&preview_close_rect, &close_brush);
	pDC->SetTextColor(RGB(255, 255, 255));
	pDC->SetBkMode(TRANSPARENT);
	pDC->DrawText("X", 1, &preview_close_rect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

	SelectObject(hdcBitmap, oldBitmap);
	DeleteDC(hdcBitmap);
	DeleteDC(hdcScreen);
}

// --- Live card-result overlays --------------------------------------------------

// The card rank/suit/community regions we annotate with their detected value:
//   c0cardface0 .. c0cardface4   (community cards)
//   p<0-8>cardface<0-1>rank      (player card ranks)
//   p<0-8>cardface<0-1>suit      (player card suits)
bool COpenScrapeView::IsCardResultRegion(const CString &name)
{
	if (name.GetLength() == 11 && name.Left(10) == "c0cardface") {
		TCHAR d = name[10];
		return (d >= '0' && d <= '4');
	}
	if (name.GetLength() == 15 && name[0] == 'p'
		&& name[1] >= '0' && name[1] <= '8'
		&& name.Mid(2, 8) == "cardface"
		&& (name[10] == '0' || name[10] == '1')) {
		CString kind = name.Mid(11);
		return (kind == "rank" || kind == "suit");
	}
	return false;
}

// Run the region's transform against the current table bitmap and return the text.
CString COpenScrapeView::ScrapeRegionResult(RMapCI r_iter)
{
	CString result;
	COpenScrapeDoc* pDoc = GetDocument();
	if (pDoc == NULL || pDoc->attached_bitmap == NULL) {
		return result;
	}
	int w = (int)r_iter->second.right - (int)r_iter->second.left + 1;
	int h = (int)r_iter->second.bottom - (int)r_iter->second.top + 1;
	if (w <= 0 || h <= 0) {
		return result;
	}

	HDC hdcScreen = CreateDC("DISPLAY", NULL, NULL, NULL);
	HDC hdcOrig = CreateCompatibleDC(hdcScreen);
	HBITMAP oldOrig = (HBITMAP)SelectObject(hdcOrig, pDoc->attached_bitmap);

	HDC hdcRegion = CreateCompatibleDC(hdcScreen);
	HBITMAP bmpRegion = CreateCompatibleBitmap(hdcScreen, w, h);
	HBITMAP oldRegion = (HBITMAP)SelectObject(hdcRegion, bmpRegion);

	BitBlt(hdcRegion, 0, 0, w, h, hdcOrig,
		r_iter->second.left, r_iter->second.top, SRCCOPY);

	CTransform trans;
	trans.DoTransform(r_iter, hdcRegion, &result);

	SelectObject(hdcRegion, oldRegion);
	DeleteObject(bmpRegion);
	DeleteDC(hdcRegion);
	SelectObject(hdcOrig, oldOrig);
	DeleteDC(hdcOrig);
	DeleteDC(hdcScreen);
	return result;
}

// Recompute the cached results only when the captured frame changes (the image
// compares are too costly to repeat on every repaint). Mirrors the decimal-line cache.
void COpenScrapeView::RefreshCardResultsIfNeeded(COpenScrapeDoc *pDoc)
{
	if (pDoc == NULL || p_tablemap == NULL) {
		return;
	}
	if (pDoc->attached_bitmap == _card_results_bmp) {
		return;   // same frame -> cache still valid
	}
	_card_results_bmp = pDoc->attached_bitmap;
	_card_results.clear();
	if (pDoc->attached_bitmap == NULL) {
		return;
	}

	// Gather the card regions to detect this frame.
	std::vector<RMapCI> regions;
	for (RMapCI r_iter = p_tablemap->r$()->begin(); r_iter != p_tablemap->r$()->end(); ++r_iter) {
		if (IsCardResultRegion(r_iter->second.name)) regions.push_back(r_iter);
	}
	if (regions.empty()) return;

	EnsureWorkerPool();

	// The pdiff metric lazily initialises a few function-statics on first use; warm
	// them once on the UI thread so concurrent first calls don't race. Also the
	// serial path for single-worker setups.
	static bool s_metric_warmed = false;
	if (g_card_pool.ThreadCount() <= 1 || !s_metric_warmed) {
		for (size_t i = 0; i < regions.size(); ++i)
			_card_results[regions[i]->second.name] = ScrapeRegionResult(regions[i]);
		s_metric_warmed = true;
		return;
	}

	// Parallel path: extract each region's pixels on this (UI) thread, then run the
	// transform/compare on worker threads (each job owns its own bitmap + DCs). Wait
	// for all to finish so the result set is complete when we return.
	CRITICAL_SECTION cs;
	InitializeCriticalSection(&cs);
	std::map<CString, CString> *results = &_card_results;
	HANDLE done = CreateEvent(NULL, TRUE, FALSE, NULL);
	volatile LONG remaining = 0;
	int submitted = 0;

	for (size_t i = 0; i < regions.size(); ++i) {
		RMapCI reg = regions[i];
		int w = (int)reg->second.right - (int)reg->second.left + 1;
		int h = (int)reg->second.bottom - (int)reg->second.top + 1;
		HBITMAP bmp = ExtractRegionBitmap(pDoc->attached_bitmap, reg->second.left, reg->second.top, w, h);
		if (bmp == NULL) continue;
		CString name = reg->second.name;
		InterlockedIncrement(&remaining);
		++submitted;
		g_card_pool.Submit([reg, bmp, name, &cs, results, &remaining, done]() {
			CString r = TransformRegionBitmap(reg, bmp);
			DeleteObject(bmp);
			EnterCriticalSection(&cs);
			(*results)[name] = r;
			LeaveCriticalSection(&cs);
			if (InterlockedDecrement(&remaining) == 0) SetEvent(done);
		});
	}

	if (submitted > 0) {
		WaitForSingleObject(done, 15000);   // join (generous timeout safety net)
	}
	CloseHandle(done);
	DeleteCriticalSection(&cs);
}

// (Re)size the detection worker pool from the shared "Parallel Workers" settings,
// split across running Vision instances (theApp.sessionnum) to avoid oversubscription.
void COpenScrapeView::EnsureWorkerPool()
{
	int cpus = 0, wpc = 1;
	if (p_tablemap_db != NULL) {
		CString c = p_tablemap_db->GetSettingString("parallel_workers", "num_cpus");
		CString w = p_tablemap_db->GetSettingString("parallel_workers", "workers_per_cpu");
		if (!c.IsEmpty()) cpus = atoi(c);
		if (!w.IsEmpty()) wpc = atoi(w);
	}
	int instances = (theApp.sessionnum > 0) ? theApp.sessionnum : 1;
	int want = ParallelWorkerCountForInstances(cpus, wpc, instances);
	if (want != g_card_pool_size) {
		g_card_pool.Start(want);
		g_card_pool_size = want;
	}
}

// Draw the detected value inside the region box at reduced opacity, with suit letters
// rendered as pip symbols (h hearts, d diamonds, c clubs, s spades).
void COpenScrapeView::DrawCardResultOverlay(CDC *pDC, RMapCI r_iter)
{
	std::map<CString, CString>::const_iterator it = _card_results.find(r_iter->second.name);
	if (it == _card_results.end() || it->second.IsEmpty()) {
		return;   // nothing detected -> nothing to show
	}
	const CString &result = it->second;

	CStringW disp;
	bool sawRed = false, sawBlack = false;
	for (int i = 0; i < result.GetLength(); ++i) {
		switch (result[i]) {
			case 'h': disp += L"\x2665"; sawRed = true; break;    // heart
			case 'd': disp += L"\x2666"; sawRed = true; break;    // diamond
			case 'c': disp += L"\x2663"; sawBlack = true; break;  // club
			case 's': disp += L"\x2660"; sawBlack = true; break;  // spade
			default:  disp += (WCHAR)result[i]; break;            // rank / other
		}
	}
	COLORREF color = sawRed ? RGB(220, 0, 0)
		: sawBlack ? RGB(0, 0, 0)
		: RGB(0, 60, 220);   // rank-only: neutral blue

	int rh = (int)r_iter->second.bottom - (int)r_iter->second.top;
	int fpx = (int)(rh * 1.6) + 5;   // magnified a bit
	if (fpx < 13) fpx = 13;
	if (fpx > 30) fpx = 30;

	Gdiplus::Graphics g(pDC->GetSafeHdc());
	g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
	Gdiplus::FontFamily ff(L"Arial");
	Gdiplus::Font font(&ff, (Gdiplus::REAL)fpx, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);

	// Measure with the SAME format we draw with (and NoClip/NoWrap) so the glyph is
	// never cut off by a too-tight box.
	Gdiplus::StringFormat fmt;
	fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
	fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
	fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsNoClip);

	Gdiplus::RectF measured;
	g.MeasureString(disp, -1, &font, Gdiplus::RectF(0, 0, 1000, 1000), &fmt, &measured);
	const Gdiplus::REAL padX = 6.0f, padY = 3.0f, gap = 3.0f;
	Gdiplus::REAL boxW = measured.Width + padX * 2;
	Gdiplus::REAL boxH = measured.Height + padY * 2;
	CRect client;
	GetClientRect(&client);

	// Vertical: float just ABOVE the region; if there's no room above, drop just BELOW
	// it. Either way the label never sits on top of the region itself.
	Gdiplus::REAL aboveY = (Gdiplus::REAL)r_iter->second.top - boxH - gap;
	Gdiplus::REAL belowY = (Gdiplus::REAL)r_iter->second.bottom + gap;
	Gdiplus::REAL boxY;
	if (aboveY >= (Gdiplus::REAL)client.top) {
		boxY = aboveY;
	} else if (belowY + boxH <= (Gdiplus::REAL)client.bottom) {
		boxY = belowY;
	} else {
		boxY = aboveY;   // tiny view: neither fully fits; keep it above
	}

	// Horizontal: centre on the region, then nudge inside the view edges so it isn't
	// clipped. Shifting only sideways keeps it clear of the region (which is above/below).
	Gdiplus::REAL cx = (Gdiplus::REAL)((int)r_iter->second.left + (int)r_iter->second.right) / 2.0f;
	Gdiplus::REAL boxX = cx - boxW / 2.0f;
	if (boxX + boxW > (Gdiplus::REAL)client.right) boxX = (Gdiplus::REAL)client.right - boxW;
	if (boxX < (Gdiplus::REAL)client.left)         boxX = (Gdiplus::REAL)client.left;

	Gdiplus::RectF box(boxX, boxY, boxW, boxH);

	// Translucent background so the card/table underneath stays visible; thin border;
	// full-opacity coloured glyph for legibility.
	Gdiplus::SolidBrush bg(Gdiplus::Color(105, 250, 250, 245));
	g.FillRectangle(&bg, box);
	Gdiplus::Pen border(Gdiplus::Color(150, 40, 40, 40), 1.0f);
	g.DrawRectangle(&border, boxX, boxY, boxW, boxH);

	Gdiplus::SolidBrush fg(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
	g.DrawString(disp, -1, &font, box, &fmt, &fg);
}

// --- Auto card capture ---------------------------------------------------------

void COpenScrapeView::ToggleAutoCapture()
{
	_auto_capture = !_auto_capture;
	if (_auto_capture) {
		SetTimer(AUTO_CAPTURE_TIMER, AutoCaptureIntervalMs(), NULL);
		AutoCaptureTick();   // grab one immediately
	} else {
		KillTimer(AUTO_CAPTURE_TIMER);
	}
	if (theApp.m_pMainWnd) theApp.m_pMainWnd->Invalidate(FALSE);   // refresh the toolbar check
}

void COpenScrapeView::OnAutoCaptureIntervalChanged(int interval_ms)
{
	if (_auto_capture) {
		if (interval_ms < 250) interval_ms = 250;
		SetTimer(AUTO_CAPTURE_TIMER, interval_ms, NULL);
	}
}

void COpenScrapeView::OnTimer(UINT_PTR nIDEvent)
{
	if (nIDEvent == AUTO_CAPTURE_TIMER) {
		AutoCaptureTick();
		return;
	}
	CView::OnTimer(nIDEvent);
}

// A stable signature of the current detected card values, so identical situations
// aren't stored twice in a row.
CString COpenScrapeView::CaptureSignature()
{
	CString sig;
	for (std::map<CString, CString>::const_iterator it = _card_results.begin(); it != _card_results.end(); ++it) {
		sig += it->first; sig += "="; sig += it->second; sig += ";";
	}
	return sig;
}

// True when any player card ranks/suits are detected (p0-p8), or when all five
// community cards are present.
bool COpenScrapeView::CardCaptureConditionMet()
{
	bool player_hit = false;
	int community = 0;
	for (std::map<CString, CString>::const_iterator it = _card_results.begin(); it != _card_results.end(); ++it) {
		if (it->second.IsEmpty()) continue;
		const CString &n = it->first;
		if (n.GetLength() == 11 && n.Left(10) == "c0cardface") {
			++community;                       // c0cardface0..4
		} else if (n.GetLength() == 15 && n[0] == 'p'
				&& n[1] >= '0' && n[1] <= '8' && n.Mid(2, 8) == "cardface") {
			player_hit = true;                 // p<0-8>cardface<0/1>{rank|suit}
		}
	}
	return player_hit || (community >= 5);
}

void COpenScrapeView::AutoCaptureTick()
{
	COpenScrapeDoc *pDoc = GetDocument();
	CMainFrame *mf = (CMainFrame *)AfxGetMainWnd();
	if (pDoc == NULL || mf == NULL) return;
	if (pDoc->attached_hwnd == NULL || !::IsWindow(pDoc->attached_hwnd)) return;

	mf->SaveBmpPbits();                       // fresh frame -> attached_bitmap
	if (pDoc->attached_bitmap == NULL) return;

	RefreshCardResultsIfNeeded(pDoc);         // run card detection on the new frame (~slow)

	// Only buffer when the trigger is met AND the detected values changed since the
	// last stored frame (don't pile up screenshots of the same situation).
	if (CardCaptureConditionMet() && CaptureSignature() != _last_capture_sig) {
		_last_capture_sig = CaptureSignature();
		int w = pDoc->attached_rect.right - pDoc->attached_rect.left;
		int h = pDoc->attached_rect.bottom - pDoc->attached_rect.top;
		HBITMAP copy = DuplicateHBitmap(pDoc->attached_bitmap, w, h);
		if (copy != NULL) {
			const size_t kMaxBuffer = 400;
			if (_capture_buffer.size() >= kMaxBuffer) {
				if (_capture_buffer.front()) DeleteObject(_capture_buffer.front());
				_capture_buffer.erase(_capture_buffer.begin());
				_capture_sizes.erase(_capture_sizes.begin());
			}
			_capture_buffer.push_back(copy);
			_capture_sizes.push_back(CSize(w, h));
			_capture_index = (int)_capture_buffer.size() - 1;
		}
	}
	mf->ForceRedraw();                        // show this frame in the Table View every poll
}

void COpenScrapeView::NavigateCapture(int delta)
{
	if (_capture_buffer.empty()) return;
	int n = (int)_capture_buffer.size();
	if (_capture_index < 0) _capture_index = (delta >= 0) ? 0 : n - 1;
	else _capture_index += delta;
	if (_capture_index < 0) _capture_index = 0;
	if (_capture_index >= n) _capture_index = n - 1;

	COpenScrapeDoc *pDoc = GetDocument();
	if (pDoc == NULL) return;
	int w = _capture_sizes[_capture_index].cx;
	int h = _capture_sizes[_capture_index].cy;
	HBITMAP copy = DuplicateHBitmap(_capture_buffer[_capture_index], w, h);
	if (copy == NULL) return;

	if (pDoc->attached_bitmap) DeleteObject(pDoc->attached_bitmap);
	pDoc->attached_bitmap = copy;
	pDoc->attached_rect.left = 0;
	pDoc->attached_rect.top = 0;
	pDoc->attached_rect.right = w;
	pDoc->attached_rect.bottom = h;
	RefreshWholeApp();
}

// Reload detection for the current frame and repaint everything (per the spec, the
// whole app is refreshed when a buffered screenshot is loaded).
void COpenScrapeView::RefreshWholeApp()
{
	COpenScrapeDoc *pDoc = GetDocument();
	// Force the card-result (GDI+) overlays to recompute for the current frame even
	// if the bitmap handle is unchanged (e.g. the Refresh button on the same frame).
	InvalidateCardResults();
	if (pDoc) RefreshCardResultsIfNeeded(pDoc);
	// Recompute the dialog's Result field + all region displays for the current frame.
	if (theApp.m_TableMapDlg) {
		theApp.m_TableMapDlg->update_display();
		theApp.m_TableMapDlg->Invalidate(FALSE);
	}
	CMainFrame *mf = (CMainFrame *)AfxGetMainWnd();
	if (mf) mf->ForceRedraw();
	Invalidate(FALSE);
}

void COpenScrapeView::ClearCaptureBuffer()
{
	for (size_t i = 0; i < _capture_buffer.size(); ++i)
		if (_capture_buffer[i]) DeleteObject(_capture_buffer[i]);
	_capture_buffer.clear();
	_capture_sizes.clear();
	_capture_index = -1;
	_last_capture_sig = "";   // allow re-capturing a situation after a clear
}

// Per-instance folder for persisted capture PNGs (next to the exe).
static CString CaptureDir()
{
	char module[MAX_PATH] = { 0 };
	GetModuleFileNameA(NULL, module, MAX_PATH);
	CString dir(module);
	int slash = dir.ReverseFind('\\');
	if (slash >= 0) dir = dir.Left(slash);
	CString out;
	out.Format("%s\\vision_captures\\%d\\", dir.GetString(), theApp.sessionnum);
	return out;
}

static int PngEncoderClsid(CLSID *clsid)
{
	UINT num = 0, size = 0;
	Gdiplus::GetImageEncodersSize(&num, &size);
	if (size == 0) return -1;
	Gdiplus::ImageCodecInfo *info = (Gdiplus::ImageCodecInfo *)malloc(size);
	if (info == NULL) return -1;
	Gdiplus::GetImageEncoders(num, size, info);
	int found = -1;
	for (UINT i = 0; i < num; ++i) {
		if (wcscmp(info[i].MimeType, L"image/png") == 0) { *clsid = info[i].Clsid; found = (int)i; break; }
	}
	free(info);
	return found;
}

// Save the capture buffer to disk so it survives a restart.
void COpenScrapeView::SaveCapturesToDisk()
{
	CString dir = CaptureDir();
	SHCreateDirectoryExA(NULL, dir, NULL);
	// Clear any previous PNGs.
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(dir + "capture_*.png", &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do { DeleteFileA(dir + fd.cFileName); } while (FindNextFileA(h, &fd));
		FindClose(h);
	}
	CLSID png;
	if (PngEncoderClsid(&png) < 0) return;
	for (size_t i = 0; i < _capture_buffer.size(); ++i) {
		if (_capture_buffer[i] == NULL) continue;
		Gdiplus::Bitmap *bmp = Gdiplus::Bitmap::FromHBITMAP(_capture_buffer[i], NULL);
		if (bmp == NULL) continue;
		CString path;
		path.Format("%scapture_%04d.png", dir.GetString(), (int)i);
		CStringW wpath(path);
		bmp->Save(wpath, &png, NULL);
		delete bmp;
	}
}

// Reload persisted captures into the buffer on startup.
void COpenScrapeView::LoadCapturesFromDisk()
{
	ClearCaptureBuffer();
	CString dir = CaptureDir();
	// Collect + sort the capture files so they load in saved order.
	std::vector<CString> files;
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(dir + "capture_*.png", &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do { files.push_back(CString(fd.cFileName)); } while (FindNextFileA(h, &fd));
		FindClose(h);
	}
	std::sort(files.begin(), files.end(), [](const CString &a, const CString &b) { return a.Compare(b) < 0; });
	for (size_t i = 0; i < files.size(); ++i) {
		CStringW wpath(dir + files[i]);
		Gdiplus::Bitmap *img = Gdiplus::Bitmap::FromFile(wpath);
		if (img == NULL) continue;
		if (img->GetLastStatus() != Gdiplus::Ok) { delete img; continue; }
		HBITMAP hbm = NULL;
		img->GetHBITMAP(Gdiplus::Color(0, 0, 0), &hbm);
		int w = (int)img->GetWidth(), ht = (int)img->GetHeight();
		delete img;
		if (hbm != NULL) {
			_capture_buffer.push_back(hbm);
			_capture_sizes.push_back(CSize(w, ht));
		}
	}
	_capture_index = -1;   // don't override the live frame until the user navigates
}

// Toolbar "Refresh detection": re-run detection + every field/overlay on the current
// frame (same full refresh used when navigating screenshots).
void COpenScrapeView::RefreshCurrentScreenshot()
{
	RefreshWholeApp();
}

// Toolbar "Clear screenshots": drop the whole capture buffer and repaint.
void COpenScrapeView::ClearCaptures()
{
	ClearCaptureBuffer();
	CMainFrame *mf = (CMainFrame *)AfxGetMainWnd();
	if (mf) mf->ForceRedraw();
	if (theApp.m_pMainWnd) theApp.m_pMainWnd->Invalidate(FALSE);   // refresh toolbar count/state
}

// COpenScrapeView drawing

void COpenScrapeView::OnDraw(CDC* pDC)
{
	COpenScrapeDoc* pDoc = GetDocument();
	ASSERT_VALID(pDoc);
	if (!pDoc)
		return;

	CPen		*pTempPen, oldpen;
	CBrush		*pTempBrush, oldbrush;	
	HDC			hdc = *pDC;
	HDC			hdcScreen = CreateDC("DISPLAY", NULL, NULL, NULL); 
	HDC			hdcCompatible = CreateCompatibleDC(hdcScreen);
	HBITMAP		hbmp = CreateCompatibleBitmap(hdcScreen, 
										      pDoc->attached_rect.right - pDoc->attached_rect.left, 
										      pDoc->attached_rect.bottom - pDoc->attached_rect.top);
	HBITMAP		old_bitmap1, old_bitmap2;
	RECT		crect;
	LoadGroupsFromTablemap();

	// Draw attached window's bitmap as background
	if (pDoc->attached_bitmap)
	{
		old_bitmap1 = (HBITMAP) SelectObject(hdcCompatible, pDoc->attached_bitmap);
		old_bitmap2 = (HBITMAP) SelectObject(hdc, hbmp);
		BitBlt(hdc, 0, 0,
			   pDoc->attached_rect.right - pDoc->attached_rect.left,
			   pDoc->attached_rect.bottom - pDoc->attached_rect.top,
			   hdcCompatible, 0, 0, SRCCOPY);
		SelectObject(hdc, old_bitmap2);
		SelectObject(hdcCompatible, old_bitmap1);
	}
	else
	{
		pTempPen = (CPen*)pDC->SelectObject(null_pen);
		oldpen.FromHandle((HPEN)pTempPen);
		pTempBrush = (CBrush*)pDC->SelectObject(white_brush);
		oldbrush.FromHandle((HBRUSH)pTempBrush);

		GetClientRect(&crect);
		pDC->Rectangle(&crect);

		pDC->SelectObject(oldpen);
		pDC->SelectObject(oldbrush);
	}

	// In training-capture mode, hide every region/template/group overlay so the
	// user draws their box over a clean screenshot. They reappear when the value
	// has been entered (training mode is turned off again).
	if (!_training_mode)
	{
	// Refresh the cached card rank/suit/community detections for this frame.
	RefreshCardResultsIfNeeded(pDoc);
	// Draw the card-result overlays FIRST, so every region's bounding box is drawn on
	// top of them (overlays sit underneath the boxes).
	for (RMapCI r_iter=p_tablemap->r$()->begin(); r_iter!=p_tablemap->r$()->end(); r_iter++)
	{
		if (IsCardResultRegion(r_iter->second.name)) {
			DrawCardResultOverlay(pDC, r_iter);
		}
	}
	// Draw all region rectangles
	for (RMapCI r_iter=p_tablemap->r$()->begin(); r_iter!=p_tablemap->r$()->end(); r_iter++)
	{
		if (IsRegionSelected(r_iter->second.name))
		{
			pTempPen = (CPen*)pDC->SelectObject(yellow_pen);
			oldpen.FromHandle((HPEN)pTempPen);
			pTempBrush = (CBrush*)pDC->SelectObject(GetStockObject(NULL_BRUSH));
			oldbrush.FromHandle((HBRUSH)pTempBrush);

			pDC->Rectangle(r_iter->second.left - 2, r_iter->second.top - 2, r_iter->second.right + 3, r_iter->second.bottom + 3);

			pDC->SelectObject(oldpen);
			pDC->SelectObject(oldbrush);
		}
		else if ( (r_iter->second.name==dragged_region && dragging) ||
				(r_iter->second.name==drawrect_region && drawing_rect && drawing_started) )
		{
			// Set pen and brush
			pTempPen = (CPen*)pDC->SelectObject(black_dot_pen);
			oldpen.FromHandle((HPEN)pTempPen);
			pTempBrush = (CBrush*)pDC->SelectObject(GetStockObject(NULL_BRUSH));
			oldbrush.FromHandle((HBRUSH)pTempBrush);

			pDC->Rectangle(r_iter->second.left-1, r_iter->second.top-1, r_iter->second.right+2, r_iter->second.bottom+2);

			pDC->SelectObject(oldpen);
			pDC->SelectObject(oldbrush);
		}
		else
		{
			COLORREF region_color = COLOR_RED;
			GetGroupColorForRegion(r_iter->second.name, &region_color);
			CPen group_member_pen;
			group_member_pen.CreatePen(PS_SOLID, 1, region_color);
			// Set pen and brush
			pTempPen = (CPen*)pDC->SelectObject(&group_member_pen);
			oldpen.FromHandle((HPEN)pTempPen);
			pTempBrush = (CBrush*)pDC->SelectObject(GetStockObject(NULL_BRUSH));
			oldbrush.FromHandle((HBRUSH)pTempBrush);

			pDC->Rectangle(r_iter->second.left-1, r_iter->second.top-1, r_iter->second.right+2, r_iter->second.bottom+2);

			pDC->SelectObject(oldpen);
			pDC->SelectObject(oldbrush);
		}

		// Draw the optional second rectangle (orange) whenever it is enabled and the
		// region uses the Color transform, at its own coordinates.
		if (r_iter->second.rect2_enabled
			&& r_iter->second.transform.GetLength() > 0
			&& r_iter->second.transform[0] == 'C')
		{
			CPen pen2;
			pen2.CreatePen(PS_SOLID, 1, RGB(255, 140, 0));   // orange
			pTempPen = (CPen*)pDC->SelectObject(&pen2);
			oldpen.FromHandle((HPEN)pTempPen);
			pTempBrush = (CBrush*)pDC->SelectObject(GetStockObject(NULL_BRUSH));
			oldbrush.FromHandle((HBRUSH)pTempBrush);

			pDC->Rectangle(r_iter->second.left2 - 1, r_iter->second.top2 - 1,
				r_iter->second.right2 + 2, r_iter->second.bottom2 + 2);

			pDC->SelectObject(oldpen);
			pDC->SelectObject(oldbrush);
		}
	}

	DrawRegionGroups(pDC);

	// Red decimal-split markers for configured fields.
	DrawDecimalSplitLines(pDC);

	if (group_box_mode && group_box_started) {
		pTempPen = (CPen*)pDC->SelectObject(yellow_dot_pen);
		oldpen.FromHandle((HPEN)pTempPen);
		pTempBrush = (CBrush*)pDC->SelectObject(GetStockObject(NULL_BRUSH));
		oldbrush.FromHandle((HBRUSH)pTempBrush);
		pDC->Rectangle(min(group_box_start.x, group_box_end.x), min(group_box_start.y, group_box_end.y),
			max(group_box_start.x, group_box_end.x), max(group_box_start.y, group_box_end.y));
		pDC->SelectObject(oldpen);
		pDC->SelectObject(oldbrush);
	}

	// Draw all template rectangles
	for (TPLMapCI tpl_iter = p_tablemap->tpl$()->begin(); tpl_iter != p_tablemap->tpl$()->end(); tpl_iter++)
	{
		if ((tpl_iter->second.name == dragged_region && dragging) ||
			(tpl_iter->second.name == drawrect_region && drawing_rect && drawing_started))
		{
			// Set pen and brush
			pTempPen = (CPen*)pDC->SelectObject(black_dot_pen);
			oldpen.FromHandle((HPEN)pTempPen);
			pTempBrush = (CBrush*)pDC->SelectObject(GetStockObject(NULL_BRUSH));
			oldbrush.FromHandle((HBRUSH)pTempBrush);

			pDC->Rectangle(tpl_iter->second.left - 1, tpl_iter->second.top - 1, tpl_iter->second.right + 2, tpl_iter->second.bottom + 2);

			pDC->SelectObject(oldpen);
			pDC->SelectObject(oldbrush);
		}
		else
		{
			// Set pen and brush
			pTempPen = (CPen*)pDC->SelectObject(red_pen);
			oldpen.FromHandle((HPEN)pTempPen);
			pTempBrush = (CBrush*)pDC->SelectObject(GetStockObject(NULL_BRUSH));
			oldbrush.FromHandle((HBRUSH)pTempBrush);

			pDC->Rectangle(tpl_iter->second.left - 1, tpl_iter->second.top - 1, tpl_iter->second.right + 2, tpl_iter->second.bottom + 2);

			pDC->SelectObject(oldpen);
			pDC->SelectObject(oldbrush);
		}
	}

	// Clean Up
	DrawMagnifierPreview(pDC);
	}	// end if (!_training_mode)

	// Clean Up
	DeleteObject(hbmp);
	DeleteDC(hdcCompatible);
	DeleteDC(hdcScreen);
}


// COpenScrapeView diagnostics

#ifdef _DEBUG
void COpenScrapeView::AssertValid() const
{
	CView::AssertValid();
}

void COpenScrapeView::Dump(CDumpContext& dc) const
{
	CView::Dump(dc);
}

COpenScrapeDoc* COpenScrapeView::GetDocument() const // non-debug version is inline
{
	ASSERT(m_pDocument->IsKindOf(RUNTIME_CLASS(COpenScrapeDoc)));
	return (COpenScrapeDoc*)m_pDocument;
}
#endif //_DEBUG


// COpenScrapeView message handlers

void COpenScrapeView::OnLButtonDown(UINT nFlags, CPoint point)
{
	COpenScrapeDoc		*pDoc = GetDocument();
	CString				sel = theApp.m_TableMapDlg->m_TableMapTree.GetItemText(theApp.m_TableMapDlg->m_TableMapTree.GetSelectedItem());
	CString				text;

	if (_training_mode) {
		_training_dragging = true;
		_training_start = point;
		_training_cur = point;
		SetCapture();
		CView::OnLButtonDown(nFlags, point);
		return;
	}

	if (show_preview && preview_close_rect.PtInRect(point)) {
		SetShowPreview(false);
		CView::OnLButtonDown(nFlags, point);
		return;
	}

	if (color_point_mode) {
		color_point_mode = false;
		if (theApp.m_TableMapDlg != NULL) {
			theApp.m_TableMapDlg->CaptureColorPresetPoint(point);
		}
		CView::OnLButtonDown(nFlags, point);
		return;
	}

	if (color_dropper_mode) {
		color_dropper_mode = false;
		if (theApp.m_TableMapDlg != NULL) {
			theApp.m_TableMapDlg->CaptureColorPresetColor(point);
		}
		CView::OnLButtonDown(nFlags, point);
		return;
	}

	if (group_box_mode) {
		group_box_start = point;
		group_box_end = point;
		group_box_started = true;
		Invalidate(false);
		CView::OnLButtonDown(nFlags, point);
		return;
	}

	if (nFlags & MK_CONTROL) {
		for (RMapCI r_iter = p_tablemap->r$()->begin(); r_iter != p_tablemap->r$()->end(); r_iter++) {
			if (point.x >= (LONG)r_iter->second.left - 1
					&& point.x <= (LONG)r_iter->second.right + 1
					&& point.y >= (LONG)r_iter->second.top - 1
					&& point.y <= (LONG)r_iter->second.bottom + 1) {
				ToggleRegionSelection(r_iter->second.name);
				CView::OnLButtonDown(nFlags, point);
				return;
			}
		}
	}

	// If we are drawing a rectangle for a region, handle that
	if (drawing_rect)
	{
		drawrect_start = point;
		drawing_started = true;
		
		RMapI r_iter = p_tablemap->set_r$()->find(sel.GetString());
		
		if (r_iter != p_tablemap->r$()->end())
		{
			drawrect_region = r_iter->second.name;

			// Update internal structure
			r_iter->second.left = point.x;
			r_iter->second.top = point.y;
			r_iter->second.right = point.x;
			r_iter->second.bottom = point.y;

			// Update table map dialog
			text.Format("%d", point.x);
			theApp.m_TableMapDlg->m_Left.SetWindowText(text.GetString());
			theApp.m_TableMapDlg->m_Right.SetWindowText(text.GetString());
			text.Format("%d", point.y);
			theApp.m_TableMapDlg->m_Top.SetWindowText(text.GetString());
			theApp.m_TableMapDlg->m_Bottom.SetWindowText(text.GetString());

			Invalidate(false);
		}

		TPLMapI tpl_iter = p_tablemap->set_tpl$()->find(sel.GetString());

		if (tpl_iter != p_tablemap->tpl$()->end())
		{
			drawrect_region = tpl_iter->second.name;

			// Update internal structure
			tpl_iter->second.left = point.x;
			tpl_iter->second.top = point.y;
			tpl_iter->second.right = point.x;
			tpl_iter->second.bottom = point.y;

			// Update table map dialog
			text.Format("%d", point.x);
			theApp.m_TableMapDlg->m_Left.SetWindowText(text.GetString());
			theApp.m_TableMapDlg->m_Right.SetWindowText(text.GetString());
			text.Format("%d", point.y);
			theApp.m_TableMapDlg->m_Top.SetWindowText(text.GetString());
			theApp.m_TableMapDlg->m_Bottom.SetWindowText(text.GetString());

			Invalidate(false);
		}
	}

	// Drawing the optional SECOND rectangle (region-only, for the Color transform)
	else if (drawing_rect2)
	{
		drawrect_start = point;
		drawing_started = true;

		RMapI r_iter = p_tablemap->set_r$()->find(sel.GetString());
		if (r_iter != p_tablemap->r$()->end())
		{
			drawrect_region = r_iter->second.name;
			r_iter->second.rect2_enabled = true;
			r_iter->second.left2 = point.x;
			r_iter->second.top2 = point.y;
			r_iter->second.right2 = point.x;
			r_iter->second.bottom2 = point.y;

			text.Format("%d", point.x);
			theApp.m_TableMapDlg->m_Left2.SetWindowText(text.GetString());
			theApp.m_TableMapDlg->m_Right2.SetWindowText(text.GetString());
			text.Format("%d", point.y);
			theApp.m_TableMapDlg->m_Top2.SetWindowText(text.GetString());
			theApp.m_TableMapDlg->m_Bottom2.SetWindowText(text.GetString());

			Invalidate(false);
		}
	}

	// Otherwise...
	else
	{
		// Shift click means we want to drag the region
		RMapCI r_iter = p_tablemap->r$()->find(sel.GetString());
		TPLMapCI tpl_iter = p_tablemap->tpl$()->find(sel.GetString());
		if (nFlags & MK_SHIFT)
		{			
			if (r_iter != p_tablemap->r$()->end())
			{
				if (point.x >= (LONG) r_iter->second.left-1 &&
					point.x <= (LONG) r_iter->second.right+1 &&
					point.y >= (LONG) r_iter->second.top-1 &&
					point.y <= (LONG) r_iter->second.bottom+1)
				{
					dragging = true;
					dragged_region = r_iter->second.name;
					dragged_group = "";
					drag_move_before = CaptureMoveStateForRegion(dragged_region);
					drag_left_offset = point.x - r_iter->second.left;
					drag_top_offset = point.y - r_iter->second.top;
					Invalidate(false);
				}
			}

			if (tpl_iter != p_tablemap->tpl$()->end())
			{
				if (point.x >= (LONG)tpl_iter->second.left - 1 &&
					point.x <= (LONG)tpl_iter->second.right + 1 &&
					point.y >= (LONG)tpl_iter->second.top - 1 &&
					point.y <= (LONG)tpl_iter->second.bottom + 1)
				{
					dragging = true;
					dragged_region = tpl_iter->second.name;
					dragged_group = "";
					drag_move_before.clear();
					drag_left_offset = point.x - tpl_iter->second.left;
					drag_top_offset = point.y - tpl_iter->second.top;
					Invalidate(false);
				}
			}
		}

		// No shift means just select the region in the tree
		else
		{
			CString region_name = RegionNameAtPoint(point);
			if (!region_name.IsEmpty())
			{
				RMapCI clicked_region = p_tablemap->r$()->find(region_name.GetString());
				if (clicked_region != p_tablemap->r$()->end())
				{
					HTREEITEM parent_node = theApp.m_TableMapDlg->GetTypeNode("Regions");
					HTREEITEM item = theApp.m_TableMapDlg->FindItem(region_name, parent_node);
					if (item) {
						theApp.m_TableMapDlg->m_TableMapTree.SelectItem(item);
					}

					dragged_region = clicked_region->second.name;
					dragged_group = GroupNameForRegion(dragged_region);
					// A locked group (and its member regions) can't be moved.
					if (!dragged_group.IsEmpty() && IsGroupLocked(dragged_group)) {
						dragged_region = "";
						dragged_group = "";
						Invalidate(false);
						CView::OnLButtonDown(nFlags, point);
						return;
					}
					dragging = true;
					drag_move_before = dragged_group.IsEmpty()
						? CaptureMoveStateForRegion(dragged_region)
						: CaptureMoveStateForGroup(dragged_group);
					drag_left_offset = point.x - clicked_region->second.left;
					drag_top_offset = point.y - clicked_region->second.top;
					Invalidate(false);
					CView::OnLButtonDown(nFlags, point);
					return;
				}
			}

			CString group_name = GroupNameAtPoint(point);
			if (!group_name.IsEmpty())
			{
				HTREEITEM parent_node = theApp.m_TableMapDlg->GetTypeNode("Groups");
				HTREEITEM item = parent_node ? theApp.m_TableMapDlg->FindItem(group_name, parent_node) : NULL;
				if (item) {
					theApp.m_TableMapDlg->m_TableMapTree.SelectItem(item);
				}

				// A locked group can't be moved (selection above still happened).
				if (IsGroupLocked(group_name)) {
					CView::OnLButtonDown(nFlags, point);
					return;
				}
				dragging = true;
				dragged_region = "";
				dragged_group = group_name;
				drag_move_before = CaptureMoveStateForGroup(dragged_group);
				drag_left_offset = point.x;
				drag_top_offset = point.y;
				Invalidate(false);
				CView::OnLButtonDown(nFlags, point);
				return;
			}

			for (tpl_iter = p_tablemap->tpl$()->begin(); tpl_iter != p_tablemap->tpl$()->end(); tpl_iter++)
			{
				if (point.x >= (LONG)tpl_iter->second.left - 1 &&
					point.x <= (LONG)tpl_iter->second.right + 1 &&
					point.y >= (LONG)tpl_iter->second.top - 1 &&
					point.y <= (LONG)tpl_iter->second.bottom + 1 &&
					tpl_iter->second.name != sel)
				{

					// Find parent node
					HTREEITEM parent_node = theApp.m_TableMapDlg->GetTypeNode("Templates");

					// Find correct leaf node
					HTREEITEM item = theApp.m_TableMapDlg->FindItem(tpl_iter->second.name, parent_node);

					if (item)
						theApp.m_TableMapDlg->m_TableMapTree.SelectItem(item);
				}
			}
		}
	}

	CView::OnLButtonDown(nFlags, point);
}

void COpenScrapeView::OnLButtonUp(UINT nFlags, CPoint point)
{
	COpenScrapeDoc		*pDoc = GetDocument();

	if (_training_mode) {
		if (_training_dragging) {
			_training_dragging = false;
			// Erase the last rubber-band rectangle.
			{
				CClientDC dc(this);
				int old_rop = dc.SetROP2(R2_NOT);
				CGdiObject *old_brush = dc.SelectStockObject(NULL_BRUSH);
				dc.Rectangle(CRect(_training_start, _training_cur));
				dc.SelectObject(old_brush);
				dc.SetROP2(old_rop);
			}
			ReleaseCapture();

			CRect rect(_training_start, point);
			rect.NormalizeRect();
			HandleTrainingCapture(rect);
		}
		CView::OnLButtonUp(nFlags, point);
		return;
	}

	if (group_box_mode && group_box_started)
	{
		group_box_started = false;
		RECT group_rect;
		group_rect.left = min(group_box_start.x, point.x);
		group_rect.top = min(group_box_start.y, point.y);
		group_rect.right = max(group_box_start.x, point.x);
		group_rect.bottom = max(group_box_start.y, point.y);
		SelectRegionsInsideRect(group_rect);
		group_box_mode = false;
		if (theApp.m_TableMapDlg != NULL) {
			theApp.m_TableMapDlg->GetDlgItem(IDC_GROUP_BOX)->SetWindowText("Box");
		}
		if (selected_regions.size() >= 2
				&& AfxMessageBox("Do you want to group these items?", MB_YESNO) == IDYES) {
			CreateGroupFromSelection();
		}
		Invalidate(false);
	}

	else if (drawing_rect)
	{
		drawing_rect = false;
		drawing_started = false;

		RMapI r_iter=p_tablemap->set_r$()->find(drawrect_region.GetString());

		if (r_iter != p_tablemap->r$()->end())
		{
			r_iter->second.left = drawrect_start.x<point.x ? drawrect_start.x : point.x;
			r_iter->second.top = drawrect_start.y<point.y ? drawrect_start.y : point.y;
			r_iter->second.right = drawrect_start.x>=point.x ? drawrect_start.x : point.x;
			r_iter->second.bottom = drawrect_start.y>=point.y ? drawrect_start.y : point.y;

			theApp.m_TableMapDlg->m_DrawRect.OnBnClicked();
			Invalidate(false);
			theApp.m_TableMapDlg->Invalidate(false);
		}

		TPLMapI tpl_iter = p_tablemap->set_tpl$()->find(drawrect_region.GetString());

		if (tpl_iter != p_tablemap->tpl$()->end())
		{
			tpl_iter->second.left = drawrect_start.x<point.x ? drawrect_start.x : point.x;
			tpl_iter->second.top = drawrect_start.y<point.y ? drawrect_start.y : point.y;
			tpl_iter->second.right = drawrect_start.x >= point.x ? drawrect_start.x : point.x;
			tpl_iter->second.bottom = drawrect_start.y >= point.y ? drawrect_start.y : point.y;

			theApp.m_TableMapDlg->m_DrawRect.OnBnClicked();
			Invalidate(false);
			theApp.m_TableMapDlg->Invalidate(false);
		}
	}

	else if (drawing_rect2)
	{
		drawing_rect2 = false;
		drawing_started = false;

		RMapI r_iter = p_tablemap->set_r$()->find(drawrect_region.GetString());
		if (r_iter != p_tablemap->r$()->end())
		{
			r_iter->second.left2 = drawrect_start.x<point.x ? drawrect_start.x : point.x;
			r_iter->second.top2 = drawrect_start.y<point.y ? drawrect_start.y : point.y;
			r_iter->second.right2 = drawrect_start.x>=point.x ? drawrect_start.x : point.x;
			r_iter->second.bottom2 = drawrect_start.y>=point.y ? drawrect_start.y : point.y;

			theApp.m_TableMapDlg->m_DrawRect2.OnBnClicked();
			Invalidate(false);
			theApp.m_TableMapDlg->Invalidate(false);
		}
	}


	else if (dragging)
	{
		if (!drag_move_before.empty()) {
			std::vector<CString> names;
			for (size_t i = 0; i < drag_move_before.size(); ++i) {
				names.push_back(drag_move_before[i].name);
			}
			std::vector<SOpenScrapeRegionMoveState> after = CaptureRegionMoveState(names);
			RecordRegionMove(drag_move_before, after);
			drag_move_before.clear();
		}
		dragging = false;
		dragged_region = "";
		dragged_group = "";
		Invalidate(false);
		theApp.m_TableMapDlg->Invalidate(false);
	}


	CView::OnLButtonUp(nFlags, point);
}

void COpenScrapeView::OnRButtonDown(UINT nFlags, CPoint point)
{
	CString region_name = RegionNameAtPoint(point);
	if (region_name.IsEmpty()) {
		CString empty_group_name = GroupNameAtPoint(point);
		if (!empty_group_name.IsEmpty()) {
			CMenu menu;
			menu.CreatePopupMenu();
			const UINT kDeleteAllInGroupMenuId = 51003;
			const UINT kDeleteGroupOnlyMenuId = 51004;
			const UINT kToggleLockGroupMenuId = 51005;
			bool group_locked = IsGroupLocked(empty_group_name);
			menu.AppendMenu(MF_STRING, kToggleLockGroupMenuId, group_locked ? "Unlock group" : "Lock group");
			menu.AppendMenu(MF_STRING, kDeleteAllInGroupMenuId, "Delete all regions in group");
			menu.AppendMenu(MF_STRING, kDeleteGroupOnlyMenuId, "Delete group only (keep regions)");

			CPoint screen_point = point;
			ClientToScreen(&screen_point);
			UINT command = menu.TrackPopupMenu(TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
				screen_point.x, screen_point.y, this);

			if (command == kToggleLockGroupMenuId) {
				SetGroupLocked(empty_group_name, !group_locked);
				theApp.m_TableMapDlg->update_tree("Groups");
			}
			else if (command == kDeleteAllInGroupMenuId) {
				CString message;
				message.Format("Delete all regions in group '%s'?\n\nThis also removes the group.", empty_group_name);
				if (AfxMessageBox(message, MB_YESNO) == IDYES && DeleteAllRegionsInGroup(empty_group_name)) {
					theApp.m_TableMapDlg->update_tree("Regions");
					theApp.m_TableMapDlg->update_tree("Groups");
				}
			}
			else if (command == kDeleteGroupOnlyMenuId) {
				CString message;
				message.Format("Delete group '%s' only?\n\nThe regions in this group will be kept.", empty_group_name);
				if (AfxMessageBox(message, MB_YESNO) == IDYES && DeleteRegionGroup(empty_group_name)) {
					theApp.m_TableMapDlg->update_tree("Groups");
				}
			}
			CView::OnRButtonDown(nFlags, point);
			return;
		}
		CView::OnRButtonDown(nFlags, point);
		return;
	}

	CString group_name = GroupNameForRegion(region_name);
	if (group_name.IsEmpty()) {
		HTREEITEM parent_node = theApp.m_TableMapDlg->GetTypeNode("Regions");
		HTREEITEM item = theApp.m_TableMapDlg->FindItem(region_name, parent_node);
		if (item != NULL) {
			theApp.m_TableMapDlg->m_TableMapTree.SelectItem(item);
		}

		CMenu menu;
		menu.CreatePopupMenu();
		const UINT kDeleteRegionMenuId = 51001;
		menu.AppendMenu(MF_STRING, kDeleteRegionMenuId, "Delete");

		CPoint screen_point = point;
		ClientToScreen(&screen_point);
		UINT command = menu.TrackPopupMenu(TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
			screen_point.x, screen_point.y, this);

		if (command == kDeleteRegionMenuId) {
			CString message;
			message.Format("Delete region: %s?", region_name);
			if (AfxMessageBox(message, MB_YESNO) == IDYES && p_tablemap->r$_erase(region_name)) {
				PurgeMissingRegionsFromGroups();
				theApp.m_TableMapDlg->update_tree("Regions");
				Invalidate(false);
				GetDocument()->SetModifiedFlag(true);
			}
		}
		CView::OnRButtonDown(nFlags, point);
		return;
	}

	HTREEITEM parent_node = theApp.m_TableMapDlg->GetTypeNode("Regions");
	HTREEITEM item = theApp.m_TableMapDlg->FindItem(region_name, parent_node);
	if (item != NULL) {
		theApp.m_TableMapDlg->m_TableMapTree.SelectItem(item);
	}

	CMenu menu;
	menu.CreatePopupMenu();
	const UINT kDeleteRegionMenuId = 51001;
	const UINT kUngroupRegionMenuId = 51002;
	const UINT kToggleLockGroupMenuId = 51005;
	bool group_locked = IsGroupLocked(group_name);
	menu.AppendMenu(MF_STRING, kToggleLockGroupMenuId, group_locked ? "Unlock group" : "Lock group");
	menu.AppendMenu(MF_STRING, kDeleteRegionMenuId, "Delete");
	menu.AppendMenu(MF_STRING, kUngroupRegionMenuId, "Ungroup this item");

	CPoint screen_point = point;
	ClientToScreen(&screen_point);
	UINT command = menu.TrackPopupMenu(TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
		screen_point.x, screen_point.y, this);

	if (command == kToggleLockGroupMenuId) {
		SetGroupLocked(group_name, !group_locked);
		theApp.m_TableMapDlg->update_tree("Groups");
	}
	else if (command == kDeleteRegionMenuId) {
		CString message;
		message.Format("Delete region: %s?", region_name);
		if (AfxMessageBox(message, MB_YESNO) == IDYES && p_tablemap->r$_erase(region_name)) {
			PurgeMissingRegionsFromGroups();
			theApp.m_TableMapDlg->update_tree("Regions");
			Invalidate(false);
			GetDocument()->SetModifiedFlag(true);
		}
	}
	else if (command == kUngroupRegionMenuId) {
		RemoveRegionFromGroup(region_name);
		theApp.m_TableMapDlg->update_tree("Groups");
	}

	CView::OnRButtonDown(nFlags, point);
}

void COpenScrapeView::OnEditUndo()
{
	if (undo_region_moves.empty()) {
		return;
	}

	SOpenScrapeRegionMoveAction action = undo_region_moves.back();
	undo_region_moves.pop_back();
	ApplyRegionMoveState(action.before);
	redo_region_moves.push_back(action);
}

void COpenScrapeView::OnEditRedo()
{
	if (redo_region_moves.empty()) {
		return;
	}

	SOpenScrapeRegionMoveAction action = redo_region_moves.back();
	redo_region_moves.pop_back();
	ApplyRegionMoveState(action.after);
	undo_region_moves.push_back(action);
}

void COpenScrapeView::OnUpdateEditUndo(CCmdUI *pCmdUI)
{
	pCmdUI->Enable(!undo_region_moves.empty());
}

void COpenScrapeView::OnUpdateEditRedo(CCmdUI *pCmdUI)
{
	pCmdUI->Enable(!redo_region_moves.empty());
}

void COpenScrapeView::SetShowPreview(bool show)
{
	show_preview = show;
	if (!show_preview) {
		preview_close_rect.SetRectEmpty();
	}
	Invalidate(false);
}

void COpenScrapeView::OnViewShowPreview()
{
	SetShowPreview(!show_preview);
}

void COpenScrapeView::OnUpdateViewShowPreview(CCmdUI *pCmdUI)
{
	pCmdUI->SetCheck(show_preview);
	pCmdUI->SetText(show_preview ? "Hide Preview" : "Show Preview");
}

void COpenScrapeView::OnMouseMove(UINT nFlags, CPoint point)
{
	COpenScrapeDoc		*pDoc = GetDocument();
	int					width, height;
	CString				text;

	if (_training_mode) {
		if (_training_dragging) {
			// XOR rubber-band: erase the previous rectangle, draw the new one.
			CClientDC dc(this);
			int old_rop = dc.SetROP2(R2_NOT);
			CGdiObject *old_brush = dc.SelectStockObject(NULL_BRUSH);
			dc.Rectangle(CRect(_training_start, _training_cur));
			_training_cur = point;
			dc.Rectangle(CRect(_training_start, _training_cur));
			dc.SelectObject(old_brush);
			dc.SetROP2(old_rop);
		}
		CView::OnMouseMove(nFlags, point);
		return;
	}

	if (show_preview && pDoc->attached_bitmap != NULL && preview_point != point) {
		preview_point = point;
		Invalidate(false);
	}

	if (group_box_mode && group_box_started)
	{
		group_box_end = point;
		Invalidate(false);
	}

	else if (drawing_rect && drawing_started)
	{
		RMapI r_iter=p_tablemap->set_r$()->find(drawrect_region.GetString());

		if (r_iter != p_tablemap->r$()->end())
		{
			// Update internal structure for selected region
			r_iter->second.left = drawrect_start.x<point.x ? drawrect_start.x : point.x;
			r_iter->second.top = drawrect_start.y<point.y ? drawrect_start.y : point.y;
			r_iter->second.right = drawrect_start.x>=point.x ? drawrect_start.x : point.x;
			r_iter->second.bottom = drawrect_start.y>=point.y ? drawrect_start.y : point.y;

			// Update table map dialog
			text.Format("%d", r_iter->second.left);
			theApp.m_TableMapDlg->m_Left.SetWindowText(text.GetString());
			text.Format("%d", r_iter->second.top);
			theApp.m_TableMapDlg->m_Top.SetWindowText(text.GetString());
			text.Format("%d", r_iter->second.right);
			theApp.m_TableMapDlg->m_Right.SetWindowText(text.GetString());
			text.Format("%d", r_iter->second.bottom);
			theApp.m_TableMapDlg->m_Bottom.SetWindowText(text.GetString());

			theApp.m_TableMapDlg->update_display();
			theApp.m_TableMapDlg->Invalidate(false);
			Invalidate(false);
			pDoc->SetModifiedFlag(true);
		}

		TPLMapI tpl_iter = p_tablemap->set_tpl$()->find(drawrect_region.GetString());

		if (tpl_iter != p_tablemap->tpl$()->end())
		{
			// Update internal structure for selected region
			tpl_iter->second.left = drawrect_start.x<point.x ? drawrect_start.x : point.x;
			tpl_iter->second.top = drawrect_start.y<point.y ? drawrect_start.y : point.y;
			tpl_iter->second.right = drawrect_start.x >= point.x ? drawrect_start.x : point.x;
			tpl_iter->second.bottom = drawrect_start.y >= point.y ? drawrect_start.y : point.y;

			// Update table map dialog
			text.Format("%d", tpl_iter->second.left);
			theApp.m_TableMapDlg->m_Left.SetWindowText(text.GetString());
			text.Format("%d", tpl_iter->second.top);
			theApp.m_TableMapDlg->m_Top.SetWindowText(text.GetString());
			text.Format("%d", tpl_iter->second.right);
			theApp.m_TableMapDlg->m_Right.SetWindowText(text.GetString());
			text.Format("%d", tpl_iter->second.bottom);
			theApp.m_TableMapDlg->m_Bottom.SetWindowText(text.GetString());

			theApp.m_TableMapDlg->update_display();
			theApp.m_TableMapDlg->Invalidate(false);
			Invalidate(false);
			pDoc->SetModifiedFlag(true);
		}
	}

	else if (drawing_rect2 && drawing_started)
	{
		RMapI r_iter = p_tablemap->set_r$()->find(drawrect_region.GetString());
		if (r_iter != p_tablemap->r$()->end())
		{
			r_iter->second.left2 = drawrect_start.x<point.x ? drawrect_start.x : point.x;
			r_iter->second.top2 = drawrect_start.y<point.y ? drawrect_start.y : point.y;
			r_iter->second.right2 = drawrect_start.x>=point.x ? drawrect_start.x : point.x;
			r_iter->second.bottom2 = drawrect_start.y>=point.y ? drawrect_start.y : point.y;

			text.Format("%d", r_iter->second.left2);
			theApp.m_TableMapDlg->m_Left2.SetWindowText(text.GetString());
			text.Format("%d", r_iter->second.top2);
			theApp.m_TableMapDlg->m_Top2.SetWindowText(text.GetString());
			text.Format("%d", r_iter->second.right2);
			theApp.m_TableMapDlg->m_Right2.SetWindowText(text.GetString());
			text.Format("%d", r_iter->second.bottom2);
			theApp.m_TableMapDlg->m_Bottom2.SetWindowText(text.GetString());

			theApp.m_TableMapDlg->update_display();
			theApp.m_TableMapDlg->Invalidate(false);
			Invalidate(false);
			pDoc->SetModifiedFlag(true);
		}
	}

	else if (dragging)
	{
		if (!dragged_group.IsEmpty() && dragged_region.IsEmpty())
		{
			int dx = point.x - drag_left_offset;
			int dy = point.y - drag_top_offset;
			if (dx != 0 || dy != 0) {
				MoveGroupBy(dragged_group, dx, dy, false);
				drag_left_offset = point.x;
				drag_top_offset = point.y;
				theApp.m_TableMapDlg->Invalidate(false);
				Invalidate(false);
				pDoc->SetModifiedFlag(true);
			}
			CView::OnMouseMove(nFlags, point);
			return;
		}

		RMapI r_iter=p_tablemap->set_r$()->find(dragged_region.GetString());

		if (r_iter != p_tablemap->r$()->end())
		{
			width = r_iter->second.right - r_iter->second.left;
			height = r_iter->second.bottom - r_iter->second.top;
			int new_left = point.x - drag_left_offset;
			int new_top = point.y - drag_top_offset;
			int dx = new_left - r_iter->second.left;
			int dy = new_top - r_iter->second.top;

			// Update internal structure for selected region
			if (dragged_group != "") {
				MoveRegionWithGroup(r_iter->second.name, dx, dy, false);
			} else {
				r_iter->second.left = new_left;
				r_iter->second.top = new_top;
				r_iter->second.right = r_iter->second.left + width;
				r_iter->second.bottom = r_iter->second.top + height;
				RebuildGroupBounds();
			}

			// Update table map dialog
			text.Format("%d", r_iter->second.left);
			theApp.m_TableMapDlg->m_Left.SetWindowText(text.GetString());
			text.Format("%d", r_iter->second.top);
			theApp.m_TableMapDlg->m_Top.SetWindowText(text.GetString());
			text.Format("%d", r_iter->second.right);
			theApp.m_TableMapDlg->m_Right.SetWindowText(text.GetString());
			text.Format("%d", r_iter->second.bottom);
			theApp.m_TableMapDlg->m_Bottom.SetWindowText(text.GetString());

			theApp.m_TableMapDlg->update_display();
			theApp.m_TableMapDlg->Invalidate(false);
			Invalidate(false);
			pDoc->SetModifiedFlag(true);
		}

		TPLMapI tpl_iter = p_tablemap->set_tpl$()->find(dragged_region.GetString());

		if (tpl_iter != p_tablemap->tpl$()->end())
		{
			width = tpl_iter->second.right - tpl_iter->second.left;
			height = tpl_iter->second.bottom - tpl_iter->second.top;

			// Update internal structure for selected region
			tpl_iter->second.left = point.x - drag_left_offset;
			tpl_iter->second.top = point.y - drag_top_offset;
			tpl_iter->second.right = tpl_iter->second.left + width;
			tpl_iter->second.bottom = tpl_iter->second.top + height;

			// Update table map dialog
			text.Format("%d", tpl_iter->second.left);
			theApp.m_TableMapDlg->m_Left.SetWindowText(text.GetString());
			text.Format("%d", tpl_iter->second.top);
			theApp.m_TableMapDlg->m_Top.SetWindowText(text.GetString());
			text.Format("%d", tpl_iter->second.right);
			theApp.m_TableMapDlg->m_Right.SetWindowText(text.GetString());
			text.Format("%d", tpl_iter->second.bottom);
			theApp.m_TableMapDlg->m_Bottom.SetWindowText(text.GetString());

			theApp.m_TableMapDlg->update_display();
			theApp.m_TableMapDlg->Invalidate(false);
			Invalidate(false);
			pDoc->SetModifiedFlag(true);
		}
	}

	CView::OnMouseMove(nFlags, point);
}

void OnKeyUp(UINT nChar, UINT nRepCnt, UINT nFlags) 
{
	
}


/**
 * handles Keyboard presses, currently used for moving/resizing regions.
 * \param nChar the virtual key pressed down
 */
void COpenScrapeView::OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags)
{
	if(nChar==VK_UP || nChar==VK_DOWN || nChar==VK_LEFT || nChar==VK_RIGHT
		|| nChar==VK_NUMPAD1 || nChar==VK_NUMPAD3 || nChar==VK_NUMPAD7 || nChar==VK_NUMPAD9) {
		// find the currently selected
		CString sel = theApp.m_TableMapDlg->m_TableMapTree.GetItemText(theApp.m_TableMapDlg->m_TableMapTree.GetSelectedItem());	
		RMapI r_iter = p_tablemap->set_r$()->find(sel.GetString());

		if (r_iter != p_tablemap->r$()->end())
		{
			// A region in a locked group can't be moved or resized from the keyboard.
			if (IsRegionInLockedGroup(r_iter->second.name)) {
				CView::OnKeyDown(nChar, nRepCnt, nFlags);
				return;
			}
			// check what key combinations are down
			bool shiftKeyDown = GetKeyState(VK_SHIFT)>>7;
			bool sizeChangeKeyDown = GetKeyState(VK_CONTROL)>>7;
			bool moveSingleGroupedRegion = shiftKeyDown && !GroupNameForRegion(r_iter->second.name).IsEmpty();

			int speed = 1;
			if(shiftKeyDown && !moveSingleGroupedRegion) {
				speed = 5;
			} 
			
			if(sizeChangeKeyDown) {
				// change size of region
				if(nChar == VK_UP)	{
					r_iter->second.top = r_iter->second.top-speed;
				} else if(nChar == VK_DOWN)	{
					r_iter->second.top = r_iter->second.top+speed;
				} else if(nChar == VK_LEFT)	{
					r_iter->second.right = r_iter->second.right-speed;
				} else if(nChar == VK_RIGHT)	{
					r_iter->second.right = r_iter->second.right+speed;
				} 
			} else {

				// Default is just to move the selected region
				int dx = 0;
				int dy = 0;
				if(nChar == VK_UP)	{
					dy = -speed;
				} else if(nChar == VK_DOWN)	{
					dy = speed;
				} else if(nChar == VK_LEFT)	{
					dx = -speed;
				} else if(nChar == VK_RIGHT)	{
					dx = speed;
				} else if(nChar == VK_NUMPAD1)	{
					dx = -speed;
					dy = speed;
				} else if(nChar == VK_NUMPAD3)	{
					dx = speed;
					dy = speed;
				} else if(nChar == VK_NUMPAD7)	{
					dx = -speed;
					dy = -speed;
				} else if(nChar == VK_NUMPAD9)	{
					dx = speed;
					dy = -speed;
				}
				if (dx != 0 || dy != 0) {
					if (moveSingleGroupedRegion) {
						MoveRegionBy(r_iter->second.name, dx, dy);
					} else {
						MoveRegionWithGroup(r_iter->second.name, dx, dy);
					}
				}
			}

			theApp.m_TableMapDlg->update_display();
			theApp.m_TableMapDlg->Invalidate(false);
			Invalidate(false);
			GetDocument()->SetModifiedFlag(true);
		}

		TPLMapI tpl_iter = p_tablemap->set_tpl$()->find(sel.GetString());

		if (tpl_iter != p_tablemap->tpl$()->end())
		{
			// check what key combinations are down
			bool shiftKeyDown = GetKeyState(VK_SHIFT) >> 7;
			bool sizeChangeKeyDown = GetKeyState(VK_CONTROL) >> 7;

			int speed = 1;
			if (shiftKeyDown) {
				speed = 5;
			}

			if (sizeChangeKeyDown) {
				// change size of region
				if (nChar == VK_UP) {
					tpl_iter->second.top = tpl_iter->second.top - speed;
				}
				else if (nChar == VK_DOWN) {
					tpl_iter->second.top = tpl_iter->second.top + speed;
				}
				else if (nChar == VK_LEFT) {
					tpl_iter->second.right = tpl_iter->second.right - speed;
				}
				else if (nChar == VK_RIGHT) {
					tpl_iter->second.right = tpl_iter->second.right + speed;
				}
			}
			else {

				// Default is just to move the selected region
				if (nChar == VK_UP) {
					tpl_iter->second.top = tpl_iter->second.top - speed;
					tpl_iter->second.bottom = tpl_iter->second.bottom - speed;
				}
				else if (nChar == VK_DOWN) {
					tpl_iter->second.top = tpl_iter->second.top + speed;
					tpl_iter->second.bottom = tpl_iter->second.bottom + speed;
				}
				else if (nChar == VK_LEFT) {
					tpl_iter->second.left = tpl_iter->second.left - speed;
					tpl_iter->second.right = tpl_iter->second.right - speed;
				}
				else if (nChar == VK_RIGHT) {
					tpl_iter->second.left = tpl_iter->second.left + speed;
					tpl_iter->second.right = tpl_iter->second.right + speed;
				}
				else if (nChar == VK_NUMPAD1) {
					tpl_iter->second.top = tpl_iter->second.top + speed;
					tpl_iter->second.bottom = tpl_iter->second.bottom + speed;
					tpl_iter->second.left = tpl_iter->second.left - speed;
					tpl_iter->second.right = tpl_iter->second.right - speed;
				}
				else if (nChar == VK_NUMPAD3) {
					tpl_iter->second.top = tpl_iter->second.top + speed;
					tpl_iter->second.bottom = tpl_iter->second.bottom + speed;
					tpl_iter->second.left = tpl_iter->second.left + speed;
					tpl_iter->second.right = tpl_iter->second.right + speed;
				}
				else if (nChar == VK_NUMPAD7) {
					tpl_iter->second.top = tpl_iter->second.top - speed;
					tpl_iter->second.bottom = tpl_iter->second.bottom - speed;
					tpl_iter->second.left = tpl_iter->second.left - speed;
					tpl_iter->second.right = tpl_iter->second.right - speed;
				}
				else if (nChar == VK_NUMPAD9) {
					tpl_iter->second.top = tpl_iter->second.top - speed;
					tpl_iter->second.bottom = tpl_iter->second.bottom - speed;
					tpl_iter->second.left = tpl_iter->second.left + speed;
					tpl_iter->second.right = tpl_iter->second.right + speed;
				}
			}

			theApp.m_TableMapDlg->update_display();
			theApp.m_TableMapDlg->Invalidate(false);
			Invalidate(false);
			GetDocument()->SetModifiedFlag(true);
		}
	}

	CView::OnKeyDown(nChar, nRepCnt, nFlags);
}


BOOL COpenScrapeView::OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message)
{
	if (_training_mode)
	{
		::SetCursor(::LoadCursor(NULL, IDC_CROSS));
		return true;
	}

	if (drawing_rect || drawing_rect2)
	{
		::SetCursor(hCurDrawRect);
		return true;
	}

	return CView::OnSetCursor(pWnd, nHitTest, message);
}

// ---------------------------------------------------------------------------
//  Tesseract training-data capture
// ---------------------------------------------------------------------------

void COpenScrapeView::SetTrainingMode(bool on)
{
	if (_training_mode == on) {
		return;
	}
	_training_mode = on;
	_training_dragging = false;
	Invalidate(false);		// hide overlays when turning on, restore when off
}

// "training\" sub-folder next to Vision.exe, created if needed.
static CString TrainingDirectory()
{
	char path[MAX_PATH] = { 0 };
	::GetModuleFileName(NULL, path, MAX_PATH);
	char *last_backslash = strrchr(path, '\\');
	if (last_backslash != NULL) {
		*(last_backslash + 1) = '\0';
	}
	CString dir = CString(path) + "training\\";
	CreateDirectory(dir, NULL);
	return dir;
}

// Lowest free "sample_NNNN" base name (neither .png nor .gt.txt present).
static CString NextTrainingBaseName(const CString &dir)
{
	for (int i = 1; i < 100000; ++i) {
		CString base;
		base.Format("sample_%04d", i);
		CString png = dir + base + ".png";
		CString txt = dir + base + ".gt.txt";
		if (GetFileAttributes(png) == INVALID_FILE_ATTRIBUTES
			&& GetFileAttributes(txt) == INVALID_FILE_ATTRIBUTES) {
			return base;
		}
	}
	return "sample_overflow";
}

void COpenScrapeView::HandleTrainingCapture(CRect rect)
{
	COpenScrapeDoc *pDoc = GetDocument();
	if (pDoc == NULL || pDoc->attached_bitmap == NULL) {
		SetTrainingMode(false);
		return;
	}

	// Clamp the drawn rectangle to the screenshot bounds.
	int img_w = pDoc->attached_rect.right - pDoc->attached_rect.left;
	int img_h = pDoc->attached_rect.bottom - pDoc->attached_rect.top;
	if (rect.left < 0) rect.left = 0;
	if (rect.top < 0) rect.top = 0;
	if (rect.right > img_w) rect.right = img_w;
	if (rect.bottom > img_h) rect.bottom = img_h;
	if (rect.Width() < 2 || rect.Height() < 2) {
		SetTrainingMode(false);		// stray click, nothing to capture
		return;
	}

	int w = rect.Width();
	int h = rect.Height();

	// Crop the screenshot region into a top-down 32-bit Mat, then drop alpha.
	HDC hdcScreen = CreateDC("DISPLAY", NULL, NULL, NULL);
	HDC hdc_src = CreateCompatibleDC(hdcScreen);
	HBITMAP old_src = (HBITMAP)SelectObject(hdc_src, pDoc->attached_bitmap);
	HDC hdc_crop = CreateCompatibleDC(hdcScreen);
	HBITMAP crop_bmp = CreateCompatibleBitmap(hdcScreen, w, h);
	HBITMAP old_crop = (HBITMAP)SelectObject(hdc_crop, crop_bmp);
	BitBlt(hdc_crop, 0, 0, w, h, hdc_src, rect.left, rect.top, SRCCOPY);

	cv::Mat crop_bgra(h, w, CV_8UC4);
	BITMAPINFO bi;
	ZeroMemory(&bi, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;		// top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	GetDIBits(hdc_crop, crop_bmp, 0, h, crop_bgra.data, &bi, DIB_RGB_COLORS);

	SelectObject(hdc_crop, old_crop);
	DeleteObject(crop_bmp);
	DeleteDC(hdc_crop);
	SelectObject(hdc_src, old_src);
	DeleteDC(hdc_src);
	DeleteDC(hdcScreen);

	cv::Mat crop_bgr;
	cv::cvtColor(crop_bgra, crop_bgr, cv::COLOR_BGRA2BGR);

	// Ask the user for the ground-truth value for this crop.
	CDlgEdit dlg;
	dlg.m_titletext = "Enter the text shown in this region (training label)";
	dlg.m_result = "";
	if (dlg.DoModal() != IDOK) {
		SetTrainingMode(false);
		return;
	}
	dlg.m_result.Trim();
	if (dlg.m_result.IsEmpty()) {
		SetTrainingMode(false);
		return;
	}

	CString dir = TrainingDirectory();
	CString base = NextTrainingBaseName(dir);
	CString png_path = dir + base + ".png";
	CString txt_path = dir + base + ".gt.txt";

	bool image_ok = false;
	try {
		image_ok = cv::imwrite(std::string(png_path.GetString()), crop_bgr);
	}
	catch (const cv::Exception &) {
		image_ok = false;
	}

	bool text_ok = false;
	FILE *fp = NULL;
	if (fopen_s(&fp, txt_path.GetString(), "w") == 0 && fp != NULL) {
		fputs(dlg.m_result.GetString(), fp);
		fputs("\n", fp);
		fclose(fp);
		text_ok = true;
	}

	if (image_ok && text_ok) {
		CString msg;
		msg.Format("Saved training sample:\n%s\n%s", png_path.GetString(), txt_path.GetString());
		AfxMessageBox(msg, MB_ICONINFORMATION);
	}
	else {
		AfxMessageBox("Failed to save training sample (is the training\\ folder writable?).", MB_ICONWARNING);
	}

	SetTrainingMode(false);		// restores the region overlays
}

void COpenScrapeView::blink_rect(void)
{
	COpenScrapeDoc *pDoc = COpenScrapeDoc::GetDocument();
	CString				 sel = theApp.m_TableMapDlg->m_TableMapTree.GetItemText(theApp.m_TableMapDlg->m_TableMapTree.GetSelectedItem());	
	CDC					   *pDC = GetDC();
	CPen				   *pTempPen, oldpen;
	CBrush				 *pTempBrush, oldbrush;	

  if (dragging) {
    return;
  }
  // Set pen and brush
	pTempPen = (CPen*)pDC->SelectObject(pDoc->blink_on ? green_pen : red_pen);
	oldpen.FromHandle((HPEN)pTempPen);					// Save old pen
	pTempBrush = (CBrush*)pDC->SelectObject(GetStockObject(NULL_BRUSH));
	oldbrush.FromHandle((HBRUSH)pTempBrush);			// Save old brush

	RMapCI r_iter=p_tablemap->r$()->find(sel.GetString());
  if (r_iter != p_tablemap->r$()->end()) {
    pDC->Rectangle(r_iter->second.left - 1, r_iter->second.top - 1, r_iter->second.right + 2, r_iter->second.bottom + 2);
  }

	TPLMapCI tpl_iter = p_tablemap->tpl$()->find(sel.GetString());
  if (tpl_iter != p_tablemap->tpl$()->end()) {
	  pDC->Rectangle(tpl_iter->second.left - 1, tpl_iter->second.top - 1, tpl_iter->second.right + 2, tpl_iter->second.bottom + 2);
  }
	// Clean up
	pDC->SelectObject(oldpen);
	pDC->SelectObject(oldbrush);
	ReleaseDC(pDC);
}


COpenScrapeView *COpenScrapeView::GetView()
{
	CFrameWnd * pFrame = (CFrameWnd *)(AfxGetApp()->m_pMainWnd);

	CView * pView = pFrame->GetActiveView();

	if ( !pView )
		return NULL;

	// Fail if view is of wrong kind
	// (this could occur with splitter windows, or additional
	// views on a single document
	if ( ! pView->IsKindOf( RUNTIME_CLASS(COpenScrapeView) ) )
		return NULL;

	return (COpenScrapeView *) pView;
} 

