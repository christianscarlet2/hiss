//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Settings dialog. Left group list (General / Fields / Advanced) with
//   a swappable content area. Reads/writes the `settings` table in the hiss DB.
//
//******************************************************************************

#pragma once
#include "afxwin.h"
#include <vector>
#include "resource.h"

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
	afx_msg void OnSelchangeGroups();
	afx_msg void OnBnClickedOk();
	afx_msg void OnBrowseA0();
	afx_msg void OnBrowseA1();
	DECLARE_MESSAGE_MAP()

private:
	CListBox  m_groups;
	CEdit     m_a0_path;   // path to the A0 .traineddata model
	CEdit     m_a1_path;   // path to the A1 .traineddata model
	CListBox  m_decimal;
	std::vector<CString> m_field_types;   // canonical decimal-split field types

	void ShowPage(int index);             // 0=General 1=Fields 2=Advanced
	void BrowseModel(CEdit *edit);        // file-open for a *.traineddata model
};
