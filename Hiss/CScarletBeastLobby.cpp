//******************************************************************************
// Scarlet Beast embedded lobby — a WebView2-hosted poker.scarletbeast.com lobby
// inside Hiss. The operator selects tables/tournaments here; the multi-instance
// manager then spawns a Hiss per joined table. Falls back to the browser if the
// WebView2 runtime/environment is unavailable.
//******************************************************************************

#include "stdafx.h"
#include "CScarletBeastLobby.h"

#include <wrl.h>
#include "WebView2.h"

// Static loader for the installed Evergreen WebView2 runtime (x86 / Win32).
#pragma comment(lib, "C:\\www\\openholdembot_old\\Hiss\\webview2\\build\\native\\x86\\WebView2LoaderStatic.lib")

using namespace Microsoft::WRL;

static void SB_OpenInBrowser() {
  ::ShellExecute(NULL, _T("open"), _T("https://poker.scarletbeast.com/"), NULL, NULL, SW_SHOWNORMAL);
}

// Top-level frame window that hosts the WebView2 control.
class CSBLobbyWnd : public CFrameWnd {
 public:
  CSBLobbyWnd() {}
  void Boot();
  void Resize();
 protected:
  afx_msg void OnSize(UINT nType, int cx, int cy);
  afx_msg void OnDestroy();
  // CFrameWnd auto-deletes itself here (delete this). We must clear the global
  // owning pointer so a later "Open Lobby" doesn't dereference freed memory.
  virtual void PostNcDestroy();
  DECLARE_MESSAGE_MAP()
 private:
  ComPtr<ICoreWebView2Controller> _controller;
  ComPtr<ICoreWebView2> _webview;
};

BEGIN_MESSAGE_MAP(CSBLobbyWnd, CFrameWnd)
  ON_WM_SIZE()
  ON_WM_DESTROY()
END_MESSAGE_MAP()

void CSBLobbyWnd::Resize() {
  if (_controller != nullptr) {
    RECT rc;
    GetClientRect(&rc);
    _controller->put_Bounds(rc);
  }
}

void CSBLobbyWnd::OnSize(UINT nType, int cx, int cy) {
  CFrameWnd::OnSize(nType, cx, cy);
  Resize();
}

void CSBLobbyWnd::OnDestroy() {
  if (_controller != nullptr) {
    _controller->Close();
    _controller = nullptr;
  }
  CFrameWnd::OnDestroy();
}

void CSBLobbyWnd::Boot() {
  HWND hwnd = m_hWnd;
  HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, nullptr, nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [hwnd, this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result) || env == nullptr) {
              SB_OpenInBrowser();
              return result;
            }
            env->CreateCoreWebView2Controller(
                hwnd,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this](HRESULT r2, ICoreWebView2Controller* controller) -> HRESULT {
                      if (FAILED(r2) || controller == nullptr) {
                        SB_OpenInBrowser();
                        return r2;
                      }
                      _controller = controller;
                      _controller->get_CoreWebView2(&_webview);
                      Resize();
                      if (_webview != nullptr) {
                        _webview->Navigate(L"https://poker.scarletbeast.com/");
                      }
                      return S_OK;
                    })
                    .Get());
            return S_OK;
          })
          .Get());
  if (FAILED(hr)) {
    SB_OpenInBrowser();
  }
}

static CSBLobbyWnd* g_lobby = NULL;

void CSBLobbyWnd::PostNcDestroy() {
  // The window object is about to be deleted by the framework. Drop the global
  // reference first so SB_ShowLobby() never touches a dangling pointer.
  if (g_lobby == this) g_lobby = NULL;
  CFrameWnd::PostNcDestroy();  // deletes this
}

void SB_ShowLobby() {
  // WebView2 is COM-based; make sure COM is up on this (UI) thread.
  ::CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

  // Treat a non-null-but-destroyed window as gone (belt-and-suspenders alongside
  // PostNcDestroy clearing g_lobby).
  if (g_lobby != NULL && !::IsWindow(g_lobby->GetSafeHwnd())) {
    g_lobby = NULL;
  }
  if (g_lobby == NULL || g_lobby->GetSafeHwnd() == NULL) {
    g_lobby = new CSBLobbyWnd();
    if (!g_lobby->Create(NULL, _T("Scarlet Beast Lobby"), WS_OVERLAPPEDWINDOW,
                         CRect(80, 80, 1180, 860))) {
      delete g_lobby;
      g_lobby = NULL;
      SB_OpenInBrowser();
      return;
    }
    g_lobby->Boot();
  }
  g_lobby->ShowWindow(SW_SHOW);
  g_lobby->SetForegroundWindow();
}
