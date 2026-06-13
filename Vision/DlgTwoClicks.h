//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Per-tablemap editor for the "two successive clicks" auto-action.
//   Two match texts (each with an enable checkbox) and a delay in ms, saved to
//   the loaded tablemap in the shared postgres DB. Hiss reads these at runtime
//   and clicks the two rectangles of the "two_successive_clicks" region whenever
//   the "two_successive_clicks_label" OCR matches an enabled text (case-insensitive).
//
//******************************************************************************

#pragma once

#include "resource.h"

class CDlgTwoClicks : public CDialog {
  DECLARE_DYNAMIC(CDlgTwoClicks)
 public:
  CDlgTwoClicks(CWnd* pParent = NULL);
  virtual ~CDlgTwoClicks();
  enum { IDD = IDD_TWO_CLICKS };
 protected:
  virtual void DoDataExchange(CDataExchange* pDX);
  virtual BOOL OnInitDialog();
  virtual void OnOK();
  DECLARE_MESSAGE_MAP()
 private:
  CString CurrentTablemapName();
  CEdit   m_Text1, m_Text2, m_Delay;
  CButton m_En1, m_En2;
};
