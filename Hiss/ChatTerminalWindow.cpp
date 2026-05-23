#include "stdafx.h"
#include "ChatTerminalWindow.h"
#include "CEngineContainer.h"
#include "CSymbolEngineChipAmounts.h"
#include "CTableState.h"
#include "inlines/eval.h"

const UINT WM_CHAT_TERMINAL_APPEND = WM_APP + 410;
const UINT WM_CHAT_TERMINAL_CLEAR = WM_APP + 411;

const int kTerminalWidth = 540;
const int kTerminalHeight = 640;
const int kTerminalGap = 8;
const int kControlMargin = 8;
const int kTopHeight = 30;
const int kChatHeight = 44;
const int kRangeSelectorHeight = 236;

const UINT IDC_TERMINAL_CLEAR = 24001;
const UINT IDC_TERMINAL_SEND = 24002;
const UINT IDC_TERMINAL_CHAT = 24003;
const UINT IDC_TERMINAL_SCREEN = 24004;
const UINT IDC_TERMINAL_HOLE_CARDS = 24005;
const UINT IDC_TERMINAL_RANGE_BASE = 24200;
const UINT ID_TERMINAL_FEATURE_POT_ODDS = 24101;
const UINT ID_TERMINAL_FEATURE_IMPLIED_POT_ODDS = 24102;
const UINT ID_TERMINAL_FEATURE_REVERSE_IMPLIED_ODDS = 24103;
const UINT ID_TERMINAL_VIEW_RANGE_SELECTOR = 24104;

struct SChatTerminalMessage {
	CString screen;
	int section;
	CString text;
	bool stream;
	bool clear_screen;
};

CChatTerminalWindow *p_chat_terminal = NULL;

IMPLEMENT_DYNAMIC(CChatTerminalWindow, CWnd)

BEGIN_MESSAGE_MAP(CChatTerminalWindow, CWnd)
	ON_WM_CREATE()
	ON_WM_SIZE()
	ON_WM_MOVING()
	ON_BN_CLICKED(IDC_TERMINAL_CLEAR, &CChatTerminalWindow::OnClearClicked)
	ON_BN_CLICKED(IDC_TERMINAL_SEND, &CChatTerminalWindow::OnSendClicked)
	ON_CBN_SELCHANGE(IDC_TERMINAL_SCREEN, &CChatTerminalWindow::OnScreenChanged)
	ON_COMMAND(ID_TERMINAL_FEATURE_POT_ODDS, &CChatTerminalWindow::OnFeaturePotOdds)
	ON_UPDATE_COMMAND_UI(ID_TERMINAL_FEATURE_POT_ODDS, &CChatTerminalWindow::OnUpdateFeaturePotOdds)
	ON_COMMAND(ID_TERMINAL_FEATURE_IMPLIED_POT_ODDS, &CChatTerminalWindow::OnFeatureImpliedPotOdds)
	ON_UPDATE_COMMAND_UI(ID_TERMINAL_FEATURE_IMPLIED_POT_ODDS, &CChatTerminalWindow::OnUpdateFeatureImpliedPotOdds)
	ON_COMMAND(ID_TERMINAL_FEATURE_REVERSE_IMPLIED_ODDS, &CChatTerminalWindow::OnFeatureReverseImpliedOdds)
	ON_UPDATE_COMMAND_UI(ID_TERMINAL_FEATURE_REVERSE_IMPLIED_ODDS, &CChatTerminalWindow::OnUpdateFeatureReverseImpliedOdds)
	ON_COMMAND(ID_TERMINAL_VIEW_RANGE_SELECTOR, &CChatTerminalWindow::OnViewRangeSelector)
	ON_UPDATE_COMMAND_UI(ID_TERMINAL_VIEW_RANGE_SELECTOR, &CChatTerminalWindow::OnUpdateViewRangeSelector)
	ON_EN_KILLFOCUS(IDC_TERMINAL_HOLE_CARDS, &CChatTerminalWindow::OnHoleCardsChanged)
	ON_CONTROL_RANGE(BN_CLICKED, IDC_TERMINAL_RANGE_BASE, IDC_TERMINAL_RANGE_BASE + 168, &CChatTerminalWindow::OnRangeChanged)
	ON_MESSAGE(WM_CHAT_TERMINAL_APPEND, &CChatTerminalWindow::OnAppendMessage)
	ON_MESSAGE(WM_CHAT_TERMINAL_CLEAR, &CChatTerminalWindow::OnClearTerminal)
END_MESSAGE_MAP()

CChatTerminalWindow::CChatTerminalWindow()
{
	_owner = NULL;
	_attach_left = false;
	_active_screen = 0;
	_layout_ready = false;
	_pot_odds_enabled = false;
	_implied_pot_odds_enabled = false;
	_reverse_implied_odds_enabled = false;
	_range_selector_visible = false;
	for (int i = 0; i < 169; ++i) {
		_range_enabled[i] = true;
	}
}

CChatTerminalWindow::~CChatTerminalWindow()
{
}

BOOL CChatTerminalWindow::Create(CWnd *owner)
{
	_owner = owner;
	CString class_name = AfxRegisterWndClass(
		CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW,
		AfxGetApp()->LoadStandardCursor(IDC_ARROW),
		(HBRUSH)(COLOR_WINDOW + 1),
		::LoadIcon(NULL, IDI_APPLICATION));

	DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
	DWORD ex_style = WS_EX_TOOLWINDOW;
	BOOL created = CWnd::CreateEx(
		ex_style,
		class_name,
		"OpenHoldem Terminal",
		style,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		kTerminalWidth,
		kTerminalHeight,
		owner == NULL ? NULL : owner->GetSafeHwnd(),
		NULL);
	if (created) {
		AttachToOwner(true);
	}
	return created;
}

int CChatTerminalWindow::OnCreate(LPCREATESTRUCT lpCreateStruct)
{
	if (CWnd::OnCreate(lpCreateStruct) == -1) {
		return -1;
	}

	_title.Create("OpenHoldem Terminal", WS_CHILD | WS_VISIBLE | SS_LEFT, CRect(0, 0, 0, 0), this);
	_clear_button.Create("Clear", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, CRect(0, 0, 0, 0), this, IDC_TERMINAL_CLEAR);
	_send_button.Create("Send", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, CRect(0, 0, 0, 0), this, IDC_TERMINAL_SEND);
	_screen_combo.Create(WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST, CRect(0, 0, 0, 0), this, IDC_TERMINAL_SCREEN);
	_hole_cards_label.Create("Hole cards", WS_CHILD | WS_VISIBLE | SS_LEFT, CRect(0, 0, 0, 0), this);
	_hole_cards_input.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, CRect(0, 0, 0, 0), this, IDC_TERMINAL_HOLE_CARDS);
	_chat_input.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, CRect(0, 0, 0, 0), this, IDC_TERMINAL_CHAT);

	_menu.CreateMenu();
	CMenu view_menu;
	view_menu.CreatePopupMenu();
	view_menu.AppendMenu(MF_STRING, ID_TERMINAL_VIEW_RANGE_SELECTOR, "Show Range Selector");
	_menu.AppendMenu(MF_POPUP, (UINT_PTR)view_menu.Detach(), "View");
	CMenu features_menu;
	features_menu.CreatePopupMenu();
	features_menu.AppendMenu(MF_STRING, ID_TERMINAL_FEATURE_POT_ODDS, "Enable Pot Odds Calculation");
	features_menu.AppendMenu(MF_STRING, ID_TERMINAL_FEATURE_IMPLIED_POT_ODDS, "Enable Implied Pot Odds");
	features_menu.AppendMenu(MF_STRING, ID_TERMINAL_FEATURE_REVERSE_IMPLIED_ODDS, "Enable Reverse Implied Odds");
	_menu.AppendMenu(MF_POPUP, (UINT_PTR)features_menu.Detach(), "Features");
	SetMenu(&_menu);

	_range_label.Create("Opponent range", WS_CHILD | SS_LEFT, CRect(0, 0, 0, 0), this);
	BuildRangeSelector();

	const char *labels[kChatTerminalSectionCount] = {
		"Context",
		"State",
		"Decisions",
		"Chat"
	};
	for (int i = 0; i < kChatTerminalSectionCount; ++i) {
		_section_labels[i].Create(labels[i], WS_CHILD | WS_VISIBLE | SS_LEFT, CRect(0, 0, 0, 0), this);
		_sections[i].Create(
			WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
			ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
			CRect(0, 0, 0, 0),
			this,
			25000 + i);
		_sections[i].SetLimitText(0);
	}

	_layout_ready = true;
	EnsureScreen("main");
	RefreshScreenList();
	LayoutControls(lpCreateStruct->cx, lpCreateStruct->cy);
	AppendToSection("main", kChatTerminalContext, "Terminal API ready.\r\n", true);
	AppendToSection("main", kChatTerminalChat, "Type steering notes here. The send hook is intentionally local for now.\r\n", true);
	return 0;
}

void CChatTerminalWindow::OnSize(UINT nType, int cx, int cy)
{
	CWnd::OnSize(nType, cx, cy);
	LayoutControls(cx, cy);
}

void CChatTerminalWindow::OnMoving(UINT fwSide, LPRECT pRect)
{
	CWnd::OnMoving(fwSide, pRect);
	if (_owner == NULL || !::IsWindow(_owner->GetSafeHwnd())) {
		return;
	}

	CRect owner_rect;
	_owner->GetWindowRect(&owner_rect);
	int terminal_center = (pRect->left + pRect->right) / 2;
	int owner_center = (owner_rect.left + owner_rect.right) / 2;
	_attach_left = terminal_center < owner_center;

	int width = pRect->right - pRect->left;
	int height = pRect->bottom - pRect->top;
	if (_attach_left) {
		pRect->right = owner_rect.left - kTerminalGap;
		pRect->left = pRect->right - width;
	}
	else {
		pRect->left = owner_rect.right + kTerminalGap;
		pRect->right = pRect->left + width;
	}
	pRect->top = owner_rect.top;
	pRect->bottom = pRect->top + height;
}

void CChatTerminalWindow::AttachToOwner(bool force)
{
	if (_owner == NULL || !::IsWindow(_owner->GetSafeHwnd()) || !::IsWindow(GetSafeHwnd())) {
		return;
	}

	CRect owner_rect, rect;
	_owner->GetWindowRect(&owner_rect);
	GetWindowRect(&rect);
	if (!force) {
		int terminal_center = rect.CenterPoint().x;
		int owner_center = owner_rect.CenterPoint().x;
		_attach_left = terminal_center < owner_center;
	}

	int x = _attach_left ? owner_rect.left - rect.Width() - kTerminalGap : owner_rect.right + kTerminalGap;
	SetWindowPos(NULL, x, owner_rect.top, rect.Width(), rect.Height(), SWP_NOZORDER | SWP_NOACTIVATE);
}

void CChatTerminalWindow::LayoutControls(int cx, int cy)
{
	if (!_layout_ready || cx <= 0 || cy <= 0) {
		return;
	}

	int left = kControlMargin;
	int right = cx - kControlMargin;
	int top = kControlMargin;
	_title.MoveWindow(left, top + 5, 145, 18);
	_screen_combo.MoveWindow(left + 150, top, 120, 120);
	_hole_cards_label.MoveWindow(left + 278, top + 5, 58, 18);
	_hole_cards_input.MoveWindow(left + 338, top, 48, 22);
	_clear_button.MoveWindow(right - 64, top, 64, 22);

	int grid_top = top + kTopHeight;
	if (_range_selector_visible) {
		_range_label.MoveWindow(left, grid_top + 3, 140, 14);
		_range_label.ShowWindow(SW_SHOW);
		const int cell_width = 34;
		const int cell_height = 15;
		const int range_left = left;
		const int range_top = grid_top + 20;
		for (int row = 0; row < 13; ++row) {
			for (int col = 0; col < 13; ++col) {
				int index = row * 13 + col;
				_range_buttons[index].MoveWindow(range_left + col * cell_width, range_top + row * cell_height, cell_width - 1, cell_height - 1);
				_range_buttons[index].ShowWindow(SW_SHOW);
			}
		}
		grid_top += kRangeSelectorHeight;
	}
	else {
		_range_label.ShowWindow(SW_HIDE);
		for (int i = 0; i < 169; ++i) {
			_range_buttons[i].ShowWindow(SW_HIDE);
		}
	}
	int chat_top = cy - kControlMargin - kChatHeight;
	int grid_bottom = chat_top - kControlMargin;
	int grid_height = max(120, grid_bottom - grid_top);
	int col_gap = 6;
	int row_gap = 18;
	int col_width = (right - left - col_gap) / 2;
	int row_height = (grid_height - row_gap) / 2;

	for (int i = 0; i < kChatTerminalSectionCount; ++i) {
		int col = i % 2;
		int row = i / 2;
		int section_left = left + col * (col_width + col_gap);
		int section_top = grid_top + row * (row_height + row_gap);
		_section_labels[i].MoveWindow(section_left, section_top, col_width, 14);
		_sections[i].MoveWindow(section_left, section_top + 14, col_width, row_height - 14);
	}

	_chat_input.MoveWindow(left, chat_top, right - left - 72, 22);
	_send_button.MoveWindow(right - 64, chat_top, 64, 22);
}

void CChatTerminalWindow::AppendMessage(int section, CString text, bool stream)
{
	AppendMessage("main", section, text, stream);
}

void CChatTerminalWindow::AppendMessage(CString screen, int section, CString text, bool stream)
{
	if (!::IsWindow(GetSafeHwnd())) {
		return;
	}
	SChatTerminalMessage *message = new SChatTerminalMessage;
	message->screen = screen;
	message->section = section;
	message->text = text;
	message->stream = stream;
	message->clear_screen = false;
	PostMessage(WM_CHAT_TERMINAL_APPEND, 0, (LPARAM)message);
}

void CChatTerminalWindow::ClearTerminal(void)
{
	if (::IsWindow(GetSafeHwnd())) {
		PostMessage(WM_CHAT_TERMINAL_CLEAR, 0, 0);
	}
}

void CChatTerminalWindow::ClearScreen(CString screen)
{
	if (!::IsWindow(GetSafeHwnd())) {
		return;
	}
	SChatTerminalMessage *message = new SChatTerminalMessage;
	message->screen = screen;
	message->section = 0;
	message->stream = false;
	message->clear_screen = true;
	PostMessage(WM_CHAT_TERMINAL_APPEND, 0, (LPARAM)message);
}

int CChatTerminalWindow::EnsureScreen(CString screen)
{
	screen.Trim();
	if (screen.IsEmpty()) {
		screen = "main";
	}
	for (size_t i = 0; i < _screens.size(); ++i) {
		if (_screens[i].name.CompareNoCase(screen) == 0) {
			return (int)i;
		}
	}
	SChatTerminalScreen new_screen;
	new_screen.name = screen;
	_screens.push_back(new_screen);
	return (int)_screens.size() - 1;
}

void CChatTerminalWindow::RefreshScreenList(void)
{
	CString active_name;
	if (_active_screen >= 0 && _active_screen < (int)_screens.size()) {
		active_name = _screens[_active_screen].name;
	}
	_screen_combo.ResetContent();
	for (size_t i = 0; i < _screens.size(); ++i) {
		int item = _screen_combo.AddString(_screens[i].name);
		_screen_combo.SetItemData(item, (DWORD_PTR)i);
	}
	int selection = active_name.IsEmpty() ? 0 : _screen_combo.FindStringExact(-1, active_name);
	if (selection == CB_ERR) {
		selection = 0;
	}
	_screen_combo.SetCurSel(selection);
	if (selection != CB_ERR) {
		_active_screen = (int)_screen_combo.GetItemData(selection);
	}
}

void CChatTerminalWindow::RefreshVisibleSections(void)
{
	if (_active_screen < 0 || _active_screen >= (int)_screens.size()) {
		return;
	}
	for (int i = 0; i < kChatTerminalSectionCount; ++i) {
		CString text = _screens[_active_screen].sections[i];
		if (i == kChatTerminalState && !_screens[_active_screen].pinned_state.IsEmpty()) {
			text = _screens[_active_screen].pinned_state + text;
		}
		_sections[i].SetWindowText(text);
		_sections[i].LineScroll(_sections[i].GetLineCount());
	}
}

void CChatTerminalWindow::AppendToSection(CString screen, int section, CString text, bool stream)
{
	int screen_index = EnsureScreen(screen);
	if (section < 0 || section >= kChatTerminalSectionCount) {
		section = kChatTerminalContext;
	}
	if (!stream && text.Right(2) != "\r\n") {
		text += "\r\n";
	}
	_screens[screen_index].sections[section] += text;
	if (screen_index != _active_screen) {
		RefreshScreenList();
		return;
	}
	if (section == kChatTerminalState && !_screens[screen_index].pinned_state.IsEmpty()) {
		RefreshVisibleSections();
		return;
	}
	int length = _sections[section].GetWindowTextLength();
	_sections[section].SetSel(length, length);
	_sections[section].ReplaceSel(text);
	_sections[section].LineScroll(_sections[section].GetLineCount());
}

void CChatTerminalWindow::SetPinnedState(CString screen, CString text)
{
	int screen_index = EnsureScreen(screen);
	if (!text.IsEmpty() && text.Right(2) != "\r\n") {
		text += "\r\n";
	}
	_screens[screen_index].pinned_state = text;
	if (screen_index == _active_screen) {
		RefreshVisibleSections();
	}
}

void CChatTerminalWindow::OnClearClicked()
{
	if (_active_screen >= 0 && _active_screen < (int)_screens.size()) {
		ClearScreen(_screens[_active_screen].name);
	}
}

void CChatTerminalWindow::OnSendClicked()
{
	SendChatText();
}

void CChatTerminalWindow::OnScreenChanged()
{
	int sel = _screen_combo.GetCurSel();
	if (sel != CB_ERR) {
		_active_screen = (int)_screen_combo.GetItemData(sel);
		RefreshVisibleSections();
	}
}

void CChatTerminalWindow::OnFeaturePotOdds()
{
	_pot_odds_enabled = !_pot_odds_enabled;
	_menu.ModifyMenu(ID_TERMINAL_FEATURE_POT_ODDS, MF_BYCOMMAND | MF_STRING,
		ID_TERMINAL_FEATURE_POT_ODDS,
		_pot_odds_enabled ? "Disable Pot Odds Calculation" : "Enable Pot Odds Calculation");
	if (!_pot_odds_enabled) {
		_last_pot_odds_board = "";
	}
	UpdatePotOddsForCurrentBoard(true);
}

void CChatTerminalWindow::OnUpdateFeaturePotOdds(CCmdUI *pCmdUI)
{
	pCmdUI->SetCheck(_pot_odds_enabled);
	pCmdUI->SetText(_pot_odds_enabled ? "Disable Pot Odds Calculation" : "Enable Pot Odds Calculation");
}

void CChatTerminalWindow::OnFeatureImpliedPotOdds()
{
	_implied_pot_odds_enabled = !_implied_pot_odds_enabled;
	_menu.ModifyMenu(ID_TERMINAL_FEATURE_IMPLIED_POT_ODDS, MF_BYCOMMAND | MF_STRING,
		ID_TERMINAL_FEATURE_IMPLIED_POT_ODDS,
		_implied_pot_odds_enabled ? "Disable Implied Pot Odds" : "Enable Implied Pot Odds");
	if (_implied_pot_odds_enabled) {
		SetRangeSelectorVisible(true);
		UpdatePotOddsForCurrentBoard(true);
	}
	else {
		UpdatePotOddsForCurrentBoard(true);
	}
}

void CChatTerminalWindow::OnUpdateFeatureImpliedPotOdds(CCmdUI *pCmdUI)
{
	pCmdUI->SetCheck(_implied_pot_odds_enabled);
	pCmdUI->SetText(_implied_pot_odds_enabled ? "Disable Implied Pot Odds" : "Enable Implied Pot Odds");
}

void CChatTerminalWindow::OnFeatureReverseImpliedOdds()
{
	_reverse_implied_odds_enabled = !_reverse_implied_odds_enabled;
	_menu.ModifyMenu(ID_TERMINAL_FEATURE_REVERSE_IMPLIED_ODDS, MF_BYCOMMAND | MF_STRING,
		ID_TERMINAL_FEATURE_REVERSE_IMPLIED_ODDS,
		_reverse_implied_odds_enabled ? "Disable Reverse Implied Odds" : "Enable Reverse Implied Odds");
	if (_reverse_implied_odds_enabled) {
		SetRangeSelectorVisible(true);
	}
	UpdatePotOddsForCurrentBoard(true);
}

void CChatTerminalWindow::OnUpdateFeatureReverseImpliedOdds(CCmdUI *pCmdUI)
{
	pCmdUI->SetCheck(_reverse_implied_odds_enabled);
	pCmdUI->SetText(_reverse_implied_odds_enabled ? "Disable Reverse Implied Odds" : "Enable Reverse Implied Odds");
}

void CChatTerminalWindow::OnViewRangeSelector()
{
	SetRangeSelectorVisible(!_range_selector_visible);
}

void CChatTerminalWindow::OnUpdateViewRangeSelector(CCmdUI *pCmdUI)
{
	pCmdUI->SetCheck(_range_selector_visible);
	pCmdUI->SetText(_range_selector_visible ? "Hide Range Selector" : "Show Range Selector");
}

void CChatTerminalWindow::OnHoleCardsChanged()
{
	if (_pot_odds_enabled || _implied_pot_odds_enabled || _reverse_implied_odds_enabled) {
		UpdatePotOddsForCurrentBoard(true);
	}
}

void CChatTerminalWindow::OnRangeChanged(UINT id)
{
	int index = (int)(id - IDC_TERMINAL_RANGE_BASE);
	if (index >= 0 && index < 169) {
		_range_enabled[index] = _range_buttons[index].GetCheck() != 0;
		if (_implied_pot_odds_enabled || _reverse_implied_odds_enabled) {
			UpdatePotOddsForCurrentBoard(true);
		}
	}
}

void CChatTerminalWindow::SendChatText(void)
{
	CString text;
	_chat_input.GetWindowText(text);
	text.Trim();
	if (text.IsEmpty()) {
		return;
	}
	_chat_input.SetWindowText("");
	CString line;
	line.Format("You: %s", text.GetString());
	CString screen = "main";
	if (_active_screen >= 0 && _active_screen < (int)_screens.size()) {
		screen = _screens[_active_screen].name;
	}
	AppendToSection(screen, kChatTerminalChat, line, false);
}

LRESULT CChatTerminalWindow::OnAppendMessage(WPARAM wParam, LPARAM lParam)
{
	SChatTerminalMessage *message = (SChatTerminalMessage *)lParam;
	if (message != NULL) {
		if (message->clear_screen) {
			int screen_index = EnsureScreen(message->screen);
			for (int i = 0; i < kChatTerminalSectionCount; ++i) {
				_screens[screen_index].sections[i] = "";
			}
			if (screen_index == _active_screen) {
				RefreshVisibleSections();
			}
		}
		else {
			AppendToSection(message->screen, message->section, message->text, message->stream);
		}
		delete message;
	}
	return 0;
}

LRESULT CChatTerminalWindow::OnClearTerminal(WPARAM wParam, LPARAM lParam)
{
	for (int i = 0; i < kChatTerminalSectionCount; ++i) {
		_sections[i].SetWindowText("");
	}
	for (size_t s = 0; s < _screens.size(); ++s) {
		for (int i = 0; i < kChatTerminalSectionCount; ++i) {
			_screens[s].sections[i] = "";
		}
	}
	RefreshVisibleSections();
	return 0;
}

CString CChatTerminalWindow::CurrentCommunityCardsText(void)
{
	CString board;
	if (p_table_state == NULL) {
		return board;
	}
	for (int i = 0; i < kNumberOfCommunityCards; ++i) {
		Card *card = p_table_state->CommonCards(i);
		if (card != NULL && card->IsKnownCard()) {
			board += card->ToString();
		}
	}
	return board;
}

static int TerminalRankFromChar(char rank)
{
	rank = (char)toupper(rank);
	if (rank >= '2' && rank <= '9') return rank - '2';
	if (rank == 'T') return Rank_TEN;
	if (rank == 'J') return Rank_JACK;
	if (rank == 'Q') return Rank_QUEEN;
	if (rank == 'K') return Rank_KING;
	if (rank == 'A') return Rank_ACE;
	return -1;
}

static int TerminalSuitFromChar(char suit)
{
	suit = (char)tolower(suit);
	if (suit == 'h') return Suit_HEARTS;
	if (suit == 'd') return Suit_DIAMONDS;
	if (suit == 'c') return Suit_CLUBS;
	if (suit == 's') return Suit_SPADES;
	return -1;
}

static bool TerminalParseCard(CString text, int offset, int *card)
{
	if (card == NULL || offset + 1 >= text.GetLength()) {
		return false;
	}
	int rank = TerminalRankFromChar((char)text[offset]);
	int suit = TerminalSuitFromChar((char)text[offset + 1]);
	if (rank < 0 || suit < 0) {
		return false;
	}
	*card = StdDeck_MAKE_CARD(rank, suit);
	return true;
}

CString CChatTerminalWindow::CalculatePotOddsText(CString hole_cards, CString board_cards, bool use_range, bool reverse, CString label)
{
	hole_cards.Trim();
	hole_cards.Remove(' ');
	board_cards.Trim();
	board_cards.Remove(' ');
	if (hole_cards.GetLength() != 4) {
		return label + ": enter hole cards like AsTc";
	}

	int hero0 = -1, hero1 = -1;
	if (!TerminalParseCard(hole_cards, 0, &hero0) || !TerminalParseCard(hole_cards, 2, &hero1) || hero0 == hero1) {
		return label + ": invalid hole cards";
	}

	std::vector<int> board;
	for (int i = 0; i + 1 < board_cards.GetLength(); i += 2) {
		int card = -1;
		if (TerminalParseCard(board_cards, i, &card)) {
			board.push_back(card);
		}
	}
	if (board.size() < 3) {
		return label + ": waiting for community cards";
	}
	if (board.size() > 5) {
		return label + ": invalid board";
	}

	bool used[StdDeck_N_CARDS] = { false };
	used[hero0] = true;
	used[hero1] = true;
	for (size_t i = 0; i < board.size(); ++i) {
		if (used[board[i]]) {
			return label + ": duplicate card detected";
		}
		used[board[i]] = true;
	}

	CardMask hero_cards, board_mask;
	CardMask_RESET(hero_cards);
	CardMask_RESET(board_mask);
	CardMask_SET(hero_cards, hero0);
	CardMask_SET(hero_cards, hero1);
	for (size_t i = 0; i < board.size(); ++i) {
		CardMask_SET(board_mask, board[i]);
	}

	long wins = 0, ties = 0, losses = 0;
	long range_hands = 0;
	int missing_board = 5 - (int)board.size();
	for (int opp0 = 0; opp0 < StdDeck_N_CARDS; ++opp0) {
		if (used[opp0]) continue;
		used[opp0] = true;
		for (int opp1 = opp0 + 1; opp1 < StdDeck_N_CARDS; ++opp1) {
			if (used[opp1]) continue;
			if (use_range && !RangeAllowsOpponentHand(opp0, opp1)) continue;
			++range_hands;
			used[opp1] = true;

			CardMask opp_cards;
			CardMask_RESET(opp_cards);
			CardMask_SET(opp_cards, opp0);
			CardMask_SET(opp_cards, opp1);

			if (missing_board == 0) {
				CardMask hero_eval, opp_eval;
				CardMask_OR(hero_eval, hero_cards, board_mask);
				CardMask_OR(opp_eval, opp_cards, board_mask);
				HandVal hero_value = Hand_EVAL_N(hero_eval, 7);
				HandVal opp_value = Hand_EVAL_N(opp_eval, 7);
				if (hero_value > opp_value) ++wins;
				else if (hero_value == opp_value) ++ties;
				else ++losses;
			}
			else if (missing_board == 1) {
				for (int b0 = 0; b0 < StdDeck_N_CARDS; ++b0) {
					if (used[b0]) continue;
					CardMask runout, full_board, hero_eval, opp_eval;
					CardMask_RESET(runout);
					CardMask_SET(runout, b0);
					CardMask_OR(full_board, board_mask, runout);
					CardMask_OR(hero_eval, hero_cards, full_board);
					CardMask_OR(opp_eval, opp_cards, full_board);
					HandVal hero_value = Hand_EVAL_N(hero_eval, 7);
					HandVal opp_value = Hand_EVAL_N(opp_eval, 7);
					if (hero_value > opp_value) ++wins;
					else if (hero_value == opp_value) ++ties;
					else ++losses;
				}
			}
			else {
				for (int b0 = 0; b0 < StdDeck_N_CARDS; ++b0) {
					if (used[b0]) continue;
					used[b0] = true;
					for (int b1 = b0 + 1; b1 < StdDeck_N_CARDS; ++b1) {
						if (used[b1]) continue;
						CardMask runout, full_board, hero_eval, opp_eval;
						CardMask_RESET(runout);
						CardMask_SET(runout, b0);
						CardMask_SET(runout, b1);
						CardMask_OR(full_board, board_mask, runout);
						CardMask_OR(hero_eval, hero_cards, full_board);
						CardMask_OR(opp_eval, opp_cards, full_board);
						HandVal hero_value = Hand_EVAL_N(hero_eval, 7);
						HandVal opp_value = Hand_EVAL_N(opp_eval, 7);
						if (hero_value > opp_value) ++wins;
						else if (hero_value == opp_value) ++ties;
						else ++losses;
					}
					used[b0] = false;
				}
			}
			used[opp1] = false;
		}
		used[opp0] = false;
	}

	double total = (double)wins + (double)ties + (double)losses;
	if (total <= 0.0) {
		return label + ": no valid outcomes";
	}
	double equity = ((double)wins + (double)ties * 0.5) / total;
	double opponent_equity = ((double)losses + (double)ties * 0.5) / total;
	double displayed_equity = reverse ? opponent_equity : equity;
	double pot = p_engine_container == NULL ? 0.0 : p_engine_container->symbol_engine_chip_amounts()->pot();
	double call = p_engine_container == NULL ? 0.0 : p_engine_container->symbol_engine_chip_amounts()->call();
	CString text;
	if (call > 0.0) {
		double required = call / (pot + call);
		text.Format("%s: %s / %s | %s %.1f%% | required %.1f%% | call %.2f into %.2f",
			label.GetString(), hole_cards.GetString(), board_cards.GetString(),
			reverse ? "opponent equity" : "equity", displayed_equity * 100.0, required * 100.0, call, pot);
	}
	else {
		text.Format("%s: %s / %s | %s %.1f%% | no call due",
			label.GetString(), hole_cards.GetString(), board_cards.GetString(),
			reverse ? "opponent equity" : "equity", displayed_equity * 100.0);
	}
	if (use_range) {
		CString suffix;
		suffix.Format(" | range hands %ld", range_hands);
		text += suffix;
	}
	return text;
}

void CChatTerminalWindow::UpdatePotOddsForCurrentBoard(bool force)
{
	if (!_pot_odds_enabled && !_implied_pot_odds_enabled && !_reverse_implied_odds_enabled) {
		SetPinnedState("main", "");
		return;
	}
	CString board = CurrentCommunityCardsText();
	if (!force && board == _last_pot_odds_board) {
		return;
	}
	_last_pot_odds_board = board;
	CString hole_cards;
	_hole_cards_input.GetWindowText(hole_cards);
	CString state_text;
	if (_pot_odds_enabled) {
		state_text += CalculatePotOddsText(hole_cards, board, false, false, "Pot odds");
		state_text += "\r\n";
	}
	if (_implied_pot_odds_enabled) {
		state_text += CalculatePotOddsText(hole_cards, board, true, false, "Implied pot odds");
		state_text += "\r\n";
	}
	if (_reverse_implied_odds_enabled) {
		state_text += CalculatePotOddsText(hole_cards, board, true, true, "Reverse implied odds");
		state_text += "\r\n";
	}
	SetPinnedState("main", state_text);
}

void CChatTerminalWindow::MaybeUpdatePotOddsFromTableState(void)
{
	UpdatePotOddsForCurrentBoard(false);
}

void CChatTerminalWindow::BuildRangeSelector(void)
{
	for (int row = 0; row < 13; ++row) {
		for (int col = 0; col < 13; ++col) {
			int index = row * 13 + col;
			_range_buttons[index].Create(RangeLabel(row, col),
				WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE,
				CRect(0, 0, 0, 0),
				this,
				IDC_TERMINAL_RANGE_BASE + index);
			_range_buttons[index].SetCheck(BST_CHECKED);
			_range_buttons[index].ShowWindow(SW_HIDE);
		}
	}
}

void CChatTerminalWindow::SetRangeSelectorVisible(bool visible)
{
	_range_selector_visible = visible;
	_menu.ModifyMenu(ID_TERMINAL_VIEW_RANGE_SELECTOR, MF_BYCOMMAND | MF_STRING,
		ID_TERMINAL_VIEW_RANGE_SELECTOR,
		_range_selector_visible ? "Hide Range Selector" : "Show Range Selector");
	CRect rect;
	GetClientRect(&rect);
	LayoutControls(rect.Width(), rect.Height());
}

CString CChatTerminalWindow::RangeLabel(int row, int col)
{
	const char *ranks = "AKQJT98765432";
	CString label;
	if (row == col) {
		label.Format("%c%c", ranks[row], ranks[col]);
	}
	else if (row < col) {
		label.Format("%c%cs", ranks[row], ranks[col]);
	}
	else {
		label.Format("%c%co", ranks[col], ranks[row]);
	}
	return label;
}

bool CChatTerminalWindow::RangeAllowsOpponentHand(int first_card, int second_card)
{
	const int ace_high_row_from_rank[] = { 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
	int rank0 = StdDeck_RANK(first_card);
	int rank1 = StdDeck_RANK(second_card);
	int suit0 = StdDeck_SUIT(first_card);
	int suit1 = StdDeck_SUIT(second_card);
	if (rank0 == rank1) {
		int row = ace_high_row_from_rank[rank0];
		return _range_enabled[row * 13 + row];
	}

	int high_rank = rank0 > rank1 ? rank0 : rank1;
	int low_rank = rank0 > rank1 ? rank1 : rank0;
	int high_row = ace_high_row_from_rank[high_rank];
	int low_row = ace_high_row_from_rank[low_rank];
	int index = suit0 == suit1
		? high_row * 13 + low_row
		: low_row * 13 + high_row;
	return index >= 0 && index < 169 && _range_enabled[index];
}

void ChatTerminalAppend(int section, CString text)
{
	if (p_chat_terminal != NULL) {
		p_chat_terminal->AppendMessage(section, text, false);
	}
}

void ChatTerminalStream(int section, CString text)
{
	if (p_chat_terminal != NULL) {
		p_chat_terminal->AppendMessage(section, text, true);
	}
}

void ChatTerminalAppendToScreen(CString screen, int section, CString text)
{
	if (p_chat_terminal != NULL) {
		p_chat_terminal->AppendMessage(screen, section, text, false);
	}
}

void ChatTerminalStreamToScreen(CString screen, int section, CString text)
{
	if (p_chat_terminal != NULL) {
		p_chat_terminal->AppendMessage(screen, section, text, true);
	}
}

void ChatTerminalClear(void)
{
	if (p_chat_terminal != NULL) {
		p_chat_terminal->ClearTerminal();
	}
}

void ChatTerminalClearScreen(CString screen)
{
	if (p_chat_terminal != NULL) {
		p_chat_terminal->ClearScreen(screen);
	}
}
