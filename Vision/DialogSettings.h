//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Modeless Settings dialog. Left group list (General / Fields /
//   AutoOcr0 / AutoOcr1 / Advanced). The AutoOcr pages edit per-transform OCR
//   settings (settings table keys autoocr0/autoocr1) and provide a live OCR
//   preview of the region selected in the scrape view.
//
//******************************************************************************

#pragma once
#include "afxwin.h"
#include <vector>
#include "resource.h"
#include "../trainer/TrainerOcr.h"

class CDlgSettings : public CDialog
{
	DECLARE_DYNAMIC(CDlgSettings)
public:
	CDlgSettings(CWnd* pParent = NULL);
	virtual ~CDlgSettings();
	enum { IDD = IDD_SETTINGS };

protected:
	virtual void DoDataExchange(CDataExchange* pDX);
	virtual BOOL OnInitDialog();
	virtual void PostNcDestroy();
	virtual void OnOK();
	virtual void OnCancel();
	afx_msg void OnSelchangeGroups();
	afx_msg void OnBrowseModel();
	afx_msg void OnLivePollToggled();
	afx_msg void OnNoPreprocessToggled();
	afx_msg void OnTimer(UINT_PTR nIDEvent);
	DECLARE_MESSAGE_MAP()

private:
	CListBox        m_groups;
	CEdit           m_model;
	CEdit           m_threshold;
	CSpinButtonCtrl m_threshold_spin;
	CComboBox       m_mode;
	CListBox        m_decimal;
	std::vector<CString> m_field_types;   // canonical decimal-split field types
	CListBox        m_scrape;             // per-field-type enable list (shared scrape_fields)
	std::vector<CString> m_scrape_types;  // field types the trainer scrapes (balance, name)
	CTrainerOcr     m_ocr;
	CString         m_loaded_model;       // model currently Init'd in m_ocr
	int             m_current_group;      // 0=General 1=Fields 2=AutoOcr0 3=AutoOcr1 4=Advanced

	void ShowPage(int index);
	const char *GroupKey(int group);      // "autoocr0" / "autoocr1" / NULL
	void LoadGroup(int group);            // DB -> controls
	void SaveGroup(int group);            // controls -> DB
	void SaveDecimalFields();
	void SaveScrapeFields();
	void UpdateCharSpacingEnable();
	void RunPreview();
	bool RegionUsesDecimalSplit(const CString &name);
};
