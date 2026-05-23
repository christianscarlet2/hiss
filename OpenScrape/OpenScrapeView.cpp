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
#include <ctype.h>

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

// COpenScrapeView

IMPLEMENT_DYNCREATE(COpenScrapeView, CView)

BEGIN_MESSAGE_MAP(COpenScrapeView, CView)
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONUP()
	ON_WM_RBUTTONDOWN()
	ON_WM_MOUSEMOVE()
	ON_WM_SETCURSOR()
	ON_WM_KEYDOWN()
	ON_COMMAND(ID_EDIT_UNDO, &COpenScrapeView::OnEditUndo)
	ON_COMMAND(ID_EDIT_REDO, &COpenScrapeView::OnEditRedo)
	ON_UPDATE_COMMAND_UI(ID_EDIT_UNDO, &COpenScrapeView::OnUpdateEditUndo)
	ON_UPDATE_COMMAND_UI(ID_EDIT_REDO, &COpenScrapeView::OnUpdateEditRedo)
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

	dragging = false;
	dragged_region = "";
	dragged_group = "";
	group_color_index = 3;

	drawing_rect = drawing_started = false;
	color_point_mode = false;
	group_box_mode = group_box_started = false;
	hCurDrawRect = ::LoadCursor(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDC_DRAWRECTCURSOR));
	hCurStandard = ::LoadCursor(NULL, IDC_ARROW);
}

COpenScrapeView::~COpenScrapeView()
{
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

	SOpenScrapeRegionGroup new_group;
	new_group.name = new_group_name;
	new_group.color = new_color;
	new_group.members = new_members;
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

		SOpenScrapeRegionGroup group;
		group.name = name;
		group.color = strtoul(color.GetString(), NULL, 16);
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

void COpenScrapeView::DrawRegionGroups(CDC *pDC)
{
	RebuildGroupBounds();
	for (std::map<CString, SOpenScrapeRegionGroup>::iterator g = region_groups.begin(); g != region_groups.end(); ++g) {
		if (g->second.members.empty()) {
			continue;
		}
		CPen group_pen;
		group_pen.CreatePen(PS_SOLID, 2, g->second.color);
		CPen *old_pen = pDC->SelectObject(&group_pen);
		CGdiObject *old_brush = pDC->SelectStockObject(NULL_BRUSH);
		pDC->Rectangle(g->second.bounds.left - 4, g->second.bounds.top - 18,
			g->second.bounds.right + 5, g->second.bounds.bottom + 5);
		pDC->SetTextColor(g->second.color);
		pDC->SetBkMode(TRANSPARENT);
		pDC->TextOut(g->second.bounds.left - 2, g->second.bounds.top - 17, g->second.name);
		pDC->SelectObject(old_pen);
		pDC->SelectObject(old_brush);
	}
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
	}

	DrawRegionGroups(pDC);

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

	if (color_point_mode) {
		color_point_mode = false;
		if (theApp.m_TableMapDlg != NULL) {
			theApp.m_TableMapDlg->CaptureColorPresetPoint(point);
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

					dragging = true;
					dragged_region = clicked_region->second.name;
					dragged_group = GroupNameForRegion(dragged_region);
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
		CView::OnRButtonDown(nFlags, point);
		return;
	}

	CString group_name = GroupNameForRegion(region_name);
	if (group_name.IsEmpty()) {
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
	menu.AppendMenu(MF_STRING, kDeleteRegionMenuId, "Delete");
	menu.AppendMenu(MF_STRING, kUngroupRegionMenuId, "Ungroup this item");

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

void COpenScrapeView::OnMouseMove(UINT nFlags, CPoint point)
{
	COpenScrapeDoc		*pDoc = GetDocument();
	int					width, height;
	CString				text;

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
	if (drawing_rect)
	{
		::SetCursor(hCurDrawRect);
		return true;
	}

	return CView::OnSetCursor(pWnd, nHitTest, message);
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

