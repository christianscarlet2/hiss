#ifndef INC_CHAT_TERMINAL_WINDOW_H
#define INC_CHAT_TERMINAL_WINDOW_H

#include <afxcmn.h>
#include <vector>
#include "CVTermPane.h"   // libvterm-backed terminal control

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

class CChatTerminalWindow;

class COpponentRangeWindow : public CWnd {
	DECLARE_DYNAMIC(COpponentRangeWindow)
	DECLARE_MESSAGE_MAP()

public:
	COpponentRangeWindow();
	virtual ~COpponentRangeWindow();

	BOOL Create(CWnd *owner, CChatTerminalWindow *terminal);
	void AttachToOwner(bool force = false);

protected:
	afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
	afx_msg void OnSize(UINT nType, int cx, int cy);
	afx_msg void OnPaint();
	afx_msg void OnMoving(UINT fwSide, LPRECT pRect);
	afx_msg void OnClose();
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg void OnVpipChanged();

private:
	void LayoutControls(int cx, int cy);
	void DrawRangeSelector(CDC *dc);
	int RangeCellFromPoint(CPoint point) const;
	int RangeRowTriangleFromPoint(CPoint point) const;
	int RangeColumnTriangleFromPoint(CPoint point) const;

	CWnd *_owner;
	CChatTerminalWindow *_terminal;
	CStatic _title;
	CStatic _vpip_label;
	CEdit _vpip_input;
	bool _layout_ready;
	bool _range_dragging;
	bool _range_drag_value;
	int _last_drag_range_index;
	CRect _range_grid_rect;
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
	// Public wrapper so other modules (e.g. the timing-tells engine) can pin a
	// persistent line into a screen's State section.
	void PinStatePublic(CString screen, CString text) { SetPinnedState(screen, text); }
	// Thread-safe version (posts to the UI thread). Use this from worker/heartbeat
	// threads to update the fixed (pinned) State block in place.
	void SetPinnedStateAsync(CString screen, CString text);
	void AttachToOwner(bool force = false);
	void MaybeUpdatePotOddsFromTableState(void);
	void ShowOpponentRangeWindow(bool visible);
	bool IsOpponentRangeWindowVisible(void) const;
	bool RangeAllowsOpponentHand(int first_card, int second_card);

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
	afx_msg void OnFeatureLoadHudProfile();
	afx_msg void OnFeatureOpponentRange();
	afx_msg void OnUpdateFeatureOpponentRange(CCmdUI *pCmdUI);
	afx_msg void OnHoleCardsChanged();
	afx_msg LRESULT OnAppendMessage(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnClearTerminal(WPARAM wParam, LPARAM lParam);
	afx_msg HBRUSH OnCtlColor(CDC *pDC, CWnd *pWnd, UINT nCtlColor);
	afx_msg BOOL OnEraseBkgnd(CDC *pDC);
	virtual BOOL PreTranslateMessage(MSG *pMsg);   // chat prompt: Enter sends, Up/Down recall

private:
	friend class COpponentRangeWindow;

	void LayoutControls(int cx, int cy);
	int EnsureScreen(CString screen);
	void RefreshScreenList(void);
	void RefreshVisibleSections(void);
	void RefreshSection(int section);
	void AppendToSection(CString screen, int section, CString text, bool stream);
	void SetPinnedState(CString screen, CString text);
	void SendChatText(void);
	void UpdatePotOddsForCurrentBoard(bool force);
	CString CurrentCommunityCardsText(void);
	CString CalculatePotOddsText(CString hole_cards, CString board_cards, bool use_range, bool reverse, CString label);
	void BuildRangeSelector(void);
	bool IsRangeCellEnabled(int index) const;
	void SetRangeCell(int index, bool enabled);
	void SetRangeRow(int row, bool enabled);
	void SetRangeColumn(int col, bool enabled);
	void RefreshRangeOdds(void);
	void ApplyVpipRange(CString vpip_text);
	int RangeComboCount(int row, int col) const;
	int RangeScore(int row, int col) const;
	CString RangeLabel(int row, int col);

	CWnd *_owner;
	CMenu _menu;
	CButton _clear_button;
	CButton _send_button;
	CComboBox _screen_combo;
	CEdit _chat_input;
	CEdit _hole_cards_input;
	CStatic _title;
	CStatic _hole_cards_label;
	CStatic _section_labels[kChatTerminalSectionCount];
	// libvterm-backed terminal consoles: full VT engine, scrollback, ANSI colour.
	CVTermPane _sections[kChatTerminalSectionCount];
	CFont _terminal_font;     // monospace font for the labels / input boxes
	CBrush _term_bg_brush;    // dark background brush for the input boxes / labels
	// Command prompt history (Up/Down recall on the chat input).
	std::vector<CString> _input_history;
	int _history_index;
	COpponentRangeWindow _opponent_range_window;
	std::vector<SChatTerminalScreen> _screens;
	int _active_screen;
	bool _attach_left;
	bool _layout_ready;
	bool _pot_odds_enabled;
	bool _implied_pot_odds_enabled;
	bool _reverse_implied_odds_enabled;
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
