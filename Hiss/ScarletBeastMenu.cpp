//******************************************************************************
// Scarlet Beast menu — runtime-appended menu on the Hiss main window.
//
// Adds a "Scarlet Beast" menu with: toggle the server scrape source, set the
// API key / table id from the clipboard (no dialog needed), test the connection
// (GraphQL), and open the lobby. Kept in its own file so MainFrm.cpp barely
// changes. The command IDs + handler declarations live in MainFrm.h.
//******************************************************************************

#include "stdafx.h"
#include "MainFrm.h"
#include "CScarletBeast.h"
#include "CScarletBeastLobby.h"
#include <string>

// Read trimmed text from the clipboard (empty on failure). ANSI/MBCS build.
static CString SB_ClipboardText() {
  CString out;
  if (!::OpenClipboard(NULL)) return out;
  HANDLE h = ::GetClipboardData(CF_TEXT);
  if (h != NULL) {
    LPCSTR p = (LPCSTR)::GlobalLock(h);
    if (p != NULL) { out = p; ::GlobalUnlock(h); }
  }
  ::CloseClipboard();
  out.Trim();
  return out;
}

// CString (ANSI) -> std::wstring for the wide-string config setters.
static std::wstring SB_ToW(const CString& s) {
  int n = ::MultiByteToWideChar(CP_ACP, 0, (LPCSTR)s, s.GetLength(), NULL, 0);
  std::wstring w(n, 0);
  if (n) ::MultiByteToWideChar(CP_ACP, 0, (LPCSTR)s, s.GetLength(), &w[0], n);
  return w;
}

// Posted from OnCreate so the menu is fully loaded before we append to it.
LRESULT CMainFrame::OnSbInitMenu(WPARAM, LPARAM) {
  AppendScarletBeastMenu();
  return 0;
}

void CMainFrame::AppendScarletBeastMenu() {
  CMenu* pTop = GetMenu();
  if (pTop == NULL) return;
  CMenu sb;
  sb.CreatePopupMenu();
  sb.AppendMenu(MF_STRING, IDM_SB_CONNECT, _T("Connect to poker.scarletbeast.com (toggle)"));
  sb.AppendMenu(MF_STRING, IDM_SB_SETKEY, _T("Set API Key from Clipboard"));
  sb.AppendMenu(MF_STRING, IDM_SB_SETTABLE, _T("Set Table ID from Clipboard"));
  sb.AppendMenu(MF_SEPARATOR, 0, _T(""));
  sb.AppendMenu(MF_STRING, IDM_SB_GOOGLE, _T("Link Google Account (console + networkedin)"));
  sb.AppendMenu(MF_STRING, IDM_SB_TEST, _T("Test Connection"));
  sb.AppendMenu(MF_STRING, IDM_SB_LOBBY, _T("Open Lobby"));
  sb.AppendMenu(MF_SEPARATOR, 0, _T(""));
  sb.AppendMenu(MF_STRING, IDM_SB_AUTOREBUY, _T("Auto Rebuy when busted (toggle)"));
  sb.AppendMenu(MF_STRING, IDM_SB_LOADHUD, _T("Load PT4 HUD file..."));
  pTop->AppendMenu(MF_POPUP, (UINT_PTR)sb.Detach(), _T("Scarlet Beast"));
  // Reflect current toggle states.
  if (p_scarlet_beast != NULL && p_scarlet_beast->ScrapeFromServer()) {
    pTop->CheckMenuItem(IDM_SB_CONNECT, MF_BYCOMMAND | MF_CHECKED);
  }
  if (p_scarlet_beast != NULL && p_scarlet_beast->AutoRebuy()) {
    pTop->CheckMenuItem(IDM_SB_AUTOREBUY, MF_BYCOMMAND | MF_CHECKED);
  }
  DrawMenuBar();
}

void CMainFrame::OnSbConnect() {
  if (p_scarlet_beast == NULL) return;
  bool now = !p_scarlet_beast->ScrapeFromServer();
  p_scarlet_beast->SetScrapeFromServer(now);
  CMenu* m = GetMenu();
  if (m != NULL) m->CheckMenuItem(IDM_SB_CONNECT, MF_BYCOMMAND | (now ? MF_CHECKED : MF_UNCHECKED));
  AfxMessageBox(now ? _T("Scarlet Beast: scraping FROM poker.scarletbeast.com is now ON.")
                    : _T("Scarlet Beast: server scrape OFF (using screen scraping)."));
}

void CMainFrame::OnSbSetKey() {
  if (p_scarlet_beast == NULL) return;
  CString k = SB_ClipboardText();
  if (k.IsEmpty()) { AfxMessageBox(_T("Clipboard is empty. Copy your sbp_... machine key first.")); return; }
  p_scarlet_beast->SetApiKey(SB_ToW(k));
  AfxMessageBox(_T("Scarlet Beast: API key saved from clipboard."));
}

void CMainFrame::OnSbSetTable() {
  if (p_scarlet_beast == NULL) return;
  CString t = SB_ClipboardText();
  int id = _ttoi(t);
  if (id <= 0) { AfxMessageBox(_T("Clipboard does not contain a valid table id.")); return; }
  p_scarlet_beast->SetTableId(id);
  CString msg;
  msg.Format(_T("Scarlet Beast: table id set to %d."), id);
  AfxMessageBox(msg);
}

void CMainFrame::OnSbTest() {
  if (p_scarlet_beast == NULL) return;
  std::string stats = p_scarlet_beast->GraphQL("{ marketplaceStats { count totalHands avgBbPer100 } }");
  int code = p_scarlet_beast->LastStatus();
  CString msg;
  msg.Format(_T("Scarlet Beast test\nGraphQL HTTP %d\n\n%s"), code, stats.c_str());
  AfxMessageBox(msg);
}

void CMainFrame::OnSbGoogle() {
  // Reuses the estate SSO Google flow: signing in binds this identity across
  // poker.scarletbeast.com/console and /networkedin in the browser session.
  ::ShellExecute(NULL, _T("open"),
    _T("https://poker.scarletbeast.com/auth/google/redirect?return=/console"),
    NULL, NULL, SW_SHOWNORMAL);
  AfxMessageBox(_T("Scarlet Beast: opening Google sign-in. Once linked, your account is bound to /console and /networkedin."));
}

void CMainFrame::OnSbLobby() {
  // Embedded WebView2 lobby (falls back to the browser if unavailable).
  SB_ShowLobby();
}

void CMainFrame::OnSbLoadHud() {
  if (p_scarlet_beast == NULL) return;
  CFileDialog dlg(TRUE, _T("pt4hud"), NULL, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
    _T("PokerTracker 4 HUD (*.pt4hud)|*.pt4hud|All Files (*.*)|*.*||"));
  if (dlg.DoModal() != IDOK) return;
  std::wstring wpath = SB_ToW(dlg.GetPathName());
  std::string result = p_scarlet_beast->UploadPt4Hud(wpath);
  AfxMessageBox(CString(result.c_str()));
}

void CMainFrame::OnSbAutoRebuy() {
  if (p_scarlet_beast == NULL) return;
  bool now = !p_scarlet_beast->AutoRebuy();
  p_scarlet_beast->SetAutoRebuy(now);
  CMenu* m = GetMenu();
  if (m != NULL) m->CheckMenuItem(IDM_SB_AUTOREBUY, MF_BYCOMMAND | (now ? MF_CHECKED : MF_UNCHECKED));
  AfxMessageBox(now ? _T("Scarlet Beast: Auto Rebuy is ON. When your stack busts to zero, ")
                      _T("the bot will buy back in a full stack from your bankroll.")
                    : _T("Scarlet Beast: Auto Rebuy is OFF. You will sit out when busted."));
}
