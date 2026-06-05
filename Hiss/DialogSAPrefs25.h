//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Decimal Splitting preferences page. Selects which numeric field types
//   are OCR'd with decimal splitting; stored in the shared hiss settings table
//   ("decimal_split_fields"/"fields") used by Hiss, Vision and the trainer.
//
//******************************************************************************

#ifndef INC_DIALOGSAPREFS25_H
#define INC_DIALOGSAPREFS25_H

#include "resource.h"
#include "afxwin.h"
#include "SAPrefsDialog.h"
#include <vector>

class CDlgSAPrefs25 : public CSAPrefsSubDlg
{
	DECLARE_DYNAMIC(CDlgSAPrefs25)

public:
	CDlgSAPrefs25(CWnd* pParent = NULL);
	virtual ~CDlgSAPrefs25();

protected:
	virtual void DoDataExchange(CDataExchange* pDX);
	virtual BOOL OnInitDialog();
	virtual void OnOK();

	enum { IDD = IDD_SAPREFS25 };
	CListBox m_fields;
	std::vector<CString> m_field_types;

	DECLARE_MESSAGE_MAP()
};

#endif //INC_DIALOGSAPREFS25_H
