//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Decimal Splitting preferences page. Reads/writes the shared
//   "decimal_split_fields"/"fields" setting (same one Vision's Settings > Fields
//   edits), so the decimal-split field list is global across all three apps.
//
//******************************************************************************

#include "stdafx.h"

#include "DialogSAPrefs25.h"
#include "OpenHoldem.h"
#include "CAutoOcr.h"
#include "..\CTablemap\CTablemapDB.h"
#include "SAPrefsSubDlg.h"

// Canonical numeric field types that can use decimal splitting (mirrors Vision).
static const char *kDecimalFieldTypes[] = {
	"balance", "pot", "bet", "stack", "call", "raise", "blinds", "ante"
};

IMPLEMENT_DYNAMIC(CDlgSAPrefs25, CSAPrefsSubDlg)

CDlgSAPrefs25::CDlgSAPrefs25(CWnd* pParent /*=NULL*/)
		: CSAPrefsSubDlg(CDlgSAPrefs25::IDD, pParent)
{
}

CDlgSAPrefs25::~CDlgSAPrefs25()
{
}

void CDlgSAPrefs25::DoDataExchange(CDataExchange* pDX)
{
	CSAPrefsSubDlg::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_DECIMAL_FIELDS_LIST, m_fields);
}

BEGIN_MESSAGE_MAP(CDlgSAPrefs25, CSAPrefsSubDlg)
END_MESSAGE_MAP()

BOOL CDlgSAPrefs25::OnInitDialog()
{
	CSAPrefsSubDlg::OnInitDialog();

	for (int i = 0; i < (int)(sizeof(kDecimalFieldTypes) / sizeof(kDecimalFieldTypes[0])); ++i) {
		m_field_types.push_back(CString(kDecimalFieldTypes[i]));
		m_fields.AddString(kDecimalFieldTypes[i]);
	}
	if (p_tablemap_db != NULL) {
		std::vector<CString> selected;
		p_tablemap_db->GetSettingArray("decimal_split_fields", "fields", &selected);
		for (size_t i = 0; i < selected.size(); ++i)
			for (int j = 0; j < (int)m_field_types.size(); ++j)
				if (m_field_types[j].CompareNoCase(selected[i]) == 0) { m_fields.SetSel(j, TRUE); break; }
	}

	return TRUE;
}

void CDlgSAPrefs25::OnOK()
{
	if (p_tablemap_db != NULL) {
		std::vector<CString> chosen;
		for (int j = 0; j < (int)m_field_types.size(); ++j)
			if (m_fields.GetSel(j) > 0) chosen.push_back(m_field_types[j]);
		p_tablemap_db->SetSettingArray("decimal_split_fields", "fields", chosen);
		// Re-read into the running AutoOcr engine so it takes effect without a restart.
		AutoOcr()->LoadModelSettings();
	}

	CSAPrefsSubDlg::OnOK();
}
