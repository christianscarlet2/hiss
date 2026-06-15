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
	void DefaultFractionForChair(int chair, int nchairs, double *fx, double *fy) const;
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

#endif // INC_HUD_OVERLAY_WINDOW_H
