//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: see DlgTwoClicks.h
//
//******************************************************************************

#include "stdafx.h"
#include "OpenScrape.h"
#include "DlgTwoClicks.h"

#include "OpenScrapeDoc.h"
#include "..\CTablemap\CTablemapDB.h"

// DB setting-keys (fielded by the loaded tablemap name). These MUST match the
// keys Hiss reads in CTwoSuccessiveClicks.
static const CString kKeyText1 = "two_successive_clicks_text";
static const CString kKeyText2 = "two_successive_clicks_text2";
static const CString kKeyEn1   = "two_successive_clicks_en1";
static const CString kKeyEn2   = "two_successive_clicks_en2";
static const CString kKeyDelay = "two_successive_clicks_delay";
static const int     kDefaultDelayMs = 200;

IMPLEMENT_DYNAMIC(CDlgTwoClicks, CDialog)

BEGIN_MESSAGE_MAP(CDlgTwoClicks, CDialog)
END_MESSAGE_MAP()

CDlgTwoClicks::CDlgTwoClicks(CWnd* pParent /*=NULL*/)
  : CDialog(IDD_TWO_CLICKS, pParent) {
}

CDlgTwoClicks::~CDlgTwoClicks() {
}

void CDlgTwoClicks::DoDataExchange(CDataExchange* pDX) {
  CDialog::DoDataExchange(pDX);
  DDX_Control(pDX, IDC_TC_TEXT1, m_Text1);
  DDX_Control(pDX, IDC_TC_TEXT2, m_Text2);
  DDX_Control(pDX, IDC_TC_DELAY, m_Delay);
  DDX_Control(pDX, IDC_TC_EN1,   m_En1);
  DDX_Control(pDX, IDC_TC_EN2,   m_En2);
}

CString CDlgTwoClicks::CurrentTablemapName() {
  COpenScrapeDoc* pDoc = COpenScrapeDoc::GetDocument();
  CString name = (pDoc != NULL) ? pDoc->m_tm_name : CString("");
  name.Trim();
  if (name.IsEmpty()) name = "default";
  return name;
}

BOOL CDlgTwoClicks::OnInitDialog() {
  CDialog::OnInitDialog();
  if (p_tablemap_db == NULL) return TRUE;
  CString field = CurrentTablemapName();
  CString text1 = p_tablemap_db->GetSettingString(kKeyText1, field);
  CString text2 = p_tablemap_db->GetSettingString(kKeyText2, field);
  CString en1s  = p_tablemap_db->GetSettingString(kKeyEn1,   field);
  CString en2s  = p_tablemap_db->GetSettingString(kKeyEn2,   field);
  CString delay = p_tablemap_db->GetSettingString(kKeyDelay, field);
  // Default enable to ON when there is saved text but the flag was never stored.
  bool en1 = en1s.IsEmpty() ? !text1.IsEmpty() : (en1s == "1");
  bool en2 = en2s.IsEmpty() ? !text2.IsEmpty() : (en2s == "1");
  int d = atoi(delay.GetString());
  if (d <= 0) d = kDefaultDelayMs;
  CString delay_string;
  delay_string.Format("%d", d);
  m_Text1.SetWindowText(text1);
  m_Text2.SetWindowText(text2);
  m_Delay.SetWindowText(delay_string);
  m_En1.SetCheck(en1 ? BST_CHECKED : BST_UNCHECKED);
  m_En2.SetCheck(en2 ? BST_CHECKED : BST_UNCHECKED);
  return TRUE;
}

void CDlgTwoClicks::OnOK() {
  if (p_tablemap_db != NULL) {
    CString field = CurrentTablemapName();
    CString text1, text2, delay_string;
    m_Text1.GetWindowText(text1);
    m_Text2.GetWindowText(text2);
    m_Delay.GetWindowText(delay_string);
    text1.Trim();
    text2.Trim();
    int d = atoi(delay_string.GetString());
    if (d < 0) d = 0;
    CString clean_delay;
    clean_delay.Format("%d", d);
    bool en1 = (m_En1.GetCheck() == BST_CHECKED);
    bool en2 = (m_En2.GetCheck() == BST_CHECKED);
    p_tablemap_db->SetSettingString(kKeyText1, field, text1);
    p_tablemap_db->SetSettingString(kKeyText2, field, text2);
    p_tablemap_db->SetSettingString(kKeyEn1,   field, en1 ? "1" : "0");
    p_tablemap_db->SetSettingString(kKeyEn2,   field, en2 ? "1" : "0");
    p_tablemap_db->SetSettingString(kKeyDelay, field, clean_delay);
  }
  CDialog::OnOK();
}
