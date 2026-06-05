//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Parallel Workers preferences page. Reads/writes the shared
//   "parallel_workers" settings (num_cpus, workers_per_cpu) used to size the
//   in-process OCR/hashing worker pool. Same keys Vision's settings use.
//
//******************************************************************************

#include "stdafx.h"

#include "DialogSAPrefs24.h"
#include "OpenHoldem.h"
#include "..\CTablemap\CTablemapDB.h"
#include "..\Shared\ParallelWorkerPool.h"
#include "SAPrefsSubDlg.h"

IMPLEMENT_DYNAMIC(CDlgSAPrefs24, CSAPrefsSubDlg)

CDlgSAPrefs24::CDlgSAPrefs24(CWnd* pParent /*=NULL*/)
		: CSAPrefsSubDlg(CDlgSAPrefs24::IDD, pParent)
{
}

CDlgSAPrefs24::~CDlgSAPrefs24()
{
}

void CDlgSAPrefs24::DoDataExchange(CDataExchange* pDX)
{
	CSAPrefsSubDlg::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_PW_NUM_CPUS, m_num_cpus);
	DDX_Control(pDX, IDC_PW_WORKERS_PER_CPU, m_workers_per_cpu);
}

BEGIN_MESSAGE_MAP(CDlgSAPrefs24, CSAPrefsSubDlg)
END_MESSAGE_MAP()

BOOL CDlgSAPrefs24::OnInitDialog()
{
	CSAPrefsSubDlg::OnInitDialog();

	int cpus = 0, wpc = 1;
	if (p_tablemap_db != NULL) {
		CString c = p_tablemap_db->GetSettingString("parallel_workers", "num_cpus");
		CString w = p_tablemap_db->GetSettingString("parallel_workers", "workers_per_cpu");
		if (!c.IsEmpty()) cpus = atoi(c.GetString());
		if (!w.IsEmpty()) wpc = atoi(w.GetString());
	}
	if (cpus <= 0) cpus = ParallelDefaultCpuCount();
	if (wpc <= 0) wpc = 1;
	SetDlgItemInt(IDC_PW_NUM_CPUS, cpus, FALSE);
	SetDlgItemInt(IDC_PW_WORKERS_PER_CPU, wpc, FALSE);

	return TRUE;
}

void CDlgSAPrefs24::OnOK()
{
	if (p_tablemap_db != NULL) {
		BOOL okc = FALSE, okw = FALSE;
		int cpus = (int)GetDlgItemInt(IDC_PW_NUM_CPUS, &okc, FALSE);
		int wpc = (int)GetDlgItemInt(IDC_PW_WORKERS_PER_CPU, &okw, FALSE);
		if (okc && cpus > 0) {
			CString v; v.Format("%d", cpus);
			p_tablemap_db->SetSettingString("parallel_workers", "num_cpus", v);
		}
		if (okw && wpc > 0) {
			CString v; v.Format("%d", wpc);
			p_tablemap_db->SetSettingString("parallel_workers", "workers_per_cpu", v);
		}
	}

	CSAPrefsSubDlg::OnOK();
}
