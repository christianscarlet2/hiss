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
	CString pinned_state;
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
	void MaybeUpdatePotOddsFromTableState(void);

protected:
	afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
	afx_msg void OnSize(UINT nType, int cx, int cy);
	afx_msg void OnMoving(UINT fwSide, LPRECT pRect);
	afx_msg void OnClearClicked();
	afx_msg void OnSendClicked();
	afx_msg void OnScreenChanged();
	afx_msg void OnFeaturePotOdds();
	afx_msg void OnUpdateFeaturePotOdds(CCmdUI *pCmdUI);
	afx_msg void OnFeatureImpliedPotOdds();
	afx_msg void OnUpdateFeatureImpliedPotOdds(CCmdUI *pCmdUI);
	afx_msg void OnFeatureReverseImpliedOdds();
	afx_msg void OnUpdateFeatureReverseImpliedOdds(CCmdUI *pCmdUI);
	afx_msg void OnViewRangeSelector();
	afx_msg void OnUpdateViewRangeSelector(CCmdUI *pCmdUI);
	afx_msg void OnHoleCardsChanged();
	afx_msg void OnRangeChanged(UINT id);
	afx_msg LRESULT OnAppendMessage(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnClearTerminal(WPARAM wParam, LPARAM lParam);

private:
	void LayoutControls(int cx, int cy);
	int EnsureScreen(CString screen);
	void RefreshScreenList(void);
	void RefreshVisibleSections(void);
	void AppendToSection(CString screen, int section, CString text, bool stream);
	void SetPinnedState(CString screen, CString text);
	void SendChatText(void);
	void UpdatePotOddsForCurrentBoard(bool force);
	CString CurrentCommunityCardsText(void);
	CString CalculatePotOddsText(CString hole_cards, CString board_cards, bool use_range, bool reverse, CString label);
	void BuildRangeSelector(void);
	void SetRangeSelectorVisible(bool visible);
	CString RangeLabel(int row, int col);
	bool RangeAllowsOpponentHand(int first_card, int second_card);

	CWnd *_owner;
	CMenu _menu;
	CButton _clear_button;
	CButton _send_button;
	CComboBox _screen_combo;
	CEdit _chat_input;
	CEdit _hole_cards_input;
	CStatic _title;
	CStatic _hole_cards_label;
	CStatic _range_label;
	CButton _range_buttons[169];
	CStatic _section_labels[kChatTerminalSectionCount];
	CEdit _sections[kChatTerminalSectionCount];
	std::vector<SChatTerminalScreen> _screens;
	int _active_screen;
	bool _attach_left;
	bool _layout_ready;
	bool _pot_odds_enabled;
	bool _implied_pot_odds_enabled;
	bool _reverse_implied_odds_enabled;
	bool _range_selector_visible;
	bool _range_enabled[169];
	CString _last_pot_odds_board;
};

extern CChatTerminalWindow *p_chat_terminal;

void ChatTerminalAppend(int section, CString text);
void ChatTerminalStream(int section, CString text);
void ChatTerminalAppendToScreen(CString screen, int section, CString text);
void ChatTerminalStreamToScreen(CString screen, int section, CString text);
void ChatTerminalClear(void);
void ChatTerminalClearScreen(CString screen);

#endif // INC_CHAT_TERMINAL_WINDOW_H
