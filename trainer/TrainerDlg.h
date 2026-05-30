#pragma once

#include "resource.h"
#include "TablemapRegions.h"
#include <vector>

class CTrainerServer;
class CTrainerWebWindow;

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
	afx_msg void OnTimer(UINT_PTR nIDEvent);
	afx_msg LRESULT OnAttachWindow(WPARAM wParam, LPARAM lParam);
	afx_msg void OnDestroy();
	DECLARE_MESSAGE_MAP()

private:
	void SetStatus(const CString &text);
	void CaptureTick();
	CStringA OcrCrop(const cv::Mat &bgr, int *mean_conf);
	static bool LooksBlank(const cv::Mat &bgr);

	std::vector<STrainerRegion> _regions;
	std::vector<std::vector<BYTE> > _last;       // per-region previous pixels
	std::vector<std::vector<BYTE> > _committed;  // per-region last snapshotted
	std::vector<bool> _have_baseline;

	HWND _attached;
	bool _capturing;
	HHOOK _mouse_hook;

	CTrainerServer *_server;
	CTrainerWebWindow *_web;
	tesseract::TessBaseAPI *_ocr;
	bool _ocr_ready;
	HICON _icon;

	static CTrainerDlg *s_instance;
	static LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam);
};
