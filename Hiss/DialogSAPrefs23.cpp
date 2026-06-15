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
static const char *kOcrKeys[4] = { "autoocr0", "autoocr1", "autoocr2", "autoocr3" };

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
	DDX_Control(pDX, IDC_OCR_MODEL2, m_model2);
	DDX_Control(pDX, IDC_OCR_MODEL3, m_model3);
	DDX_Control(pDX, IDC_OCR_DCORR0, m_dcorr0);
	DDX_Control(pDX, IDC_OCR_DCORR1, m_dcorr1);
	DDX_Control(pDX, IDC_OCR_DCORR2, m_dcorr2);
	DDX_Control(pDX, IDC_OCR_DCORR3, m_dcorr3);
	DDX_Control(pDX, IDC_OCR_MEMORY, m_ocr_memory);
}

BEGIN_MESSAGE_MAP(CDlgSAPrefs23, CSAPrefsSubDlg)
	ON_BN_CLICKED(IDC_OCR_BROWSE0, &CDlgSAPrefs23::OnBnClickedBrowse0)
	ON_BN_CLICKED(IDC_OCR_BROWSE1, &CDlgSAPrefs23::OnBnClickedBrowse1)
	ON_BN_CLICKED(IDC_OCR_BROWSE2, &CDlgSAPrefs23::OnBnClickedBrowse2)
	ON_BN_CLICKED(IDC_OCR_BROWSE3, &CDlgSAPrefs23::OnBnClickedBrowse3)
END_MESSAGE_MAP()

// CDlgSAPrefs23 message handlers
BOOL CDlgSAPrefs23::OnInitDialog()
{
	CSAPrefsSubDlg::OnInitDialog();

	if (p_tablemap_db != NULL) {
		m_model0.SetWindowText(p_tablemap_db->GetSettingString(kOcrKeys[0], "model"));
		m_model1.SetWindowText(p_tablemap_db->GetSettingString(kOcrKeys[1], "model"));
		m_model2.SetWindowText(p_tablemap_db->GetSettingString(kOcrKeys[2], "model"));
		m_model3.SetWindowText(p_tablemap_db->GetSettingString(kOcrKeys[3], "model"));
		m_dcorr0.SetCheck(p_tablemap_db->GetSettingString(kOcrKeys[0], "decimal_correct") == "1" ? BST_CHECKED : BST_UNCHECKED);
		m_dcorr1.SetCheck(p_tablemap_db->GetSettingString(kOcrKeys[1], "decimal_correct") == "1" ? BST_CHECKED : BST_UNCHECKED);
		m_dcorr2.SetCheck(p_tablemap_db->GetSettingString(kOcrKeys[2], "decimal_correct") == "1" ? BST_CHECKED : BST_UNCHECKED);
		m_dcorr3.SetCheck(p_tablemap_db->GetSettingString(kOcrKeys[3], "decimal_correct") == "1" ? BST_CHECKED : BST_UNCHECKED);
		m_ocr_memory.SetCheck(p_tablemap_db->GetSettingString("ocr_memory", "enabled") == "1" ? BST_CHECKED : BST_UNCHECKED);
	}

	return TRUE;  // return TRUE unless you set the focus to a control
}

void CDlgSAPrefs23::OnOK()
{
	if (p_tablemap_db != NULL) {
		CString m0, m1, m2, m3;
		m_model0.GetWindowText(m0);
		m_model1.GetWindowText(m1);
		m_model2.GetWindowText(m2);
		m_model3.GetWindowText(m3);
		m0.Trim();
		m1.Trim();
		m2.Trim();
		m3.Trim();
		p_tablemap_db->SetSettingString(kOcrKeys[0], "model", m0);
		p_tablemap_db->SetSettingString(kOcrKeys[1], "model", m1);
		p_tablemap_db->SetSettingString(kOcrKeys[2], "model", m2);
		p_tablemap_db->SetSettingString(kOcrKeys[3], "model", m3);
		p_tablemap_db->SetSettingString(kOcrKeys[0], "decimal_correct", m_dcorr0.GetCheck() == BST_CHECKED ? "1" : "0");
		p_tablemap_db->SetSettingString(kOcrKeys[1], "decimal_correct", m_dcorr1.GetCheck() == BST_CHECKED ? "1" : "0");
		p_tablemap_db->SetSettingString(kOcrKeys[2], "decimal_correct", m_dcorr2.GetCheck() == BST_CHECKED ? "1" : "0");
		p_tablemap_db->SetSettingString(kOcrKeys[3], "decimal_correct", m_dcorr3.GetCheck() == BST_CHECKED ? "1" : "0");
		p_tablemap_db->SetSettingString("ocr_memory", "enabled", m_ocr_memory.GetCheck() == BST_CHECKED ? "1" : "0");
		// Re-read into the running AutoOcr engine so the change takes effect without
		// a restart (also reloads the OCR-memory toggle into g_ocr_memory).
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
void CDlgSAPrefs23::OnBnClickedBrowse2() { BrowseForModel(&m_model2); }
void CDlgSAPrefs23::OnBnClickedBrowse3() { BrowseForModel(&m_model3); }
