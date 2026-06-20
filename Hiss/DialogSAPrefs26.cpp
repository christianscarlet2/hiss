//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Audio preferences page - microphone device selection for the
//   voice-feedback loop (used by the AIL improvement loop).
//
//******************************************************************************

// DialogSAPrefs26.cpp : implementation file
//

#include "stdafx.h"

#include "DialogSAPrefs26.h"
#include "OpenHoldem.h"
#include "..\CTablemap\CTablemapDB.h"
#include "SAPrefsSubDlg.h"

#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

// The selected microphone is stored in the shared settings table (the same place
// the OCR page and CAutoOcr use): key "audio", field "mic_device". We store the
// device NAME (not its index) because wave-in indices are not stable across
// reboots / device hot-plugs; voice_feedback.py matches a name substring. The
// special value "" / "default" means "use the system default input".
static const char *kAudioKey = "audio";
static const char *kMicField = "mic_device";

// CDlgSAPrefs26 dialog

IMPLEMENT_DYNAMIC(CDlgSAPrefs26, CSAPrefsSubDlg)

CDlgSAPrefs26::CDlgSAPrefs26(CWnd* pParent /*=NULL*/)
		: CSAPrefsSubDlg(CDlgSAPrefs26::IDD, pParent)
{
}

CDlgSAPrefs26::~CDlgSAPrefs26()
{
}

void CDlgSAPrefs26::DoDataExchange(CDataExchange* pDX)
{
	CSAPrefsSubDlg::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_AUDIO_MIC, m_mic_device);
}

BEGIN_MESSAGE_MAP(CDlgSAPrefs26, CSAPrefsSubDlg)
END_MESSAGE_MAP()

// Enumerate the system's recording (wave-in) devices into the combo. Item 0 is
// the system default; the rest are the named capture devices.
void CDlgSAPrefs26::PopulateDevices()
{
	m_mic_device.ResetContent();
	m_mic_device.AddString("(System default microphone)");
	UINT n = waveInGetNumDevs();
	for (UINT i = 0; i < n; ++i) {
		WAVEINCAPS caps;
		if (waveInGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
			// caps.szPname is a TCHAR[MAXPNAMELEN]; in this ANSI build it is char[].
			m_mic_device.AddString(caps.szPname);
		}
	}
}

// CDlgSAPrefs26 message handlers
BOOL CDlgSAPrefs26::OnInitDialog()
{
	CSAPrefsSubDlg::OnInitDialog();

	PopulateDevices();

	// Pre-select the saved device (by name match); fall back to the default item.
	int sel = 0;
	if (p_tablemap_db != NULL) {
		CString saved = p_tablemap_db->GetSettingString(kAudioKey, kMicField);
		saved.Trim();
		if (!saved.IsEmpty()) {
			int idx = m_mic_device.FindStringExact(-1, saved);
			if (idx == CB_ERR) {
				// Saved device not currently present: show it anyway so the user sees
				// what is configured, and select it.
				idx = m_mic_device.AddString(saved);
			}
			if (idx != CB_ERR) sel = idx;
		}
	}
	m_mic_device.SetCurSel(sel);

	return TRUE;  // return TRUE unless you set the focus to a control
}

void CDlgSAPrefs26::OnOK()
{
	if (p_tablemap_db != NULL) {
		int sel = m_mic_device.GetCurSel();
		CString value;   // empty => system default
		if (sel > 0) {   // item 0 is the "(System default microphone)" sentinel
			m_mic_device.GetLBText(sel, value);
			value.Trim();
		}
		p_tablemap_db->SetSettingString(kAudioKey, kMicField, value);
	}

	CSAPrefsSubDlg::OnOK();
}
