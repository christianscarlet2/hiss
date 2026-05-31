#include "stdafx.h"
#include "trainer.h"
#include "TrainerDlg.h"
#include "TrainerServer.h"
#include "TrainerWebWindow.h"
#include "ScreenshotView.h"
#include "SampleStore.h"
#include "WindowCapture.h"

using namespace cv;
using namespace tesseract;

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

#define WM_TRAINER_ATTACH   (WM_APP + 1)
#define TRAINER_TIMER       1
const UINT kCaptureIntervalMs = 150;
const int  kMinOcrConfidence = 55;

struct PsmEntry { const char *label; int value; };
static const PsmEntry kPsmModes[] = {
	{ "Single block", PSM_SINGLE_BLOCK },
	{ "Single line", PSM_SINGLE_LINE },
	{ "Single word", PSM_SINGLE_WORD },
	{ "Single char", PSM_SINGLE_CHAR },
	{ "Sparse text", PSM_SPARSE_TEXT },
	{ "Sparse text + OSD", PSM_SPARSE_TEXT_OSD },
	{ "Auto", PSM_AUTO },
	{ "Single column", PSM_SINGLE_COLUMN },
};
static const int kNumPsmModes = sizeof(kPsmModes) / sizeof(kPsmModes[0]);
static const int kDefaultPsmIndex = 7;   // Single column

static const char *kTrainerRegKey = "Software\\Hiss\\Trainer";

static void RegWriteString(const char *name, const CString &value)
{
	HKEY key; DWORD disp;
	if (RegCreateKeyEx(HKEY_CURRENT_USER, kTrainerRegKey, 0, NULL,
		REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &key, &disp) == ERROR_SUCCESS) {
		RegSetValueEx(key, name, 0, REG_SZ, (const BYTE *)value.GetString(), value.GetLength() + 1);
		RegCloseKey(key);
	}
}

static CString RegReadString(const char *name)
{
	CString out;
	HKEY key;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, kTrainerRegKey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
		char buf[1024] = { 0 };
		DWORD size = sizeof(buf), type = 0;
		if (RegQueryValueEx(key, name, NULL, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS && type == REG_SZ) {
			out = buf;
		}
		RegCloseKey(key);
	}
	return out;
}

struct STrainerFindWindow { CString title; CString cls; HWND self; HWND result; };

static BOOL CALLBACK TrainerFindWindowProc(HWND hwnd, LPARAM lparam)
{
	STrainerFindWindow *f = (STrainerFindWindow *)lparam;
	if (hwnd == f->self) return TRUE;
	if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;
	char t[512] = { 0 };
	::GetWindowTextA(hwnd, t, sizeof(t) - 1);
	if (f->title != CString(t)) return TRUE;
	if (!f->cls.IsEmpty()) {
		char c[256] = { 0 };
		::GetClassNameA(hwnd, c, sizeof(c) - 1);
		if (f->cls != CString(c)) return TRUE;
	}
	f->result = hwnd;
	return FALSE;
}

// First visible top-level window matching the saved title (and class, if known).
static HWND FindTopWindow(const CString &title, const CString &cls, HWND self)
{
	if (title.IsEmpty()) return NULL;
	STrainerFindWindow f;
	f.title = title; f.cls = cls; f.self = self; f.result = NULL;
	EnumWindows(TrainerFindWindowProc, (LPARAM)&f);
	return f.result;
}

CTrainerDlg *CTrainerDlg::s_instance = NULL;

CTrainerDlg::CTrainerDlg(CWnd *pParent)
	: CDialog(CTrainerDlg::IDD, pParent)
{
	_attached = NULL;
	_capturing = false;
	_mouse_hook = NULL;
	_frame = NULL;
	_frame_w = _frame_h = 0;
	_selected = -1;
	_server = NULL;
	_web = NULL;
	_screenshot = NULL;
	_icon = NULL;
}

CTrainerDlg::~CTrainerDlg()
{
}

void CTrainerDlg::DoDataExchange(CDataExchange *pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_TRANSFORM, m_transform);
	DDX_Control(pDX, IDC_MATCH_MODE, m_matchMode);
	DDX_Control(pDX, IDC_THRESHOLD, m_threshold);
	DDX_Control(pDX, IDC_THRESHOLD_SPIN, m_thresholdSpin);
	DDX_Control(pDX, IDC_CROP_SIZE, m_cropSize);
	DDX_Control(pDX, IDC_CROP_SPIN, m_cropSpin);
	DDX_Control(pDX, IDC_SHARPEN, m_sharpen);
	DDX_Control(pDX, IDC_SHARPEN_SPIN, m_sharpenSpin);
	DDX_Control(pDX, IDC_OCR_RESULT, m_ocrResult);
}

BEGIN_MESSAGE_MAP(CTrainerDlg, CDialog)
	ON_BN_CLICKED(IDC_LOAD_TM, &CTrainerDlg::OnBnClickedLoadTm)
	ON_BN_CLICKED(IDC_CONNECT, &CTrainerDlg::OnBnClickedConnect)
	ON_BN_CLICKED(IDC_STARTSTOP, &CTrainerDlg::OnBnClickedStartStop)
	ON_BN_CLICKED(IDC_OPEN_TABLE, &CTrainerDlg::OnBnClickedOpenTable)
	ON_BN_CLICKED(IDC_CLEAR_TRAINING, &CTrainerDlg::OnBnClickedClearTraining)
	ON_WM_TIMER()
	ON_WM_DESTROY()
	ON_MESSAGE(WM_TRAINER_ATTACH, &CTrainerDlg::OnAttachWindow)
	ON_MESSAGE(WM_TRAINER_REGION_SELECTED, &CTrainerDlg::OnRegionSelected)
END_MESSAGE_MAP()

void CTrainerDlg::PopulateModeCombos()
{
	m_transform.AddString("AutoOcr0");
	m_transform.AddString("AutoOcr1");
	m_transform.SetCurSel(0);

	for (int i = 0; i < kNumPsmModes; ++i) {
		int item = m_matchMode.AddString(kPsmModes[i].label);
		m_matchMode.SetItemData(item, (DWORD_PTR)kPsmModes[i].value);
	}
	m_matchMode.SetCurSel(kDefaultPsmIndex);
}

BOOL CTrainerDlg::OnInitDialog()
{
	CDialog::OnInitDialog();

	_icon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
	if (_icon != NULL) { SetIcon(_icon, TRUE); SetIcon(_icon, FALSE); }

	if (p_sample_store == NULL) {
		p_sample_store = new CSampleStore();
	}
	_server = new CTrainerServer();
	if (_server->Start()) {
		p_trainer_server = _server;
	}

	bool ocr_ok = _ocr.Init("tessdata", "my_model");

	PopulateModeCombos();

	// Defaults: Use Default OFF, threshold 65.
	CheckDlgButton(IDC_USE_DEFAULT, BST_UNCHECKED);
	m_threshold.SetWindowText("65");
	m_thresholdSpin.SetRange(0, 300);
	m_thresholdSpin.SetPos(65);
	m_thresholdSpin.SetBuddy(&m_threshold);
	CheckDlgButton(IDC_USE_CROP, BST_UNCHECKED);
	m_cropSize.SetWindowText("30");
	m_cropSpin.SetRange(1, 100);
	m_cropSpin.SetPos(30);
	m_cropSpin.SetBuddy(&m_cropSize);
	m_sharpen.SetWindowText("100");
	m_sharpenSpin.SetRange(0, 500);
	m_sharpenSpin.SetPos(100);
	m_sharpenSpin.SetBuddy(&m_sharpen);

	// Restore the OCR settings the user had on last exit (overrides defaults).
	LoadOcrSettings();

	CString status;
	status.Format("Ready. OCR model: %s. Server: %s.\nLoad a tablemap, then Connect and click a window.",
		ocr_ok ? "my_model" : "FAILED (check tessdata\\my_model)",
		(_server != NULL && _server->port() != 0) ? "running" : "FAILED");
	SetStatus(status);

	// Reload the last tablemap and reconnect to the last window if it's open.
	RestoreLastSession();
	return TRUE;
}

void CTrainerDlg::SetStatus(const CString &text)
{
	SetDlgItemText(IDC_STATUS, text);
}

bool CTrainerDlg::DoLoadTablemap(const CString &path)
{
	_regions.clear();
	if (!LoadBalanceRegions(path, &_regions)) {
		SetStatus("Failed to read the tablemap file.");
		return false;
	}
	_last.assign(_regions.size(), std::vector<BYTE>());
	_committed.assign(_regions.size(), std::vector<BYTE>());
	_have_baseline.assign(_regions.size(), false);
	_selected = _regions.empty() ? -1 : 0;

	RegWriteString("last_tablemap", path);

	CString status;
	status.Format("Loaded %d balance region(s) from %s", (int)_regions.size(), path.GetString());
	SetStatus(status);
	return true;
}

void CTrainerDlg::OnBnClickedLoadTm()
{
	CFileDialog dlg(TRUE, "tm", NULL, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
		"Tablemaps (*.tm)|*.tm|All files (*.*)|*.*||", this);
	if (dlg.DoModal() != IDOK) {
		return;
	}
	DoLoadTablemap(dlg.GetPathName());
}

LRESULT CALLBACK CTrainerDlg::LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam)
{
	if (code == HC_ACTION && wParam == WM_LBUTTONDOWN && s_instance != NULL) {
		MSLLHOOKSTRUCT *info = (MSLLHOOKSTRUCT *)lParam;
		POINT pt = info->pt;
		HHOOK hook = s_instance->_mouse_hook;
		s_instance->_mouse_hook = NULL;
		if (hook != NULL) {
			UnhookWindowsHookEx(hook);
		}
		s_instance->PostMessage(WM_TRAINER_ATTACH, (WPARAM)pt.x, (LPARAM)pt.y);
		return 1;
	}
	return CallNextHookEx(NULL, code, wParam, lParam);
}

void CTrainerDlg::OnBnClickedConnect()
{
	if (_mouse_hook != NULL) {
		return;
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
	HWND self_root = ::GetAncestor(GetSafeHwnd(), GA_ROOT);
	HWND web_root = (_web != NULL && ::IsWindow(_web->GetSafeHwnd())) ? ::GetAncestor(_web->GetSafeHwnd(), GA_ROOT) : NULL;
	HWND shot_root = (_screenshot != NULL && ::IsWindow(_screenshot->GetSafeHwnd())) ? ::GetAncestor(_screenshot->GetSafeHwnd(), GA_ROOT) : NULL;
	if (target == NULL || target == self_root
		|| (web_root != NULL && target == web_root)
		|| (shot_root != NULL && target == shot_root)) {
		SetStatus("Ignored a click on the trainer's own window. Click Connect and pick the table.");
		return 0;
	}

	AttachToWindow(target);
	return 0;
}

void CTrainerDlg::AttachToWindow(HWND target)
{
	if (target == NULL || !::IsWindow(target)) {
		return;
	}
	_attached = target;
	for (size_t i = 0; i < _have_baseline.size(); ++i) {
		_have_baseline[i] = false;
	}

	// Open the screenshot view and start the always-on frame timer.
	if (_screenshot == NULL) {
		_screenshot = new CScreenshotView();
		_screenshot->Create(this);
	}
	SetTimer(TRAINER_TIMER, kCaptureIntervalMs, NULL);

	// Remember this window so we can reconnect to it next launch.
	char title[256] = { 0 };
	::GetWindowTextA(target, title, sizeof(title) - 1);
	char cls[256] = { 0 };
	::GetClassNameA(target, cls, sizeof(cls) - 1);
	RegWriteString("last_window_title", CString(title));
	RegWriteString("last_window_class", CString(cls));

	CString status;
	status.Format("Connected to: %s\nScreenshot view open. Click a region to preview; Start to capture.", title);
	SetStatus(status);
}

void CTrainerDlg::RestoreLastSession()
{
	CString last_tm = RegReadString("last_tablemap");
	if (!last_tm.IsEmpty() && GetFileAttributes(last_tm) != INVALID_FILE_ATTRIBUTES) {
		DoLoadTablemap(last_tm);
	}
	CString last_title = RegReadString("last_window_title");
	if (!last_title.IsEmpty()) {
		HWND w = FindTopWindow(last_title, RegReadString("last_window_class"), GetSafeHwnd());
		if (w != NULL) {
			AttachToWindow(w);
		}
	}
}

void CTrainerDlg::SaveOcrSettings()
{
	CString t;
	int ti = m_transform.GetCurSel();
	CString tr;
	if (ti >= 0) m_transform.GetLBText(ti, tr);
	RegWriteString("ocr_transform", tr);
	RegWriteString("ocr_use_default", (IsDlgButtonChecked(IDC_USE_DEFAULT) == BST_CHECKED) ? "1" : "0");
	m_threshold.GetWindowText(t); RegWriteString("ocr_threshold", t);
	int sel = m_matchMode.GetCurSel();
	CString mode;
	if (sel >= 0) mode.Format("%d", (int)m_matchMode.GetItemData(sel));
	RegWriteString("ocr_mode", mode);
	RegWriteString("ocr_use_crop", (IsDlgButtonChecked(IDC_USE_CROP) == BST_CHECKED) ? "1" : "0");
	m_cropSize.GetWindowText(t); RegWriteString("ocr_crop", t);
	m_sharpen.GetWindowText(t); RegWriteString("ocr_sharpen", t);
}

void CTrainerDlg::LoadOcrSettings()
{
	CString v;
	v = RegReadString("ocr_transform");
	if (!v.IsEmpty()) {
		int i = m_transform.FindStringExact(-1, v);
		if (i >= 0) m_transform.SetCurSel(i);
	}
	v = RegReadString("ocr_use_default");
	if (!v.IsEmpty()) CheckDlgButton(IDC_USE_DEFAULT, v == "1" ? BST_CHECKED : BST_UNCHECKED);
	v = RegReadString("ocr_threshold");
	if (!v.IsEmpty()) { m_threshold.SetWindowText(v); m_thresholdSpin.SetPos(atoi(v)); }
	v = RegReadString("ocr_mode");
	if (!v.IsEmpty()) {
		int val = atoi(v);
		for (int i = 0; i < m_matchMode.GetCount(); ++i) {
			if ((int)m_matchMode.GetItemData(i) == val) { m_matchMode.SetCurSel(i); break; }
		}
	}
	v = RegReadString("ocr_use_crop");
	if (!v.IsEmpty()) CheckDlgButton(IDC_USE_CROP, v == "1" ? BST_CHECKED : BST_UNCHECKED);
	v = RegReadString("ocr_crop");
	if (!v.IsEmpty()) { m_cropSize.SetWindowText(v); m_cropSpin.SetPos(atoi(v)); }
	v = RegReadString("ocr_sharpen");
	if (!v.IsEmpty()) { m_sharpen.SetWindowText(v); m_sharpenSpin.SetPos(atoi(v)); }
}

LRESULT CTrainerDlg::OnRegionSelected(WPARAM wParam, LPARAM lParam)
{
	int idx = (int)wParam;
	if (idx >= 0 && idx < (int)_regions.size()) {
		_selected = idx;
		UpdatePreview();
	}
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
		for (size_t i = 0; i < _have_baseline.size(); ++i) {
			_have_baseline[i] = false;   // capture changes from now on
		}
		SetDlgItemText(IDC_STARTSTOP, "Stop");
		SetStatus("Capturing... change balances on the table to generate samples.");
	} else {
		SetDlgItemText(IDC_STARTSTOP, "Start");
		SetStatus("Stopped (screenshot view still live).");
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

void CTrainerDlg::OnBnClickedClearTraining()
{
	if (AfxMessageBox("Delete ALL files in the training\\ folder?", MB_YESNO | MB_ICONWARNING) != IDYES) {
		return;
	}
	char path[MAX_PATH] = { 0 };
	::GetModuleFileName(NULL, path, MAX_PATH);
	char *last = strrchr(path, '\\');
	if (last != NULL) *(last + 1) = '\0';
	CString dir = CString(path) + "training\\";

	CString pattern = dir + "*.*";
	WIN32_FIND_DATA fd;
	HANDLE h = FindFirstFile(pattern, &fd);
	int deleted = 0;
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
				CString f = dir + fd.cFileName;
				if (DeleteFile(f)) ++deleted;
			}
		} while (FindNextFile(h, &fd));
		FindClose(h);
	}
	CString status;
	status.Format("Deleted %d file(s) from %s", deleted, dir.GetString());
	SetStatus(status);
}

STrainerOcrSettings CTrainerDlg::ReadSettings()
{
	STrainerOcrSettings s = DefaultOcrSettings();
	s.use_default = (IsDlgButtonChecked(IDC_USE_DEFAULT) == BST_CHECKED);
	CString t;
	m_threshold.GetWindowText(t); s.threshold = atoi(t);
	int sel = m_matchMode.GetCurSel();
	s.page_seg_mode = (sel >= 0) ? (int)m_matchMode.GetItemData(sel) : (int)PSM_SINGLE_COLUMN;
	s.use_cropping = (IsDlgButtonChecked(IDC_USE_CROP) == BST_CHECKED);
	m_cropSize.GetWindowText(t); s.crop_size = atoi(t);
	m_sharpen.GetWindowText(t); s.sharpen = atoi(t);
	int ti = m_transform.GetCurSel();
	CString tr;
	if (ti >= 0) m_transform.GetLBText(ti, tr);
	s.transform = tr.IsEmpty() ? CString("AutoOcr0") : tr;
	return s;
}

void CTrainerDlg::OnTimer(UINT_PTR nIDEvent)
{
	if (nIDEvent == TRAINER_TIMER) {
		CaptureTick();
	}
	CDialog::OnTimer(nIDEvent);
}

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
	*outW = w; *outH = h;
	return true;
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
	if (_frame != NULL) DeleteObject(_frame);
	_frame = bmp;
	_frame_w = bw;
	_frame_h = bh;

	if (_screenshot != NULL && ::IsWindow(_screenshot->GetSafeHwnd())) {
		_screenshot->UpdateFrame(_frame, bw, bh, _regions, _selected);
	}

	if (_capturing) {
		bool ignore_bad = (IsDlgButtonChecked(IDC_IGNORE_BAD) == BST_CHECKED);
		STrainerOcrSettings settings = ReadSettings();
		for (size_t i = 0; i < _regions.size(); ++i) {
			std::vector<BYTE> cur;
			int w = 0, h = 0;
			if (!CropRegionBgra(_frame, bw, bh, _regions[i].rect, &cur, &w, &h)) {
				continue;
			}
			if (!_have_baseline[i]) {
				_last[i] = cur; _committed[i] = cur; _have_baseline[i] = true;
				continue;
			}
			if (cur != _last[i]) { _last[i] = cur; continue; }   // still settling
			if (cur == _committed[i]) { continue; }
			_committed[i] = cur;

			Mat bgra((int)h, (int)w, CV_8UC4, &cur[0]);
			Mat bgr;
			cvtColor(bgra, bgr, COLOR_BGRA2BGR);

			Mat preview; CString text; int conf = 0;
			_ocr.Run(bgr, settings, _regions[i].name, &preview, &text, &conf);

			if (text.IsEmpty()) {
				continue;   // completely empty OCR — never add
			}
			if (ignore_bad && conf < kMinOcrConfidence) {
				continue;
			}
			std::vector<unsigned char> png, png_transformed;
			std::vector<int> params;
			params.push_back(IMWRITE_PNG_COMPRESSION);
			params.push_back(3);
			// Regular (raw) crop.
			if (!imencode(".png", bgr, png, params) || png.empty()) {
				continue;
			}
			// Transformed image the OCR actually recognized on (best effort).
			if (!preview.empty()) {
				imencode(".png", preview, png_transformed, params);
			}
			if (p_sample_store != NULL) {
				p_sample_store->Add(CStringA(_regions[i].name), png, png_transformed, CStringA(text));
			}
		}
	}

	UpdatePreview();
}

void CTrainerDlg::ClearPreview()
{
	CWnd *frame = GetDlgItem(IDC_OCR_PREVIEW);
	if (frame != NULL) {
		CClientDC dc(frame);
		CRect rc; frame->GetClientRect(&rc);
		dc.FillSolidRect(&rc, RGB(0, 0, 0));
	}
}

void CTrainerDlg::DrawMatToStatic(int ctrl_id, const Mat &bgr)
{
	CWnd *frame = GetDlgItem(ctrl_id);
	if (frame == NULL || bgr.empty()) {
		return;
	}
	Mat bgra;
	cvtColor(bgr, bgra, COLOR_BGR2BGRA);   // 32bpp = always DWORD-aligned rows

	CRect rc;
	frame->GetClientRect(&rc);
	CClientDC dc(frame);
	dc.FillSolidRect(&rc, RGB(0, 0, 0));

	BITMAPINFO bi;
	ZeroMemory(&bi, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = bgra.cols;
	bi.bmiHeader.biHeight = -bgra.rows;   // top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;

	// Fit inside the frame, preserving aspect ratio.
	double scale = min((double)rc.Width() / bgra.cols, (double)rc.Height() / bgra.rows);
	if (scale <= 0) scale = 1.0;
	int dw = (int)(bgra.cols * scale);
	int dh = (int)(bgra.rows * scale);
	int ox = (rc.Width() - dw) / 2;
	int oy = (rc.Height() - dh) / 2;

	SetStretchBltMode(dc.GetSafeHdc(), COLORONCOLOR);
	StretchDIBits(dc.GetSafeHdc(), ox, oy, dw, dh, 0, 0, bgra.cols, bgra.rows,
		bgra.data, &bi, DIB_RGB_COLORS, SRCCOPY);
}

void CTrainerDlg::UpdatePreview()
{
	if (_selected < 0 || _selected >= (int)_regions.size() || _frame == NULL) {
		return;
	}
	std::vector<BYTE> cur;
	int w = 0, h = 0;
	if (!CropRegionBgra(_frame, _frame_w, _frame_h, _regions[_selected].rect, &cur, &w, &h)) {
		return;
	}
	Mat bgra((int)h, (int)w, CV_8UC4, &cur[0]);
	Mat bgr;
	cvtColor(bgra, bgr, COLOR_BGRA2BGR);

	Mat preview; CString text; int conf = 0;
	_ocr.Run(bgr, ReadSettings(), _regions[_selected].name, &preview, &text, &conf);

	if (text.IsEmpty() || preview.empty()) {
		ClearPreview();
		CString r; r.Format("%s: (empty)", _regions[_selected].name.GetString());
		m_ocrResult.SetWindowText(r);
		return;
	}
	DrawMatToStatic(IDC_OCR_PREVIEW, preview);
	CString r;
	r.Format("%s = \"%s\"  (conf %d)", _regions[_selected].name.GetString(), text.GetString(), conf);
	m_ocrResult.SetWindowText(r);
}

void CTrainerDlg::OnDestroy()
{
	SaveOcrSettings();   // persist Image Processing settings for next launch
	KillTimer(TRAINER_TIMER);
	_capturing = false;
	if (_mouse_hook != NULL) {
		UnhookWindowsHookEx(_mouse_hook);
		_mouse_hook = NULL;
	}
	if (_screenshot != NULL) {
		_screenshot->DestroyWindow();
		delete _screenshot;
		_screenshot = NULL;
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
	if (_frame != NULL) {
		DeleteObject(_frame);
		_frame = NULL;
	}
	if (p_sample_store != NULL) {
		delete p_sample_store;
		p_sample_store = NULL;
	}
	s_instance = NULL;
	CDialog::OnDestroy();
}
