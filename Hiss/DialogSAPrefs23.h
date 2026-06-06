//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: OCR preferences page (AutoOcr0 / AutoOcr1 model selection).
//
//******************************************************************************

#ifndef INC_DIALOGSAPREFS23_H
#define INC_DIALOGSAPREFS23_H

#include "resource.h"
#include "afxwin.h"

#include "SAPrefsDialog.h"

// CDlgSAPrefs23 dialog - OCR model selection for AutoOcr0 / AutoOcr1.

class CDlgSAPrefs23 : public CSAPrefsSubDlg
{
	DECLARE_DYNAMIC(CDlgSAPrefs23)

public:
	CDlgSAPrefs23(CWnd* pParent = NULL);   // standard constructor
	virtual ~CDlgSAPrefs23();

protected:
	virtual void DoDataExchange(CDataExchange* pDX);	// DDX/DDV support
	virtual BOOL OnInitDialog();
	virtual void OnOK();
	afx_msg void OnBnClickedBrowse0();
	afx_msg void OnBnClickedBrowse1();
	afx_msg void OnBnClickedBrowse2();

	enum { IDD = IDD_SAPREFS23 };
	CEdit m_model0, m_model1, m_model2;
	CButton m_dcorr0, m_dcorr1, m_dcorr2;

	void BrowseForModel(CEdit *target);

	DECLARE_MESSAGE_MAP()
};

#endif //INC_DIALOGSAPREFS23_H
