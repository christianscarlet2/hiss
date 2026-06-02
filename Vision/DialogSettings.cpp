//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Settings dialog implementation. See DialogSettings.h.
//
//******************************************************************************

#include "stdafx.h"
#include "OpenScrape.h"
#include "DialogSettings.h"
#include "../CTablemap/CTablemapDB.h"

// Canonical list of numeric field types that can use decimal splitting.
// (Global setting; applies to every tablemap.)
static const char *kDecimalFieldTypes[] = {
	"balance", "pot", "bet", "stack", "call", "raise", "blinds", "ante"
};

// Default model when none is configured yet.
static const char *kDefaultModelPath = "tessdata\\eng.traineddata";

IMPLEMENT_DYNAMIC(CDlgSettings, CDialog)

CDlgSettings::CDlgSettings(CWnd* pParent /*=NULL*/)
	: CDialog(CDlgSettings::IDD, pParent)
{
}

CDlgSettings::~CDlgSettings()
{
}

void CDlgSettings::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_SETTINGS_GROUPS, m_groups);
	DDX_Control(pDX, IDC_EDIT_A0_MODEL, m_a0_path);
	DDX_Control(pDX, IDC_EDIT_A1_MODEL, m_a1_path);
	DDX_Control(pDX, IDC_LST_DECIMAL_FIELDS, m_decimal);
}

BEGIN_MESSAGE_MAP(CDlgSettings, CDialog)
	ON_LBN_SELCHANGE(IDC_SETTINGS_GROUPS, &CDlgSettings::OnSelchangeGroups)
	ON_BN_CLICKED(IDOK, &CDlgSettings::OnBnClickedOk)
	ON_BN_CLICKED(IDC_BTN_A0_BROWSE, &CDlgSettings::OnBrowseA0)
	ON_BN_CLICKED(IDC_BTN_A1_BROWSE, &CDlgSettings::OnBrowseA1)
END_MESSAGE_MAP()

void CDlgSettings::BrowseModel(CEdit *edit)
{
	// Start in the current value's folder, else the tessdata folder.
	CString current;
	edit->GetWindowText(current);
	CString initialDir;
	int slash = current.ReverseFind('\\');
	if (slash > 0) {
		initialDir = current.Left(slash);
	} else {
		initialDir = "tessdata";
	}

	CFileDialog dlg(TRUE, "traineddata", NULL, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
		"Tesseract models (*.traineddata)|*.traineddata|All files (*.*)|*.*||", this);
	dlg.m_ofn.lpstrInitialDir = initialDir;
	if (dlg.DoModal() == IDOK) {
		edit->SetWindowText(dlg.GetPathName());
	}
}

void CDlgSettings::OnBrowseA0() { BrowseModel(&m_a0_path); }
void CDlgSettings::OnBrowseA1() { BrowseModel(&m_a1_path); }

BOOL CDlgSettings::OnInitDialog()
{
	CDialog::OnInitDialog();

	// Left group list.
	m_groups.AddString("General");
	m_groups.AddString("Fields");
	m_groups.AddString("Advanced");
	m_groups.SetCurSel(0);

	// Decimal-split field types.
	for (int i = 0; i < (int)(sizeof(kDecimalFieldTypes) / sizeof(kDecimalFieldTypes[0])); ++i) {
		m_field_types.push_back(CString(kDecimalFieldTypes[i]));
		m_decimal.AddString(kDecimalFieldTypes[i]);
	}

	// Load saved values from the database.
	if (p_tablemap_db != NULL) {
		CString a0 = p_tablemap_db->GetSettingString("ocr_models", "a0");
		CString a1 = p_tablemap_db->GetSettingString("ocr_models", "a1");
		m_a0_path.SetWindowText(a0.IsEmpty() ? CString(kDefaultModelPath) : a0);
		m_a1_path.SetWindowText(a1.IsEmpty() ? CString(kDefaultModelPath) : a1);

		std::vector<CString> selected;
		p_tablemap_db->GetSettingArray("decimal_split_fields", "fields", &selected);
		for (size_t i = 0; i < selected.size(); ++i) {
			for (int j = 0; j < (int)m_field_types.size(); ++j) {
				if (m_field_types[j].CompareNoCase(selected[i]) == 0) {
					m_decimal.SetSel(j, TRUE);
					break;
				}
			}
		}

		CString no_pre = p_tablemap_db->GetSettingString("ocr_options", "no_preprocess");
		CString no_wl  = p_tablemap_db->GetSettingString("ocr_options", "no_whitelist");
		CheckDlgButton(IDC_CHK_NO_PREPROCESS, no_pre == "1" ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(IDC_CHK_NO_WHITELIST,  no_wl  == "1" ? BST_CHECKED : BST_UNCHECKED);
	} else {
		m_a0_path.SetWindowText(kDefaultModelPath);
		m_a1_path.SetWindowText(kDefaultModelPath);
	}

	ShowPage(0);
	return TRUE;
}

void CDlgSettings::ShowPage(int index)
{
	int general[]  = { IDC_SET_GENERAL_TEXT };
	int fields[]   = { IDC_SET_FIELDS_A0LBL, IDC_EDIT_A0_MODEL, IDC_BTN_A0_BROWSE,
		IDC_SET_FIELDS_A1LBL, IDC_EDIT_A1_MODEL, IDC_BTN_A1_BROWSE,
		IDC_CHK_NO_PREPROCESS, IDC_CHK_NO_WHITELIST,
		IDC_SET_FIELDS_DECLBL, IDC_LST_DECIMAL_FIELDS };
	int advanced[] = { IDC_SET_ADVANCED_TEXT };

	const int *show = NULL; int show_n = 0;
	CString title;
	if (index == 1)      { show = fields;   show_n = sizeof(fields) / sizeof(int);   title = "Fields"; }
	else if (index == 2) { show = advanced; show_n = sizeof(advanced) / sizeof(int); title = "Advanced"; }
	else                 { show = general;  show_n = sizeof(general) / sizeof(int);  title = "General"; }

	int all[] = { IDC_SET_GENERAL_TEXT,
		IDC_SET_FIELDS_A0LBL, IDC_EDIT_A0_MODEL, IDC_BTN_A0_BROWSE,
		IDC_SET_FIELDS_A1LBL, IDC_EDIT_A1_MODEL, IDC_BTN_A1_BROWSE,
		IDC_CHK_NO_PREPROCESS, IDC_CHK_NO_WHITELIST,
		IDC_SET_FIELDS_DECLBL, IDC_LST_DECIMAL_FIELDS,
		IDC_SET_ADVANCED_TEXT };
	for (int i = 0; i < (int)(sizeof(all) / sizeof(int)); ++i) {
		CWnd *w = GetDlgItem(all[i]);
		if (w != NULL) w->ShowWindow(SW_HIDE);
	}
	for (int i = 0; i < show_n; ++i) {
		CWnd *w = GetDlgItem(show[i]);
		if (w != NULL) w->ShowWindow(SW_SHOW);
	}
	SetDlgItemText(IDC_SET_PAGE_TITLE, title);
}

void CDlgSettings::OnSelchangeGroups()
{
	ShowPage(m_groups.GetCurSel());
}

void CDlgSettings::OnBnClickedOk()
{
	if (p_tablemap_db != NULL) {
		CString a0, a1;
		m_a0_path.GetWindowText(a0);
		m_a1_path.GetWindowText(a1);
		p_tablemap_db->SetSettingString("ocr_models", "a0", a0);
		p_tablemap_db->SetSettingString("ocr_models", "a1", a1);

		std::vector<CString> chosen;
		for (int j = 0; j < (int)m_field_types.size(); ++j) {
			if (m_decimal.GetSel(j) > 0) {
				chosen.push_back(m_field_types[j]);
			}
		}
		p_tablemap_db->SetSettingArray("decimal_split_fields", "fields", chosen);

		p_tablemap_db->SetSettingString("ocr_options", "no_preprocess",
			IsDlgButtonChecked(IDC_CHK_NO_PREPROCESS) == BST_CHECKED ? "1" : "0");
		p_tablemap_db->SetSettingString("ocr_options", "no_whitelist",
			IsDlgButtonChecked(IDC_CHK_NO_WHITELIST) == BST_CHECKED ? "1" : "0");
	}
	OnOK();
}
