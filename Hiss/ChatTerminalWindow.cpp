#include "stdafx.h"
#include "ChatTerminalWindow.h"

const UINT WM_CHAT_TERMINAL_APPEND = WM_APP + 410;
const UINT WM_CHAT_TERMINAL_CLEAR = WM_APP + 411;

const int kTerminalWidth = 420;
const int kTerminalHeight = 640;
const int kTerminalGap = 8;
const int kControlMargin = 8;
const int kTopHeight = 30;
const int kChatHeight = 44;

const UINT IDC_TERMINAL_CLEAR = 24001;
const UINT IDC_TERMINAL_SEND = 24002;
const UINT IDC_TERMINAL_CHAT = 24003;
const UINT IDC_TERMINAL_SCREEN = 24004;

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
	ON_MESSAGE(WM_CHAT_TERMINAL_APPEND, &CChatTerminalWindow::OnAppendMessage)
	ON_MESSAGE(WM_CHAT_TERMINAL_CLEAR, &CChatTerminalWindow::OnClearTerminal)
END_MESSAGE_MAP()

CChatTerminalWindow::CChatTerminalWindow()
{
	_owner = NULL;
	_attach_left = false;
	_active_screen = 0;
	_layout_ready = false;
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
	_chat_input.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, CRect(0, 0, 0, 0), this, IDC_TERMINAL_CHAT);

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
	_screen_combo.MoveWindow(left + 150, top, 130, 120);
	_clear_button.MoveWindow(right - 64, top, 64, 22);

	int grid_top = top + kTopHeight;
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
		_sections[i].SetWindowText(_screens[_active_screen].sections[i]);
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
	int length = _sections[section].GetWindowTextLength();
	_sections[section].SetSel(length, length);
	_sections[section].ReplaceSel(text);
	_sections[section].LineScroll(_sections[section].GetLineCount());
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
	return 0;
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
