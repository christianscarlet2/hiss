#pragma once

#include "resource.h"
#include "TablemapRegions.h"
#include "TrainerOcr.h"
#include <vector>

class CTrainerServer;
class CTrainerWebWindow;
class CScreenshotView;

class CTrainerDlg : public CDialog {
public:
	CTrainerDlg(CWnd *pParent = NULL);
	virtual ~CTrainerDlg();
	enum { IDD = IDD_TRAINER_DIALOG };

protected:
	virtual void DoDataExchange(CDataExchange *pDX);
	virtual BOOL OnInitDialog();
	afx_msg void OnBnClickedLoadTm();
	afx_msg void OnBnClickedConnect();
	afx_msg void OnBnClickedStartStop();
	afx_msg void OnBnClickedOpenTable();
	afx_msg void OnBnClickedClearTraining();
	afx_msg void OnTimer(UINT_PTR nIDEvent);
	afx_msg LRESULT OnAttachWindow(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnRegionSelected(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnOpenFonts(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnCaptureFonts(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnRecognizeAll(WPARAM wParam, LPARAM lParam);
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
	void CaptureTick();
	void UpdatePreview();
	void DrawMatToStatic(int ctrl_id, const cv::Mat &bgr);
	void ClearPreview();

	// OCR settings controls
	CComboBox m_transform;
	CComboBox m_matchMode;
	CEdit m_threshold, m_cropSize, m_sharpen, m_ocrResult;
	CSpinButtonCtrl m_thresholdSpin, m_cropSpin, m_sharpenSpin;

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
