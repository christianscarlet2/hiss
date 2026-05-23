#ifndef INC_CHAT_TERMINAL_WINDOW_H
#define INC_CHAT_TERMINAL_WINDOW_H

#include <vector>

enum ChatTerminalSection {
	kChatTerminalContext = 0,
	kChatTerminalState = 1,
	kChatTerminalDecisions = 2,
	kChatTerminalChat = 3,
	kChatTerminalSectionCount = 4
};

struct SChatTerminalScreen {
	CString name;
	CString sections[kChatTerminalSectionCount];
};

class CChatTerminalWindow : public CWnd {
	DECLARE_DYNAMIC(CChatTerminalWindow)
	DECLARE_MESSAGE_MAP()

public:
	CChatTerminalWindow();
	virtual ~CChatTerminalWindow();

	BOOL Create(CWnd *owner);
	void AppendMessage(int section, CString text, bool stream = false);
	void AppendMessage(CString screen, int section, CString text, bool stream = false);
	void ClearTerminal(void);
	void ClearScreen(CString screen);
	void AttachToOwner(bool force = false);

protected:
	afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
	afx_msg void OnSize(UINT nType, int cx, int cy);
	afx_msg void OnMoving(UINT fwSide, LPRECT pRect);
	afx_msg void OnClearClicked();
	afx_msg void OnSendClicked();
	afx_msg void OnScreenChanged();
	afx_msg LRESULT OnAppendMessage(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnClearTerminal(WPARAM wParam, LPARAM lParam);

private:
	void LayoutControls(int cx, int cy);
	int EnsureScreen(CString screen);
	void RefreshScreenList(void);
	void RefreshVisibleSections(void);
	void AppendToSection(CString screen, int section, CString text, bool stream);
	void SendChatText(void);

	CWnd *_owner;
	CButton _clear_button;
	CButton _send_button;
	CComboBox _screen_combo;
	CEdit _chat_input;
	CStatic _title;
	CStatic _section_labels[kChatTerminalSectionCount];
	CEdit _sections[kChatTerminalSectionCount];
	std::vector<SChatTerminalScreen> _screens;
	int _active_screen;
	bool _attach_left;
	bool _layout_ready;
};

extern CChatTerminalWindow *p_chat_terminal;

void ChatTerminalAppend(int section, CString text);
void ChatTerminalStream(int section, CString text);
void ChatTerminalAppendToScreen(CString screen, int section, CString text);
void ChatTerminalStreamToScreen(CString screen, int section, CString text);
void ChatTerminalClear(void);
void ChatTerminalClearScreen(CString screen);

#endif // INC_CHAT_TERMINAL_WINDOW_H
