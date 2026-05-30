#pragma once

#include "TablemapRegions.h"
#include <vector>

// Notifies the owner (TrainerDlg) when a region is clicked. wParam = region index.
#define WM_TRAINER_REGION_SELECTED   (WM_APP + 5)

// A standalone window that shows the live screenshot of the connected window,
// scaled to fit, with the balance regions outlined. Clicking a region selects it.
class CScreenshotView : public CWnd {
	DECLARE_DYNAMIC(CScreenshotView)
	DECLARE_MESSAGE_MAP()

public:
	CScreenshotView();
	virtual ~CScreenshotView();

	BOOL Create(CWnd *owner);
	// Hands the latest captured frame + the regions to draw. Takes a copy of the
	// bitmap so the caller can keep using/deleting its own.
	void UpdateFrame(HBITMAP src, int src_w, int src_h, const std::vector<STrainerRegion> &regions, int selected);

protected:
	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC *pDC);
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);

private:
	void ClearBitmap();
	bool ViewToSource(CPoint pt, int *sx, int *sy);

	CWnd *_owner;
	HBITMAP _frame;     // our private copy of the latest capture
	int _src_w, _src_h;
	std::vector<STrainerRegion> _regions;
	int _selected;
	// last-drawn placement of the scaled image (client coords) for hit-testing
	RECT _draw_rect;
};
