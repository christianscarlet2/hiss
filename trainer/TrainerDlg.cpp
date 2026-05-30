#include "stdafx.h"
#include "trainer.h"
#include "TrainerDlg.h"
#include "TrainerServer.h"
#include "TrainerWebWindow.h"
#include "SampleStore.h"
#include "WindowCapture.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

#define WM_TRAINER_ATTACH   (WM_APP + 1)
#define TRAINER_TIMER       1
const UINT kCaptureIntervalMs = 150;
const int  kMinOcrConfidence = 55;   // below this, "ignore bad scrapes" drops it

CTrainerDlg *CTrainerDlg::s_instance = NULL;

CTrainerDlg::CTrainerDlg(CWnd *pParent)
	: CDialog(CTrainerDlg::IDD, pParent)
{
	_attached = NULL;
	_capturing = false;
	_mouse_hook = NULL;
	_server = NULL;
	_web = NULL;
	_ocr = NULL;
	_ocr_ready = false;
	_icon = NULL;
}

CTrainerDlg::~CTrainerDlg()
{
}

void CTrainerDlg::DoDataExchange(CDataExchange *pDX)
{
	CDialog::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CTrainerDlg, CDialog)
	ON_BN_CLICKED(IDC_LOAD_TM, &CTrainerDlg::OnBnClickedLoadTm)
	ON_BN_CLICKED(IDC_CONNECT, &CTrainerDlg::OnBnClickedConnect)
	ON_BN_CLICKED(IDC_STARTSTOP, &CTrainerDlg::OnBnClickedStartStop)
	ON_BN_CLICKED(IDC_OPEN_TABLE, &CTrainerDlg::OnBnClickedOpenTable)
	ON_WM_TIMER()
	ON_WM_DESTROY()
	ON_MESSAGE(WM_TRAINER_ATTACH, &CTrainerDlg::OnAttachWindow)
END_MESSAGE_MAP()

BOOL CTrainerDlg::OnInitDialog()
{
	CDialog::OnInitDialog();

	_icon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
	if (_icon != NULL) {
		SetIcon(_icon, TRUE);
		SetIcon(_icon, FALSE);
	}

	// In-memory sample store shared with the HTTP server.
	if (p_sample_store == NULL) {
		p_sample_store = new CSampleStore();
	}

	// Local HTTP server for the WebView2 table.
	_server = new CTrainerServer();
	if (_server->Start()) {
		p_trainer_server = _server;
	}

	// Tesseract with the user's model for OCR pre-fill.
	_ocr = new tesseract::TessBaseAPI();
	_ocr_ready = (_ocr->Init("tessdata", "my_model") == 0);

	CString status;
	status.Format("Ready. OCR model: %s. Server: %s.\nLoad a tablemap, then Connect and click a window.",
		_ocr_ready ? "my_model" : "FAILED (check tessdata\\my_model)",
		(_server != NULL && _server->port() != 0) ? "running" : "FAILED");
	SetStatus(status);
	return TRUE;
}

void CTrainerDlg::SetStatus(const CString &text)
{
	SetDlgItemText(IDC_STATUS, text);
}

void CTrainerDlg::OnBnClickedLoadTm()
{
	CFileDialog dlg(TRUE, "tm", NULL, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
		"Tablemaps (*.tm)|*.tm|All files (*.*)|*.*||", this);
	if (dlg.DoModal() != IDOK) {
		return;
	}
	_regions.clear();
	if (!LoadBalanceRegions(dlg.GetPathName(), &_regions)) {
		SetStatus("Failed to read the tablemap file.");
		return;
	}
	_last.assign(_regions.size(), std::vector<BYTE>());
	_committed.assign(_regions.size(), std::vector<BYTE>());
	_have_baseline.assign(_regions.size(), false);

	CString status;
	status.Format("Loaded %d balance region(s) (p0balance..p8balance).", (int)_regions.size());
	SetStatus(status);
}

LRESULT CALLBACK CTrainerDlg::LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam)
{
	if (code == HC_ACTION && wParam == WM_LBUTTONDOWN && s_instance != NULL) {
		MSLLHOOKSTRUCT *info = (MSLLHOOKSTRUCT *)lParam;
		POINT pt = info->pt;
		// Disarm and hand off to the dialog; swallow this click.
		HHOOK hook = s_instance->_mouse_hook;
		s_instance->_mouse_hook = NULL;
		if (hook != NULL) {
			UnhookWindowsHookEx(hook);
		}
		s_instance->PostMessage(WM_TRAINER_ATTACH, (WPARAM)pt.x, (LPARAM)pt.y);
		return 1;   // do not deliver the click to the clicked window
	}
	return CallNextHookEx(NULL, code, wParam, lParam);
}

void CTrainerDlg::OnBnClickedConnect()
{
	if (_mouse_hook != NULL) {
		return;   // already arming
	}
	s_instance = this;
	_mouse_hook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, AfxGetInstanceHandle(), 0);
	if (_mouse_hook == NULL) {
		SetStatus("Could not install the mouse hook.");
		return;
	}
	SetStatus("Click anywhere on the table window you want to capture...");
}

LRESULT CTrainerDlg::OnAttachWindow(WPARAM wParam, LPARAM lParam)
{
	POINT pt;
	pt.x = (int)wParam;
	pt.y = (int)lParam;

	HWND target = ::WindowFromPoint(pt);
	if (target != NULL) {
		target = ::GetAncestor(target, GA_ROOT);
	}
	// Don't attach to our own windows.
	HWND self_root = ::GetAncestor(GetSafeHwnd(), GA_ROOT);
	HWND web_root = (_web != NULL && ::IsWindow(_web->GetSafeHwnd())) ? ::GetAncestor(_web->GetSafeHwnd(), GA_ROOT) : NULL;
	if (target == NULL || target == self_root || (web_root != NULL && target == web_root)) {
		SetStatus("Ignored a click on the trainer's own window. Click Connect and pick the table.");
		return 0;
	}

	_attached = target;
	// Reset per-region baselines so the first stable values aren't treated as changes.
	for (size_t i = 0; i < _have_baseline.size(); ++i) {
		_have_baseline[i] = false;
	}

	char title[256] = { 0 };
	::GetWindowTextA(target, title, sizeof(title) - 1);
	CString status;
	status.Format("Connected to: %s\nClick Start to begin capturing.", title);
	SetStatus(status);
	return 0;
}

void CTrainerDlg::OnBnClickedStartStop()
{
	if (_attached == NULL || !::IsWindow(_attached)) {
		SetStatus("Connect to a window first.");
		return;
	}
	if (_regions.empty()) {
		SetStatus("Load a tablemap with balance regions first.");
		return;
	}
	_capturing = !_capturing;
	if (_capturing) {
		SetTimer(TRAINER_TIMER, kCaptureIntervalMs, NULL);
		SetDlgItemText(IDC_STARTSTOP, "Stop");
		SetStatus("Capturing... change balances on the table to generate samples.");
	} else {
		KillTimer(TRAINER_TIMER);
		SetDlgItemText(IDC_STARTSTOP, "Start");
		SetStatus("Stopped.");
	}
}

void CTrainerDlg::OnBnClickedOpenTable()
{
	if (_server == NULL || _server->port() == 0) {
		SetStatus("HTTP server is not running.");
		return;
	}
	if (_web == NULL) {
		_web = new CTrainerWebWindow();
		_web->Create(this, _server->port());
	} else {
		_web->ShowWindow(SW_SHOWNORMAL);
		_web->SetForegroundWindow();
		_web->NavigateToTrainer(_server->port());
	}
}

void CTrainerDlg::OnTimer(UINT_PTR nIDEvent)
{
	if (nIDEvent == TRAINER_TIMER) {
		CaptureTick();
	}
	CDialog::OnTimer(nIDEvent);
}

// Crop region rect from a 32-bit DIB bitmap into a top-down BGRA buffer.
static bool CropRegionBgra(HBITMAP bmp, int bmpW, int bmpH, RECT r,
	std::vector<BYTE> *out, int *outW, int *outH)
{
	if (r.left < 0) r.left = 0;
	if (r.top < 0) r.top = 0;
	if (r.right >= bmpW) r.right = bmpW - 1;
	if (r.bottom >= bmpH) r.bottom = bmpH - 1;
	int w = r.right - r.left + 1;
	int h = r.bottom - r.top + 1;
	if (w < 2 || h < 2) {
		return false;
	}

	HDC hdcScreen = CreateDC("DISPLAY", NULL, NULL, NULL);
	HDC hdc_src = CreateCompatibleDC(hdcScreen);
	HBITMAP old_src = (HBITMAP)SelectObject(hdc_src, bmp);
	HDC hdc_crop = CreateCompatibleDC(hdcScreen);
	HBITMAP crop_bmp = CreateCompatibleBitmap(hdcScreen, w, h);
	HBITMAP old_crop = (HBITMAP)SelectObject(hdc_crop, crop_bmp);
	BitBlt(hdc_crop, 0, 0, w, h, hdc_src, r.left, r.top, SRCCOPY);

	out->assign((size_t)w * h * 4, 0);
	BITMAPINFO bi;
	ZeroMemory(&bi, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	GetDIBits(hdc_crop, crop_bmp, 0, h, &(*out)[0], &bi, DIB_RGB_COLORS);

	SelectObject(hdc_crop, old_crop);
	DeleteObject(crop_bmp);
	DeleteDC(hdc_crop);
	SelectObject(hdc_src, old_src);
	DeleteDC(hdc_src);
	DeleteDC(hdcScreen);

	*outW = w;
	*outH = h;
	return true;
}

bool CTrainerDlg::LooksBlank(const cv::Mat &bgr)
{
	cv::Mat gray;
	cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
	double mn = 0, mx = 0;
	cv::minMaxLoc(gray, &mn, &mx);
	return (mx - mn) < 25.0;   // near-uniform = empty seat / no value
}

CStringA CTrainerDlg::OcrCrop(const cv::Mat &bgr, int *mean_conf)
{
	*mean_conf = 0;
	if (!_ocr_ready || _ocr == NULL) {
		return "";
	}
	cv::Mat gray;
	cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
	_ocr->SetPageSegMode(tesseract::PSM_SINGLE_LINE);
	_ocr->SetImage(gray.data, gray.cols, gray.rows, 1, (int)gray.step);
	_ocr->Recognize(0);
	char *text = _ocr->GetUTF8Text();
	CStringA result(text != NULL ? text : "");
	if (text != NULL) {
		delete[] text;
	}
	*mean_conf = _ocr->MeanTextConf();
	result.Trim(" \r\n\t");
	return result;
}

void CTrainerDlg::CaptureTick()
{
	if (_attached == NULL || !::IsWindow(_attached)) {
		KillTimer(TRAINER_TIMER);
		_capturing = false;
		SetDlgItemText(IDC_STARTSTOP, "Start");
		SetStatus("The attached window is gone. Reconnect.");
		return;
	}

	int bw = 0, bh = 0;
	HBITMAP bmp = CaptureCompositedClientBitmap(_attached, &bw, &bh);
	if (bmp == NULL || bw <= 0 || bh <= 0) {
		if (bmp != NULL) DeleteObject(bmp);
		return;
	}

	bool ignore_bad = (IsDlgButtonChecked(IDC_IGNORE_BAD) == BST_CHECKED);

	for (size_t i = 0; i < _regions.size(); ++i) {
		std::vector<BYTE> cur;
		int w = 0, h = 0;
		if (!CropRegionBgra(bmp, bw, bh, _regions[i].rect, &cur, &w, &h)) {
			continue;
		}

		if (!_have_baseline[i]) {
			_last[i] = cur;
			_committed[i] = cur;
			_have_baseline[i] = true;
			continue;
		}
		if (cur != _last[i]) {
			_last[i] = cur;     // still changing/animating — wait for it to settle
			continue;
		}
		// Stable this tick. Snapshot only if it differs from the last committed value.
		if (cur == _committed[i]) {
			continue;
		}
		_committed[i] = cur;

		// Build a BGR Mat for OCR + PNG encoding.
		cv::Mat bgra((int)h, (int)w, CV_8UC4, &cur[0]);
		cv::Mat bgr;
		cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);

		int conf = 0;
		CStringA guess = OcrCrop(bgr, &conf);

		if (ignore_bad) {
			if (guess.IsEmpty() || conf < kMinOcrConfidence || LooksBlank(bgr)) {
				continue;   // drop bad scrape
			}
		}

		std::vector<unsigned char> png;
		std::vector<int> params;
		params.push_back(cv::IMWRITE_PNG_COMPRESSION);
		params.push_back(3);
		if (!cv::imencode(".png", bgr, png, params) || png.empty()) {
			continue;
		}
		if (p_sample_store != NULL) {
			p_sample_store->Add(CStringA(_regions[i].name), png, guess);
		}
	}

	DeleteObject(bmp);
}

void CTrainerDlg::OnDestroy()
{
	if (_capturing) {
		KillTimer(TRAINER_TIMER);
		_capturing = false;
	}
	if (_mouse_hook != NULL) {
		UnhookWindowsHookEx(_mouse_hook);
		_mouse_hook = NULL;
	}
	if (_web != NULL) {
		_web->DestroyWindow();
		delete _web;
		_web = NULL;
	}
	if (_server != NULL) {
		_server->Stop();
		p_trainer_server = NULL;
		delete _server;
		_server = NULL;
	}
	if (_ocr != NULL) {
		_ocr->End();
		delete _ocr;
		_ocr = NULL;
	}
	if (p_sample_store != NULL) {
		delete p_sample_store;
		p_sample_store = NULL;
	}
	s_instance = NULL;
	CDialog::OnDestroy();
}
