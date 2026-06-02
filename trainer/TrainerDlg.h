#pragma once

#include "resource.h"
#include "TablemapRegions.h"
#include "TrainerOcr.h"
#include <vector>

class CTrainerServer;
class CTrainerWebWindow;
class CScreenshotView;

// Borderless, click-through, top-most popup used as a hover magnifier: paints a
// cv::Mat scaled by a factor next to the mouse. Created once, then shown/hidden.
class CZoomPopup : public CWnd {
public:
	CZoomPopup();
	virtual ~CZoomPopup();
	BOOL EnsureCreated(CWnd *owner);
	// Show 'bgr' magnified by 'factor', anchored near the given screen point.
	void ShowImage(const cv::Mat &bgr, int factor, CPoint screen_anchor);
	void HidePopup();

protected:
	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC *pDC);
	DECLARE_MESSAGE_MAP()

private:
	cv::Mat _bgra;   // 32bpp copy for painting (DWORD-aligned rows)
};

class CTrainerDlg : public CDialog {
public:
	CTrainerDlg(CWnd *pParent = NULL);
	virtual ~CTrainerDlg();
	enum { IDD = IDD_TRAINER_DIALOG };

protected:
	virtual void DoDataExchange(CDataExchange *pDX);
	virtual BOOL OnInitDialog();
	afx_msg void OnBnClickedLoadTm();
	afx_msg void OnBnClickedSelectModel();
	afx_msg void OnSelchangeTransform();
	afx_msg void OnChangeSplitMargin();
	afx_msg void OnBnClickedDecimalSplit();
	afx_msg void OnBnClickedConnect();
	afx_msg LRESULT OnSetCapture(WPARAM wParam, LPARAM lParam);
	afx_msg void OnBnClickedOpenTable();
	afx_msg void OnBnClickedClearTraining();
	afx_msg void OnTimer(UINT_PTR nIDEvent);
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg void OnMouseLeave();
	afx_msg LRESULT OnAttachWindow(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnRegionSelected(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnOpenFonts(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnCaptureFonts(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnRecognizeAll(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnExitSizeMove(WPARAM wParam, LPARAM lParam);
	afx_msg void OnDestroy();
	DECLARE_MESSAGE_MAP()

private:
	void SetStatus(const CString &text);
	void PopulateModeCombos();
	bool DoLoadTablemap(const CString &path);
	void AttachToWindow(HWND target);
	void RestoreLastSession();
	void SaveOcrSettings();
	void LoadOcrSettings();
	STrainerOcrSettings ReadSettings();
	// Push the desktop "Transform" combo (AutoOcr0/AutoOcr1) into the shared
	// engine selection. Pass recognize=true to also re-run all table rows.
	void ApplyTransformSelection(bool recognize);
	void CaptureTick();
	// Start/stop/toggle sample capture (action: 1/2/3; 0 = query only).
	// Returns 1 if capturing afterwards, else 0. Used by the web Start/Stop button.
	int SetCapture(int action);
	void UpdatePreview();
	void UpdateDecimalSplitControls();   // enable/disable + clear the split-result boxes
	void DrawMatToStatic(int ctrl_id, const cv::Mat &bgr);
	void ClearPreview();

	// OCR settings controls
	CComboBox m_transform;
	CComboBox m_matchMode;
	CEdit m_threshold, m_charSpacing, m_ocrResult, m_splitMargin;
	CSpinButtonCtrl m_thresholdSpin, m_charSpacingSpin, m_splitMarginSpin;

	std::vector<STrainerRegion> _regions;
	std::vector<std::vector<BYTE> > _last;
	std::vector<std::vector<BYTE> > _committed;
	std::vector<bool> _have_baseline;

	HWND _attached;
	bool _capturing;
	HHOOK _mouse_hook;

	HBITMAP _frame;        // latest captured client bitmap (owned)
	int _frame_w, _frame_h;
	int _selected;         // selected region index, -1 = none

	// Hover-magnifier for the scraped-region preview (IDC_SCRAPE_PREVIEW).
	cv::Mat _scrape_img;   // the exact image fed to OCR (raw if preprocessing off)
	CZoomPopup _zoom;
	bool _tracking_mouse;  // WM_MOUSELEAVE requested?

	void OpenFontsWindow();
	void CaptureFontsForEditor();

	CTrainerServer *_server;
	CTrainerWebWindow *_web;
	CTrainerWebWindow *_fonts_web;
	CScreenshotView *_screenshot;
	CTrainerOcr _ocr;
	HICON _icon;

	static CTrainerDlg *s_instance;
	static LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam);
};
