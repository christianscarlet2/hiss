//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//
//******************************************************************************
//
// Purpose: Transparent always-on-top HUD overlay drawn over the attached
//   scrcpy table window. Per-seat PokerTracker-4 stat boxes are shown only for
//   seats that have PT4 hands. The overlay is COMPLETELY click-through by
//   default (so the bot can click the table); the user can drag / right-click a
//   box only while holding CTRL. Boxes can be locked, persist per-tablemap, and
//   be recalibrated in one shot via a Claude/MCP screenshot read.
//
//******************************************************************************

#ifndef INC_HUD_OVERLAY_WINDOW_H
#define INC_HUD_OVERLAY_WINDOW_H

#include <afxwin.h>
#include "..\Shared\MagicNumbers\MagicNumbers.h"

// Heartbeat -> overlay: apply g_hud_positions_json (posted by Claude/MCP).
#define WM_HUD_APPLY_POSITIONS (WM_APP + 71)

class CHudOverlayWindow : public CWnd {
	DECLARE_DYNAMIC(CHudOverlayWindow)
	DECLARE_MESSAGE_MAP()

public:
	CHudOverlayWindow();
	virtual ~CHudOverlayWindow();

	BOOL Create(CWnd *owner);
	// Called from the MainFrm UI timer: cover the scrcpy client area, show/hide,
	// and repaint with the latest stats.
	void TrackTableWindow();
	// CTRL-opacity only, cheap enough to run on the 50ms timer so holding CTRL feels instant
	// instead of lagging up to a 200ms tick (and lagging DIFFERENTLY per instance).
	void RefreshCtrlAlpha();
	// Anchor rect the RED decision is drawn above (the hero's HUD box, or the hero seat's
	// default position when that seat has no box yet). Public because the decision now lives
	// in its OWN window (CHudActionWindow) which is the same size as this one, so the rect
	// carries over unchanged. Returns false when there is no hero chair to anchor on.
	bool HeroAnchorRect(int client_w, int client_h, CRect *out);

protected:
	afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC *pDC);
	afx_msg LRESULT OnNcHitTest(CPoint point);
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnRButtonUp(UINT nFlags, CPoint point);
	afx_msg LRESULT OnApplyPositions(WPARAM wParam, LPARAM lParam);

private:
	void EnsureLoaded();
	void LoadPositions();
	void SavePositions();
	void ParsePositions(const CString &json);
	void DefaultFractionForChair(int chair, int nchairs, int client_w, int client_h, double *fx, double *fy) const;
	void ComputeBoxRects(int client_w, int client_h);
	int  BoxIndexAtClientPoint(CPoint pt) const;
	CString PositionsField() const;

	CWnd  *_owner;
	// Per-seat anchor as a fraction (0..1) of the client area. <0 => use default.
	double _fx[kMaxNumberOfPlayers];
	double _fy[kMaxNumberOfPlayers];
	bool   _locked;
	bool   _loaded;
	CString _loaded_for;        // tablemap filename the positions were loaded for
	int    _drag_chair;         // chair currently being dragged, or -1
	CPoint _drag_grab;          // grab offset within the dragged box
	// Filled each paint, used for hit-testing (client coordinates).
	CRect  _box_rect[kMaxNumberOfPlayers];
	bool   _box_visible[kMaxNumberOfPlayers];
};

extern CHudOverlayWindow *p_hud_overlay_window;

// Separate always-SOLID overlay carrying ONLY the bot's RED decision (action + table name +
// brain detail lines). It exists because a layered window's alpha is GLOBAL: while the decision
// shared CHudOverlayWindow, making the action readable forced the whole window -- HUD tiles
// included -- to ~92% opacity, which buried the scrcpy table behind opaque stat boxes. Splitting
// the two lets the tiles stay faint (12%) permanently while the action still reads crisply.
// [Emrald: "when the ACTION shows up ... leave the HUD tiles transparent"]
class CHudActionWindow : public CWnd {
	DECLARE_DYNAMIC(CHudActionWindow)
	DECLARE_MESSAGE_MAP()

public:
	CHudActionWindow();
	virtual ~CHudActionWindow();

	BOOL Create(CWnd *owner);
	// Same 200ms MainFrm tick as the HUD: cover the scrcpy client area while a decision is
	// trailing, and stay hidden the rest of the time.
	void TrackTableWindow();

protected:
	afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC *pDC);
	// COPY-HAND-NUMBER TILE. The window stays hit-test-transparent EVERYWHERE except this one small
	// rect, so the bot's own autoplayer clicks still reach the felt untouched. [Emrald]
	afx_msg LRESULT OnNcHitTest(CPoint point);
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);

private:
	CWnd *_owner;
	CRect _copy_rect;        // client-space rect of the CURRENT hand number (empty when not drawn)
	CRect _copy_rect_prev;   // client-space rect of the PREVIOUS hand number, shown in parentheses
	DWORD _copied_tick;      // for the brief "COPIED" confirmation
	int   _copied_which;     // 0 = none, 1 = current, 2 = previous (so only the clicked row flashes)
	CString _prev_hand;      // the hand before this one -- the one you usually want to investigate
	CString _seen_hand;      // last hand number observed, used to roll _prev_hand on change
};

extern CHudActionWindow *p_hud_action_window;

#endif // INC_HUD_OVERLAY_WINDOW_H
