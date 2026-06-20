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
//   voice-feedback loop (mic -> whisper -> postgres -> AIL). The chosen device
//   is persisted to the shared settings table (key "audio", field "mic_device")
//   so voice_feedback.py picks it up with no command-line --device argument.
//
//******************************************************************************

#ifndef INC_DIALOGSAPREFS26_H
#define INC_DIALOGSAPREFS26_H

#include "resource.h"
#include "afxwin.h"

#include "SAPrefsDialog.h"

// CDlgSAPrefs26 dialog - microphone device selection for voice feedback.

class CDlgSAPrefs26 : public CSAPrefsSubDlg
{
	DECLARE_DYNAMIC(CDlgSAPrefs26)

public:
	CDlgSAPrefs26(CWnd* pParent = NULL);   // standard constructor
	virtual ~CDlgSAPrefs26();

protected:
	virtual void DoDataExchange(CDataExchange* pDX);	// DDX/DDV support
	virtual BOOL OnInitDialog();
	virtual void OnOK();

	enum { IDD = IDD_SAPREFS26 };
	CComboBox m_mic_device;   // list of recording (wave-in) devices

	// Fill m_mic_device with the system's wave-in devices (index 0 = "Default").
	void PopulateDevices();

	DECLARE_MESSAGE_MAP()
};

#endif //INC_DIALOGSAPREFS26_H
