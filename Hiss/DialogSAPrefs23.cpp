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

// DialogSAPrefs23.cpp : implementation file
//

#include "stdafx.h"

#include "DialogSAPrefs23.h"
#include "OpenHoldem.h"
#include "CAutoOcr.h"
#include "..\CTablemap\CTablemapDB.h"
#include "SAPrefsSubDlg.h"

// The AutoOcr model specs live in the shared settings table (same place CAutoOcr reads
// them from on startup): key "autoocr0"/"autoocr1", field "model".
static const char *kOcrKeys[2] = { "autoocr0", "autoocr1" };

// CDlgSAPrefs23 dialog

IMPLEMENT_DYNAMIC(CDlgSAPrefs23, CSAPrefsSubDlg)

CDlgSAPrefs23::CDlgSAPrefs23(CWnd* pParent /*=NULL*/)
		: CSAPrefsSubDlg(CDlgSAPrefs23::IDD, pParent)
{
}

CDlgSAPrefs23::~CDlgSAPrefs23()
{
}

void CDlgSAPrefs23::DoDataExchange(CDataExchange* pDX)
{
	CSAPrefsSubDlg::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_OCR_MODEL0, m_model0);
	DDX_Control(pDX, IDC_OCR_MODEL1, m_model1);
}

BEGIN_MESSAGE_MAP(CDlgSAPrefs23, CSAPrefsSubDlg)
	ON_BN_CLICKED(IDC_OCR_BROWSE0, &CDlgSAPrefs23::OnBnClickedBrowse0)
	ON_BN_CLICKED(IDC_OCR_BROWSE1, &CDlgSAPrefs23::OnBnClickedBrowse1)
END_MESSAGE_MAP()

// CDlgSAPrefs23 message handlers
BOOL CDlgSAPrefs23::OnInitDialog()
{
	CSAPrefsSubDlg::OnInitDialog();

	if (p_tablemap_db != NULL) {
		m_model0.SetWindowText(p_tablemap_db->GetSettingString(kOcrKeys[0], "model"));
		m_model1.SetWindowText(p_tablemap_db->GetSettingString(kOcrKeys[1], "model"));
	}

	return TRUE;  // return TRUE unless you set the focus to a control
}

void CDlgSAPrefs23::OnOK()
{
	if (p_tablemap_db != NULL) {
		CString m0, m1;
		m_model0.GetWindowText(m0);
		m_model1.GetWindowText(m1);
		m0.Trim();
		m1.Trim();
		p_tablemap_db->SetSettingString(kOcrKeys[0], "model", m0);
		p_tablemap_db->SetSettingString(kOcrKeys[1], "model", m1);
		// Re-read into the running AutoOcr engine so the change takes effect without
		// a restart (the next OCR call re-Inits Tesseract with the new model).
		AutoOcr()->LoadModelSettings();
	}

	CSAPrefsSubDlg::OnOK();
}

// Let the user pick a .traineddata file; store its full path in `target`.
void CDlgSAPrefs23::BrowseForModel(CEdit *target)
{
	CString current;
	target->GetWindowText(current);
	CFileDialog dlg(TRUE, "traineddata", current,
		OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
		"Tesseract model (*.traineddata;*.checkpoint)|*.traineddata;*.checkpoint|All files (*.*)|*.*||", this);
	if (dlg.DoModal() == IDOK) {
		target->SetWindowText(dlg.GetPathName());
	}
}

void CDlgSAPrefs23::OnBnClickedBrowse0() { BrowseForModel(&m_model0); }
void CDlgSAPrefs23::OnBnClickedBrowse1() { BrowseForModel(&m_model1); }
