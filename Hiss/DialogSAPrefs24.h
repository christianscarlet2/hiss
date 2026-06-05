//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Parallel Workers preferences page (CPUs x workers-per-CPU for the
//   in-process OCR/hashing worker pool, shared via the hiss settings table).
//
//******************************************************************************

#ifndef INC_DIALOGSAPREFS24_H
#define INC_DIALOGSAPREFS24_H

#include "resource.h"
#include "afxwin.h"
#include "SAPrefsDialog.h"

class CDlgSAPrefs24 : public CSAPrefsSubDlg
{
	DECLARE_DYNAMIC(CDlgSAPrefs24)

public:
	CDlgSAPrefs24(CWnd* pParent = NULL);
	virtual ~CDlgSAPrefs24();

protected:
	virtual void DoDataExchange(CDataExchange* pDX);
	virtual BOOL OnInitDialog();
	virtual void OnOK();

	enum { IDD = IDD_SAPREFS24 };
	CEdit m_num_cpus, m_workers_per_cpu;

	DECLARE_MESSAGE_MAP()
};

#endif //INC_DIALOGSAPREFS24_H
