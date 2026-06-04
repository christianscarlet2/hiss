#include "stdafx.h"
#include "trainer.h"
#include "TrainerDlg.h"
#include "TrainerServer.h"
#include "TrainerWebWindow.h"
#include "ScreenshotView.h"
#include "SampleStore.h"
#include "TrainerFonts.h"
#include "TablemapRegions.h"
#include "TrainerDB.h"
#include "FontGlyphStore.h"
#include "TrainerMessages.h"
#include "WindowCapture.h"
#include "TextImageGen.h"

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

// ---------------------------------------------------------------------------
// CZoomPopup: borderless, click-through, top-most hover magnifier.
// ---------------------------------------------------------------------------
BEGIN_MESSAGE_MAP(CZoomPopup, CWnd)
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
END_MESSAGE_MAP()

CZoomPopup::CZoomPopup() {}
CZoomPopup::~CZoomPopup() {}

BOOL CZoomPopup::EnsureCreated(CWnd *owner)
{
	if (::IsWindow(GetSafeHwnd())) {
		return TRUE;
	}
	CString cls = AfxRegisterWndClass(0, ::LoadCursor(NULL, IDC_ARROW),
		(HBRUSH)::GetStockObject(BLACK_BRUSH), NULL);
	// WS_EX_TRANSPARENT keeps the pointer "passing through" so the cursor never
	// counts as hovering the popup (which would bounce the owner's mouse-leave).
	return CreateEx(
		WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
		cls, "", WS_POPUP,
		0, 0, 16, 16,
		owner != NULL ? owner->GetSafeHwnd() : NULL, NULL);
}

void CZoomPopup::HidePopup()
{
	if (::IsWindow(GetSafeHwnd())) {
		ShowWindow(SW_HIDE);
	}
}

void CZoomPopup::ShowImage(const Mat &bgr, int factor, CPoint screen_anchor)
{
	if (bgr.empty() || !::IsWindow(GetSafeHwnd())) {
		return;
	}
	if (factor < 1) factor = 1;

	// 32bpp copy so painting rows are DWORD-aligned.
	cvtColor(bgr, _bgra, COLOR_BGR2BGRA);

	// Magnified size, clamped to 95% of the work area (preserving aspect).
	int w = _bgra.cols * factor;
	int h = _bgra.rows * factor;
	RECT wa; SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
	int maxW = (int)((wa.right - wa.left) * 0.95);
	int maxH = (int)((wa.bottom - wa.top) * 0.95);
	if ((w > maxW || h > maxH) && w > 0 && h > 0) {
		double s = min((double)maxW / w, (double)maxH / h);
		w = max(1, (int)(w * s));
		h = max(1, (int)(h * s));
	}

	// Offset from the cursor; flip to the opposite side near a work-area edge.
	const int kGap = 18;
	int x = screen_anchor.x + kGap;
	int y = screen_anchor.y + kGap;
	if (x + w > wa.right)  x = screen_anchor.x - kGap - w;
	if (y + h > wa.bottom) y = screen_anchor.y - kGap - h;
	if (x < wa.left) x = wa.left;
	if (y < wa.top)  y = wa.top;

	SetWindowPos(&CWnd::wndTopMost, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
	Invalidate(FALSE);
	UpdateWindow();
}

BOOL CZoomPopup::OnEraseBkgnd(CDC *)
{
	return TRUE;   // OnPaint covers the whole client area
}

void CZoomPopup::OnPaint()
{
	CPaintDC dc(this);
	CRect rc; GetClientRect(&rc);
	if (_bgra.empty()) {
		dc.FillSolidRect(&rc, RGB(0, 0, 0));
		return;
	}
	BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = _bgra.cols;
	bi.bmiHeader.biHeight = -_bgra.rows;   // top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	SetStretchBltMode(dc.GetSafeHdc(), COLORONCOLOR);   // crisp, blocky magnify
	StretchDIBits(dc.GetSafeHdc(), 0, 0, rc.Width(), rc.Height(),
		0, 0, _bgra.cols, _bgra.rows, _bgra.data, &bi, DIB_RGB_COLORS, SRCCOPY);
	CBrush border(RGB(255, 220, 0));
	dc.FrameRect(&rc, &border);
}

CTrainerDlg *CTrainerDlg::s_instance = NULL;

CTrainerDlg::CTrainerDlg(CWnd *pParent)
	: CDialog(CTrainerDlg::IDD, pParent)
{
	_attached = NULL;
	_capturing = false;
	_suppress_persist = false;
	_ocr_loaded_spec = "";
	_mouse_hook = NULL;
	_frame = NULL;
	_frame_w = _frame_h = 0;
	_selected = -1;
	_server = NULL;
	_web = NULL;
	_fonts_web = NULL;
	_screenshot = NULL;
	_icon = NULL;
	_tracking_mouse = false;
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
	DDX_Control(pDX, IDC_CHAR_SPACING, m_charSpacing);
	DDX_Control(pDX, IDC_CHAR_SPACING_SPIN, m_charSpacingSpin);
	DDX_Control(pDX, IDC_OCR_RESULT, m_ocrResult);
	DDX_Control(pDX, IDC_SPLIT_MARGIN, m_splitMargin);
	DDX_Control(pDX, IDC_SPLIT_MARGIN_SPIN, m_splitMarginSpin);
}

BEGIN_MESSAGE_MAP(CTrainerDlg, CDialog)
	ON_BN_CLICKED(IDC_LOAD_TM, &CTrainerDlg::OnBnClickedLoadTm)
	ON_BN_CLICKED(IDC_SELECT_MODEL, &CTrainerDlg::OnBnClickedSelectModel)
	ON_BN_CLICKED(IDC_SELECT_MODEL1, &CTrainerDlg::OnBnClickedSelectModel1)
	ON_CBN_SELCHANGE(IDC_TRANSFORM, &CTrainerDlg::OnSelchangeTransform)
	ON_EN_CHANGE(IDC_SPLIT_MARGIN, &CTrainerDlg::OnChangeSplitMargin)
	ON_BN_CLICKED(IDC_USE_DECIMAL_SPLIT, &CTrainerDlg::OnBnClickedDecimalSplit)
	ON_EN_CHANGE(IDC_THRESHOLD, &CTrainerDlg::OnChangeThreshold)
	ON_CBN_SELCHANGE(IDC_MATCH_MODE, &CTrainerDlg::OnSelchangeMatchMode)
	ON_EN_CHANGE(IDC_CHAR_SPACING, &CTrainerDlg::OnChangeCharSpacing)
	ON_BN_CLICKED(IDC_NO_PREPROCESS, &CTrainerDlg::OnBnClickedNoPreprocess)
	ON_BN_CLICKED(IDC_NO_WHITELIST, &CTrainerDlg::OnBnClickedNoWhitelist)
	ON_BN_CLICKED(IDC_IGNORE_BAD, &CTrainerDlg::OnBnClickedIgnoreBad)
	ON_BN_CLICKED(IDC_CONNECT, &CTrainerDlg::OnBnClickedConnect)
	ON_MESSAGE(WM_TRAINER_SET_CAPTURE, &CTrainerDlg::OnSetCapture)
	ON_MESSAGE(WM_TRAINER_OCR_GLYPH, &CTrainerDlg::OnOcrGlyph)
	ON_BN_CLICKED(IDC_OPEN_TABLE, &CTrainerDlg::OnBnClickedOpenTable)
	ON_BN_CLICKED(IDC_CREATE_FONTS, &CTrainerDlg::OnBnClickedCreateFonts)
	ON_BN_CLICKED(IDC_GEN_SAMPLES, &CTrainerDlg::OnGenerateSamples)
	ON_COMMAND(ID_TOOLS_GEN_SAMPLES, &CTrainerDlg::OnGenerateSamples)
	ON_COMMAND(ID_TOOLS_SETTINGS, &CTrainerDlg::OnToolsSettings)
	ON_MESSAGE(WM_TRAINER_CLEAR_TRAINING, &CTrainerDlg::OnClearTraining)
	ON_WM_TIMER()
	ON_WM_MOUSEMOVE()
	ON_WM_MOUSELEAVE()
	ON_WM_DESTROY()
	ON_MESSAGE(WM_TRAINER_ATTACH, &CTrainerDlg::OnAttachWindow)
	ON_MESSAGE(WM_TRAINER_REGION_SELECTED, &CTrainerDlg::OnRegionSelected)
	ON_MESSAGE(WM_TRAINER_OPEN_FONTS, &CTrainerDlg::OnOpenFonts)
	ON_MESSAGE(WM_TRAINER_CAPTURE_FONTS, &CTrainerDlg::OnCaptureFonts)
	ON_MESSAGE(WM_TRAINER_RECOGNIZE_ALL, &CTrainerDlg::OnRecognizeAll)
	ON_MESSAGE(WM_TRAINER_RELOAD_REGIONS, &CTrainerDlg::OnReloadRegions)
	ON_MESSAGE(WM_EXITSIZEMOVE, &CTrainerDlg::OnExitSizeMove)
END_MESSAGE_MAP()

HWND g_trainer_main_hwnd = NULL;

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
	if (p_trainer_fonts == NULL) {
		p_trainer_fonts = new CTrainerFonts();
	}
	if (p_font_glyph_store == NULL) {
		p_font_glyph_store = new CFontGlyphStore();
	}
	g_trainer_main_hwnd = GetSafeHwnd();
	_server = new CTrainerServer();
	if (_server->Start()) {
		p_trainer_server = _server;
	}

	// Load a baseline engine; the real per-transform model (shared with Hiss/Vision
	// via the settings DB) is applied by ApplyTransformSelection() below once the
	// Transform combo is populated.
	bool ocr_ok = _ocr.Init("tessdata", "my_model");

	PopulateModeCombos();

	// Defaults: threshold 65.
	m_threshold.SetWindowText("65");
	m_thresholdSpin.SetRange(0, 300);
	m_thresholdSpin.SetPos(65);
	m_thresholdSpin.SetBuddy(&m_threshold);
	m_charSpacing.SetWindowText("6");
	m_charSpacingSpin.SetRange(0, 50);
	m_charSpacingSpin.SetPos(6);
	m_charSpacingSpin.SetBuddy(&m_charSpacing);
	m_splitMargin.SetWindowText("10");
	m_splitMarginSpin.SetRange(0, 50);
	m_splitMarginSpin.SetPos(10);
	m_splitMarginSpin.SetBuddy(&m_splitMargin);
	CheckDlgButton(IDC_NO_PREPROCESS, BST_UNCHECKED);
	CheckDlgButton(IDC_NO_WHITELIST, BST_UNCHECKED);
	CheckDlgButton(IDC_USE_DECIMAL_SPLIT, BST_UNCHECKED);

	// Restore the OCR settings the user had on last exit (overrides defaults).
	LoadOcrSettings();
	// "Ignore bad scrapes" is persisted globally in the DB.
	{
		CString ib = TrainerDB_GetSetting("trainer", "ignore_bad");
		if (!ib.IsEmpty()) CheckDlgButton(IDC_IGNORE_BAD, ib == "1" ? BST_CHECKED : BST_UNCHECKED);
	}
	UpdateDecimalSplitControls();   // sync the split boxes to the restored checkbox
	ApplyTransformSelection(false); // sync the engine to the restored Transform combo

	CString status;
	status.Format("Ready. OCR model: %s. Server: %s.\nLoad a tablemap, then Connect and click a window.",
		ocr_ok ? _ocr.model_name().GetString() : "FAILED (check tessdata\\my_model)",
		(_server != NULL && _server->port() != 0) ? "running" : "FAILED");
	SetStatus(status);

	// Put the main window back where it was on last close.
	TrainerDB_RestoreWindowRect(GetSafeHwnd(), "main");

	// Reload the last tablemap and reconnect to the last window if it's open.
	RestoreLastSession();
	return TRUE;
}

void CTrainerDlg::SetStatus(const CString &text)
{
	SetDlgItemText(IDC_STATUS, text);
}

bool CTrainerDlg::DoLoadTablemap(const CString &name)
{
	// Tablemaps now live in the PostgreSQL "hiss" database; `name` is the DB key.
	_regions.clear();
	if (!LoadBalanceRegionsFromDB(name, &_regions)) {
		SetStatus("Failed to read the tablemap from the hiss database.");
		return false;
	}
	// Load the t$ font groups + s$tXtype scan modes for Text0..Text9 recognition.
	if (p_trainer_fonts != NULL) {
		p_trainer_fonts->LoadFromDB(name);
	}
	// Seed the thread-safe region colour cache (source of truth for colour/radius,
	// shared with the HTTP server thread and written back to tm_regions on colour re-pick).
	RegionColors_Reset();
	RegionColors_SetTmPath(name);
	for (size_t i = 0; i < _regions.size(); ++i) {
		RegionColors_Add(_regions[i].name, _regions[i].color, _regions[i].radius, _regions[i].transform);
	}
	_last.assign(_regions.size(), std::vector<BYTE>());
	_committed.assign(_regions.size(), std::vector<BYTE>());
	_have_baseline.assign(_regions.size(), false);
	_selected = _regions.empty() ? -1 : 0;

	RegWriteString("last_tablemap", name);

	CString status;
	status.Format("Loaded %d balance region(s) from tablemap '%s'", (int)_regions.size(), name.GetString());
	SetStatus(status);
	return true;
}

void CTrainerDlg::OnBnClickedLoadTm()
{
	// Tablemaps live in the "hiss" database. Offer the available names in a
	// popup menu (no extra dialog resource needed) and load the chosen one.
	std::vector<CString> names;
	if (!TrainerDB_ListTablemaps(&names)) {
		AfxMessageBox("Could not read tablemaps from the hiss database.\n"
			"Is PostgreSQL running and the 'hiss' database populated?");
		return;
	}
	if (names.empty()) {
		AfxMessageBox("The hiss database has no tablemaps yet.\n"
			"Import some with Vision (OpenScrape) first.");
		return;
	}
	CMenu menu;
	menu.CreatePopupMenu();
	const UINT kBaseId = 1;
	for (size_t i = 0; i < names.size(); ++i) {
		menu.AppendMenu(MF_STRING, kBaseId + (UINT)i, names[i].GetString());
	}
	CPoint pt;
	GetCursorPos(&pt);
	int chosen = (int)menu.TrackPopupMenu(
		TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
		pt.x, pt.y, this);
	if (chosen <= 0) {
		return;
	}
	size_t idx = (size_t)(chosen - kBaseId);
	if (idx < names.size()) {
		DoLoadTablemap(names[idx]);
	}
}

// "autoocr0"/"autoocr1" for the currently-selected Transform (defaults to autoocr0).
CString CTrainerDlg::CurrentAutoOcrKey()
{
	int sel = m_transform.GetCurSel();
	CString tr;
	if (sel >= 0) m_transform.GetLBText(sel, tr);
	int index = 0;
	if (tr.Left(7).CompareNoCase("AutoOcr") == 0) index = atoi(CStringA(tr.Mid(7)));
	return (index == 1) ? CString("autoocr1") : CString("autoocr0");
}

// Load the model stored in the shared settings DB (key autoocr0/autoocr1, field
// "model") into the preview OCR engine. A .checkpoint can't be OCR'd, so it is stored
// for the other tools but the preview falls back to tessdata\my_model. Returns true if
// an engine is loaded.
// Load a model spec (full path to a .traineddata, or a bare lang stem) into the preview
// engine. A .checkpoint can't be OCR'd, so it falls back to tessdata\my_model. Skips the
// re-Init when the same spec is already loaded.
bool CTrainerDlg::ApplyModelSpec(const CString &spec_in)
{
	CString spec = spec_in; spec.Trim();
	if (spec == _ocr_loaded_spec && _ocr.ready()) {
		return true;   // already loaded
	}
	bool ok = false;
	if (!spec.IsEmpty() && spec.Right(11).CompareNoCase(".checkpoint") != 0) {
		int bslash = spec.ReverseFind('\\');
		int fslash = spec.ReverseFind('/');
		int cut = (bslash > fslash) ? bslash : fslash;
		CString dir = (cut >= 0) ? spec.Left(cut) : CString("tessdata");
		CString file = (cut >= 0) ? spec.Mid(cut + 1) : spec;
		int dot = file.ReverseFind('.');
		CString lang = (dot > 0) ? file.Left(dot) : file;
		if (dir.IsEmpty()) dir = "tessdata";
		if (!lang.IsEmpty()) ok = _ocr.Init(CStringA(dir + "\\"), CStringA(lang));
	}
	if (!ok) ok = _ocr.Init("tessdata", "my_model");   // graceful fallback
	_ocr_loaded_spec = spec;
	return ok;
}

bool CTrainerDlg::ApplyModelFromDb(const CString &key)
{
	return ApplyModelSpec(TrainerDB_GetSetting(key, "model"));
}

// Load the model for previewing/scraping a region from the region's OWN transform
// field (A0 -> autoocr0, A1 -> autoocr1). The transform -- not a separate "name model"
// -- decides which model is used; a name region simply uses whatever transform it's
// assigned in the tablemap.
void CTrainerDlg::LoadOcrModelForRegion(int region_idx)
{
	CString key = "autoocr0";
	if (region_idx >= 0 && region_idx < (int)_regions.size()) {
		CString tr = _regions[region_idx].transform; tr.Trim();
		if (tr.CompareNoCase("A1") == 0) key = "autoocr1";
	}
	ApplyModelFromDb(key);
}

// File-pick a model and store it in the shared DB under `key` (autoocr0/autoocr1), the
// same place Hiss and Vision read it from, so all three stay in sync.
void CTrainerDlg::SelectModelForKey(const CString &key)
{
	CFileDialog dlg(TRUE, "traineddata", NULL, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
		"Tesseract models (*.traineddata;*.checkpoint)|*.traineddata;*.checkpoint|All files (*.*)|*.*||", this);
	if (dlg.DoModal() != IDOK) {
		return;
	}
	CString full = dlg.GetPathName();
	TrainerDB_SetSetting(key, "model", full);   // shared with Hiss + Vision

	// Refresh the preview engine for the currently-selected transform (which may be
	// the one just changed).
	ApplyModelFromDb(CurrentAutoOcrKey());

	CString status;
	if (full.Right(11).CompareNoCase(".checkpoint") == 0) {
		status.Format("Saved %s model: %s\n(checkpoint stored for all tools; not used for the OCR preview).",
			key.GetString(), full.GetString());
	} else {
		status.Format("Saved %s model: %s", key.GetString(), full.GetString());
	}
	SetStatus(status);
	UpdatePreview();   // re-run the selected region through the new model
}

void CTrainerDlg::OnBnClickedSelectModel()  { SelectModelForKey("autoocr0"); }
void CTrainerDlg::OnBnClickedSelectModel1() { SelectModelForKey("autoocr1"); }

// Push the desktop "Transform" combo into the shared engine selection so picking
// AutoOcr0/AutoOcr1 actually takes effect (it drives CaptureTick + the web UI's
// "current transform"). This combo only offers the Tesseract transforms; the
// Text0..Text9 font engines are still selected from the web UI.
void CTrainerDlg::ApplyTransformSelection(bool recognize)
{
	if (p_sample_store == NULL) {
		return;
	}
	int sel = m_transform.GetCurSel();
	CString tr;
	if (sel >= 0) m_transform.GetLBText(sel, tr);
	int index = 0;
	if (tr.Left(7).CompareNoCase("AutoOcr") == 0) index = atoi(CStringA(tr.Mid(7)));
	p_sample_store->SetTransform(TRAINER_MODE_AUTOOCR, index);
	// Switch the preview engine + Image-Processing controls to this transform's settings
	// (autoocr0/autoocr1, shared DB).
	CString key = (index == 1) ? CString("autoocr1") : CString("autoocr0");
	ApplyModelFromDb(key);
	LoadOcrSettingsFromDb(key);
	// Mirror the web UI's /api/transform: re-recognize the existing rows with the
	// newly selected transform. Skipped at startup, when there are no samples yet.
	if (recognize) {
		SendMessage(WM_TRAINER_RECOGNIZE_ALL, 0, 0);
	}
	UpdatePreview();
}

void CTrainerDlg::OnSelchangeTransform()
{
	ApplyTransformSelection(true);
}

// Enable the split-result boxes only while decimal splitting is on; clear them
// when it's off so stale values never linger.
void CTrainerDlg::UpdateDecimalSplitControls()
{
	bool on = (IsDlgButtonChecked(IDC_USE_DECIMAL_SPLIT) == BST_CHECKED);
	CWnd *le = GetDlgItem(IDC_SPLIT_LEFT);
	CWnd *re = GetDlgItem(IDC_SPLIT_RIGHT);
	if (le != NULL) le->EnableWindow(on);
	if (re != NULL) re->EnableWindow(on);
	// The "Decimal trim %" control only matters while splitting is active.
	if (m_splitMargin.GetSafeHwnd() != NULL) m_splitMargin.EnableWindow(on);
	if (m_splitMarginSpin.GetSafeHwnd() != NULL) m_splitMarginSpin.EnableWindow(on);
	if (!on) {
		// Disabled: grey the boxes and show "disabled" rather than a stale value.
		SetDlgItemText(IDC_SPLIT_LEFT, "disabled");
		SetDlgItemText(IDC_SPLIT_RIGHT, "disabled");
	}
}

void CTrainerDlg::OnBnClickedDecimalSplit()
{
	UpdateDecimalSplitControls();
	// Coarse toggle of the shared decimal_split_fields list: ON = the full money-field
	// set, OFF = empty. (The Settings dialog's multi-select edits the same list finely.)
	if (!_suppress_persist) {
		std::vector<CString> fields;
		if (IsDlgButtonChecked(IDC_USE_DECIMAL_SPLIT) == BST_CHECKED) {
			const char *money[] = { "balance", "pot", "bet", "stack", "call", "raise", "blinds", "ante" };
			for (int i = 0; i < (int)(sizeof(money) / sizeof(money[0])); ++i) fields.push_back(CString(money[i]));
		}
		TrainerDB_SetSettingArray("decimal_split_fields", "fields", fields);
	}
	UpdatePreview();   // refresh the boxes/preview immediately
}

// The remaining Image-Processing controls: refresh the live preview and push the new
// value to the shared settings DB so Hiss/Vision use it too.
void CTrainerDlg::OnChangeThreshold()      { UpdatePreview(); PersistOcrSettingsToDb(); }
void CTrainerDlg::OnSelchangeMatchMode()   { UpdatePreview(); PersistOcrSettingsToDb(); }
void CTrainerDlg::OnChangeCharSpacing()    { UpdatePreview(); PersistOcrSettingsToDb(); }
void CTrainerDlg::OnBnClickedNoPreprocess(){ UpdatePreview(); PersistOcrSettingsToDb(); }
void CTrainerDlg::OnBnClickedNoWhitelist() { UpdatePreview(); PersistOcrSettingsToDb(); }

// Write the current Image-Processing controls to the shared settings DB (the same
// place Hiss/Vision read from): per-transform fields under autoocr0/autoocr1, plus the
// global decimal_split_fields list. No-op while LoadOcrSettingsFromDb is populating.
void CTrainerDlg::PersistOcrSettingsToDb()
{
	if (_suppress_persist) return;
	CString key = CurrentAutoOcrKey();

	CString t;
	m_threshold.GetWindowText(t);
	TrainerDB_SetSetting(key, "threshold", t);

	int sel = m_matchMode.GetCurSel();
	CString mode;
	if (sel >= 0) mode.Format("%d", (int)m_matchMode.GetItemData(sel));
	TrainerDB_SetSetting(key, "mode", mode);

	TrainerDB_SetSetting(key, "no_preprocess",
		(IsDlgButtonChecked(IDC_NO_PREPROCESS) == BST_CHECKED) ? "1" : "0");
	TrainerDB_SetSetting(key, "no_whitelist",
		(IsDlgButtonChecked(IDC_NO_WHITELIST) == BST_CHECKED) ? "1" : "0");

	// Hiss has only an on/off "no char spacing" bit (its spacing amount is a fixed
	// constant); map it from whether the trainer's spacing value is 0. Also store the
	// actual spacing value so the trainer restores it from the DB.
	CString cs; m_charSpacing.GetWindowText(cs);
	TrainerDB_SetSetting(key, "no_char_spacing", (atoi(CStringA(cs)) == 0) ? "1" : "0");
	TrainerDB_SetSetting(key, "char_spacing", cs);

	// Decimal trim % (preview tuning; trainer-only, but persisted so it survives).
	CString sm; m_splitMargin.GetWindowText(sm);
	TrainerDB_SetSetting(key, "split_margin", sm);
	// NOTE: decimal_split_fields is intentionally NOT written here -- it's owned by the
	// "Use decimal splitting" toggle and the Settings dialog's decimal multi-select, so
	// per-transform edits (threshold/mode/...) don't clobber the field list.
}

// Populate the Image-Processing controls from a transform's settings in the DB, so
// switching the Transform combo shows that transform's stored values. Only overrides a
// control when the DB has a value for it (registry/defaults remain the fallback).
void CTrainerDlg::LoadOcrSettingsFromDb(const CString &key)
{
	_suppress_persist = true;

	CString thr = TrainerDB_GetSetting(key, "threshold");
	if (!thr.IsEmpty()) { m_threshold.SetWindowText(thr); m_thresholdSpin.SetPos(atoi(CStringA(thr))); }

	CString mode = TrainerDB_GetSetting(key, "mode");
	if (!mode.IsEmpty()) {
		int val = atoi(CStringA(mode));
		for (int i = 0; i < m_matchMode.GetCount(); ++i)
			if ((int)m_matchMode.GetItemData(i) == val) { m_matchMode.SetCurSel(i); break; }
	}

	CString nopre = TrainerDB_GetSetting(key, "no_preprocess");
	if (!nopre.IsEmpty()) CheckDlgButton(IDC_NO_PREPROCESS, nopre == "1" ? BST_CHECKED : BST_UNCHECKED);
	CString nowl = TrainerDB_GetSetting(key, "no_whitelist");
	if (!nowl.IsEmpty()) CheckDlgButton(IDC_NO_WHITELIST, nowl == "1" ? BST_CHECKED : BST_UNCHECKED);

	// Prefer the stored spacing value; else fall back to the on/off bit.
	CString csv = TrainerDB_GetSetting(key, "char_spacing");
	if (!csv.IsEmpty()) { m_charSpacing.SetWindowText(csv); m_charSpacingSpin.SetPos(atoi(CStringA(csv))); }
	else {
		CString nocs = TrainerDB_GetSetting(key, "no_char_spacing");
		if (nocs == "1") { m_charSpacing.SetWindowText("0"); m_charSpacingSpin.SetPos(0); }
	}

	CString sm = TrainerDB_GetSetting(key, "split_margin");
	if (!sm.IsEmpty()) { m_splitMargin.SetWindowText(sm); m_splitMarginSpin.SetPos(atoi(CStringA(sm))); }

	// decimal_split_fields is global; value->>'fields' serializes the JSON array (e.g.
	// ["balance",...]) or is empty/"[]" when off.
	CString fields = TrainerDB_GetSetting("decimal_split_fields", "fields");
	bool dec_on = (!fields.IsEmpty() && fields != "[]");
	CheckDlgButton(IDC_USE_DECIMAL_SPLIT, dec_on ? BST_CHECKED : BST_UNCHECKED);
	UpdateDecimalSplitControls();

	_suppress_persist = false;
}

// Re-run the split preview the instant the trim changes (typing or spin), rather
// than waiting for the next capture-timer tick. Fires harmlessly during startup
// SetWindowText too (UpdatePreview no-ops with no frame/selection).
void CTrainerDlg::OnChangeSplitMargin()
{
	UpdatePreview();
	PersistOcrSettingsToDb();
}

// "Ignore bad scrapes" is a global trainer capture filter (not per-transform); persist
// it under the "trainer" settings key so it survives restarts.
void CTrainerDlg::OnBnClickedIgnoreBad()
{
	if (_suppress_persist) return;
	TrainerDB_SetSetting("trainer", "ignore_bad",
		(IsDlgButtonChecked(IDC_IGNORE_BAD) == BST_CHECKED) ? "1" : "0");
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
	// last_tablemap is now a DB tablemap name. DoLoadTablemap fails gracefully
	// if it no longer exists in the database.
	CString last_tm = RegReadString("last_tablemap");
	if (!last_tm.IsEmpty()) {
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
	m_threshold.GetWindowText(t); RegWriteString("ocr_threshold", t);
	int sel = m_matchMode.GetCurSel();
	CString mode;
	if (sel >= 0) mode.Format("%d", (int)m_matchMode.GetItemData(sel));
	RegWriteString("ocr_mode", mode);
	m_charSpacing.GetWindowText(t); RegWriteString("ocr_char_spacing", t);
	RegWriteString("ocr_no_preprocess", (IsDlgButtonChecked(IDC_NO_PREPROCESS) == BST_CHECKED) ? "1" : "0");
	RegWriteString("ocr_no_whitelist", (IsDlgButtonChecked(IDC_NO_WHITELIST) == BST_CHECKED) ? "1" : "0");
	RegWriteString("ocr_decimal_split", (IsDlgButtonChecked(IDC_USE_DECIMAL_SPLIT) == BST_CHECKED) ? "1" : "0");
	m_splitMargin.GetWindowText(t); RegWriteString("ocr_split_margin", t);
}

void CTrainerDlg::LoadOcrSettings()
{
	CString v;
	v = RegReadString("ocr_transform");
	if (!v.IsEmpty()) {
		int i = m_transform.FindStringExact(-1, v);
		if (i >= 0) m_transform.SetCurSel(i);
	}
	v = RegReadString("ocr_threshold");
	if (!v.IsEmpty()) { m_threshold.SetWindowText(v); m_thresholdSpin.SetPos(atoi(v)); }
	v = RegReadString("ocr_mode");
	if (!v.IsEmpty()) {
		int val = atoi(v);
		for (int i = 0; i < m_matchMode.GetCount(); ++i) {
			if ((int)m_matchMode.GetItemData(i) == val) { m_matchMode.SetCurSel(i); break; }
		}
	}
	v = RegReadString("ocr_char_spacing");
	if (!v.IsEmpty()) { m_charSpacing.SetWindowText(v); m_charSpacingSpin.SetPos(atoi(v)); }
	v = RegReadString("ocr_no_preprocess");
	if (!v.IsEmpty()) CheckDlgButton(IDC_NO_PREPROCESS, v == "1" ? BST_CHECKED : BST_UNCHECKED);
	v = RegReadString("ocr_no_whitelist");
	if (!v.IsEmpty()) CheckDlgButton(IDC_NO_WHITELIST, v == "1" ? BST_CHECKED : BST_UNCHECKED);
	v = RegReadString("ocr_decimal_split");
	if (!v.IsEmpty()) CheckDlgButton(IDC_USE_DECIMAL_SPLIT, v == "1" ? BST_CHECKED : BST_UNCHECKED);
	v = RegReadString("ocr_split_margin");
	if (!v.IsEmpty()) { m_splitMargin.SetWindowText(v); m_splitMarginSpin.SetPos(atoi(v)); }
}

LRESULT CTrainerDlg::OnRegionSelected(WPARAM wParam, LPARAM lParam)
{
	int idx = (int)wParam;
	if (idx >= 0 && idx < (int)_regions.size()) {
		_selected = idx;
		ApplyRegionTransform(idx);
	}
	return 0;
}

// Enable/disable the Image-Processing + transform + model controls together (used when
// a non-AutoOcr region is selected).
void CTrainerDlg::EnableOcrControls(bool on)
{
	const int ids[] = {
		IDC_TRANSFORM, IDC_THRESHOLD, IDC_THRESHOLD_SPIN, IDC_MATCH_MODE,
		IDC_CHAR_SPACING, IDC_CHAR_SPACING_SPIN, IDC_NO_PREPROCESS, IDC_NO_WHITELIST,
		IDC_USE_DECIMAL_SPLIT, IDC_SPLIT_MARGIN, IDC_SPLIT_MARGIN_SPIN,
		IDC_SELECT_MODEL, IDC_SELECT_MODEL1
	};
	for (int i = 0; i < (int)(sizeof(ids) / sizeof(ids[0])); ++i) {
		CWnd *w = GetDlgItem(ids[i]);
		if (w != NULL) w->EnableWindow(on ? TRUE : FALSE);
	}
}

// Load the selected region's own transform (A0/A1) + its DB settings into the controls.
// A region whose transform isn't AutoOcr0/AutoOcr1 has its options disabled, with a
// warning, while it stays selected.
void CTrainerDlg::ApplyRegionTransform(int idx)
{
	CString tr = _regions[idx].transform; tr.Trim();
	int autoIndex = -1;
	if (tr.CompareNoCase("A0") == 0) autoIndex = 0;
	else if (tr.CompareNoCase("A1") == 0) autoIndex = 1;

	if (autoIndex < 0) {
		EnableOcrControls(false);
		CString msg;
		msg.Format("Region '%s' uses transform '%s', not AutoOcr0/AutoOcr1 \x96 OCR settings are disabled for this region.",
			_regions[idx].name.GetString(),
			_regions[idx].transform.IsEmpty() ? CString("(none)").GetString() : _regions[idx].transform.GetString());
		SetStatus(msg);
		UpdatePreview();   // still show the scraped image (no editable OCR settings)
		return;
	}

	EnableOcrControls(true);
	// Point the Transform combo at the region's own transform (programmatic SetCurSel
	// does not fire OnSelchangeTransform), then load that transform's model + settings
	// and refresh the preview.
	CString want; want.Format("AutoOcr%d", autoIndex);
	int ci = m_transform.FindStringExact(-1, want);
	if (ci >= 0) m_transform.SetCurSel(ci);
	ApplyTransformSelection(false);   // sets sample-store transform, loads DB settings, previews
}

// The shared scrape_fields list changed (web "Scrape balances/names" toggle): refresh
// which field types are enabled, drop a now-disabled selection, and repaint the Table
// View + preview.
LRESULT CTrainerDlg::OnReloadRegions(WPARAM, LPARAM)
{
	TrainerRegions_RefreshEnabled(&_regions);
	if (_screenshot != NULL && ::IsWindow(_screenshot->GetSafeHwnd())) {
		_screenshot->UpdateFrame(_frame, _frame_w, _frame_h, _regions, _selected);
	}
	UpdatePreview();
	return 0;
}

// Start/stop/toggle capture. action: 1=start, 2=stop, 3=toggle, 0=query only.
// Returns 1 if capturing afterwards, else 0. Drives the web Start/Stop button.
int CTrainerDlg::SetCapture(int action)
{
	bool want;
	if (action == 1)      want = true;
	else if (action == 2) want = false;
	else if (action == 3) want = !_capturing;
	else                  return _capturing ? 1 : 0;   // status query

	if (want) {
		if (_attached == NULL || !::IsWindow(_attached)) {
			SetStatus("Connect to a window first.");
			return _capturing ? 1 : 0;
		}
		if (_regions.empty()) {
			SetStatus("Load a tablemap with balance regions first.");
			return _capturing ? 1 : 0;
		}
		for (size_t i = 0; i < _have_baseline.size(); ++i) {
			_have_baseline[i] = false;   // capture changes from now on
		}
		_capturing = true;
		SetStatus("Capturing... change balances on the table to generate samples.");
	} else {
		_capturing = false;
		SetStatus("Stopped (screenshot view still live).");
	}
	return _capturing ? 1 : 0;
}

// Sent by the HTTP server thread when the web Start/Stop button is clicked.
LRESULT CTrainerDlg::OnSetCapture(WPARAM wParam, LPARAM)
{
	return (LRESULT)SetCapture((int)wParam);
}

// OCR a single font glyph's REFERENCE sub-crop (not the whole scrape) with the
// transform the fonts editor selected, to autopopulate its CHAR field. Runs on
// the UI thread (Tesseract isn't thread-safe). wParam=gid, lParam=(mode<<8)|index.
// Returns the recognized character code, or 0 if nothing was recognized.
LRESULT CTrainerDlg::OnOcrGlyph(WPARAM wParam, LPARAM lParam)
{
	if (p_font_glyph_store == NULL) return 0;
	int gid = (int)wParam;
	int mode = ((int)lParam >> 8) & 0xFF;
	int index = (int)lParam & 0xFF;

	std::vector<BYTE> bgra; int w = 0, h = 0; COLORREF color = 0; int radius = 0, group = 0;
	CStringA region;
	if (!p_font_glyph_store->GetReferenceBgra(gid, &bgra, &w, &h, &color, &radius, &group, &region)
		|| w <= 0 || h <= 0) {
		return 0;
	}

	// Build a BGR view of the reference crop and find its darkest colour.
	Mat refb((int)h, (int)w, CV_8UC4, &bgra[0]);
	Mat refbgr; cvtColor(refb, refbgr, COLOR_BGRA2BGR);
	Vec3b darkest(0, 0, 0);
	double min_lum = 1e30;
	for (int y = 0; y < refbgr.rows; ++y) {
		const Vec3b *row = refbgr.ptr<Vec3b>(y);
		for (int x = 0; x < refbgr.cols; ++x) {
			const Vec3b &p = row[x];   // BGR
			double lum = 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2];
			if (lum < min_lum) { min_lum = lum; darkest = p; }
		}
	}
	// Pad a 3px border of that darkest colour around the whole reference image; this
	// bordered image is both OCR'd and shown in the view (regenerated reference PNG).
	const int kBorder = 3;
	Mat bordered;
	copyMakeBorder(refbgr, bordered, kBorder, kBorder, kBorder, kBorder,
		BORDER_CONSTANT, Scalar(darkest[0], darkest[1], darkest[2]));

	// Regenerate the reference display PNG from the bordered image (6x, pixelated to
	// match TFE_RegularGlyphPng) so the editor shows the bordered glyph.
	Mat big;
	resize(bordered, big, Size(bordered.cols * 6, bordered.rows * 6), 0, 0, INTER_NEAREST);
	std::vector<unsigned char> png;
	std::vector<int> pngp; pngp.push_back(IMWRITE_PNG_COMPRESSION); pngp.push_back(3);
	if (imencode(".png", big, png, pngp)) {
		p_font_glyph_store->SetRegularPng(gid, png);
	}

	CString text;
	if (mode == TRAINER_MODE_TEXT) {
		// Font-hash recognition uses the clean (unbordered) crop -- a border would
		// corrupt the colour-cube segmentation the hash relies on.
		if (p_trainer_fonts != NULL)
			text = p_trainer_fonts->RecognizeBgra(&bgra[0], w, h, color, radius, index);
	} else {
		// Tesseract on the bordered reference, forced to single-character mode.
		STrainerOcrSettings s = ReadSettings();
		s.use_decimal_split = false;
		s.no_preprocess = false;
		s.page_seg_mode = (int)PSM_SINGLE_CHAR;
		Mat preview; int conf = 0;
		_ocr.Run(bordered, s, CString(region), &preview, &text, &conf);
	}
	text.Trim();
	if (text.IsEmpty()) return 0;
	return (LRESULT)(unsigned char)text[0];
}

void CTrainerDlg::OnBnClickedOpenTable()
{
	if (_server == NULL || _server->port() == 0) {
		SetStatus("HTTP server is not running.");
		return;
	}
	if (_web == NULL) {
		_web = new CTrainerWebWindow();
		_web->Create(this, _server->port(), "/trainer/", "Sample Gen Tool");
	} else {
		_web->ShowWindow(SW_SHOWNORMAL);
		_web->SetForegroundWindow();
		_web->NavigateToTrainer(_server->port());
	}
}

// Open the Create Fonts editor (moved here from the Sample Gen web window).
void CTrainerDlg::OnBnClickedCreateFonts()
{
	OpenFontsWindow();
}

// "Generate Username Samples" button + Tools menu: ask for a count, then synthesize
// that many username images with text2image into the training\ folder.
void CTrainerDlg::OnGenerateSamples()
{
	T2I_GenerateInteractive(this);
}

// Tools > Settings...: edit the text2image.exe path + PT4 connection.
void CTrainerDlg::OnToolsSettings()
{
	T2I_OpenSettings(this);
}

// Delete every file in the training\ folder next to the exe. Returns the count.
// The confirmation now lives in the web UI ("Clear Training Files" button).
int CTrainerDlg::ClearTrainingFiles()
{
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
	return deleted;
}

// Triggered by the Sample Gen web window's "Clear Training Files" button.
LRESULT CTrainerDlg::OnClearTraining(WPARAM, LPARAM)
{
	return (LRESULT)ClearTrainingFiles();
}

STrainerOcrSettings CTrainerDlg::ReadSettings()
{
	STrainerOcrSettings s = DefaultOcrSettings();
	// Cropping/sharpen/use-default were removed from the pipeline.
	s.use_default = false;
	s.use_cropping = false;
	s.crop_size = 0;
	s.sharpen = 0;
	CString t;
	m_threshold.GetWindowText(t); s.threshold = atoi(t);
	int sel = m_matchMode.GetCurSel();
	s.page_seg_mode = (sel >= 0) ? (int)m_matchMode.GetItemData(sel) : (int)PSM_SINGLE_COLUMN;
	m_charSpacing.GetWindowText(t); s.char_spacing = atoi(t);
	int ti = m_transform.GetCurSel();
	CString tr;
	if (ti >= 0) m_transform.GetLBText(ti, tr);
	s.transform = tr.IsEmpty() ? CString("AutoOcr0") : tr;
	s.no_preprocess = (IsDlgButtonChecked(IDC_NO_PREPROCESS) == BST_CHECKED);
	s.no_whitelist = (IsDlgButtonChecked(IDC_NO_WHITELIST) == BST_CHECKED);
	s.use_decimal_split = (IsDlgButtonChecked(IDC_USE_DECIMAL_SPLIT) == BST_CHECKED);
	m_splitMargin.GetWindowText(t); s.decimal_split_margin_pct = atoi(t);
	return s;
}

void CTrainerDlg::OnTimer(UINT_PTR nIDEvent)
{
	if (nIDEvent == TRAINER_TIMER) {
		CaptureTick();
	}
	CDialog::OnTimer(nIDEvent);
}

// Show the 4x hover magnifier while the cursor is over the scraped-region image.
// The SS_BLACKFRAME static is hit-test-transparent, so the dialog receives these
// moves with coordinates over the control's rect.
void CTrainerDlg::OnMouseMove(UINT nFlags, CPoint point)
{
	CWnd *ctl = GetDlgItem(IDC_SCRAPE_PREVIEW);
	bool over = false;
	if (ctl != NULL && ::IsWindow(ctl->GetSafeHwnd()) && !_scrape_img.empty()) {
		CRect rc; ctl->GetWindowRect(&rc);
		ScreenToClient(&rc);
		over = (rc.PtInRect(point) != FALSE);
	}

	if (over) {
		CPoint screen = point; ClientToScreen(&screen);
		if (_zoom.EnsureCreated(this)) {
			_zoom.ShowImage(_scrape_img, 4, screen);
		}
		if (!_tracking_mouse) {
			TRACKMOUSEEVENT t; t.cbSize = sizeof(t); t.dwFlags = TME_LEAVE;
			t.hwndTrack = GetSafeHwnd(); t.dwHoverTime = 0;
			_tracking_mouse = (TrackMouseEvent(&t) != FALSE);
		}
	} else {
		_zoom.HidePopup();
	}
	CDialog::OnMouseMove(nFlags, point);
}

void CTrainerDlg::OnMouseLeave()
{
	_tracking_mouse = false;
	_zoom.HidePopup();
	CDialog::OnMouseLeave();
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

LRESULT CTrainerDlg::OnOpenFonts(WPARAM, LPARAM)
{
	OpenFontsWindow();
	return 0;
}

LRESULT CTrainerDlg::OnCaptureFonts(WPARAM, LPARAM)
{
	CaptureFontsForEditor();
	return 0;
}

// Re-recognize every table row with the current transform. Runs on the UI thread
// because AutoOcr uses Tesseract (not thread-safe). Returns the count recognized.
LRESULT CTrainerDlg::OnRecognizeAll(WPARAM, LPARAM)
{
	if (p_sample_store == NULL) return 0;
	int mode = p_sample_store->TransformMode();
	int index = p_sample_store->TransformIndex();
	STrainerOcrSettings settings = ReadSettings();

	std::vector<CSampleStore::SRecogItem> items;
	p_sample_store->SnapshotForRecognition(&items);

	int recognized = 0;
	for (size_t i = 0; i < items.size(); ++i) {
		CSampleStore::SRecogItem &it = items[i];
		if (it.bgra.empty() || it.bw <= 0 || it.bh <= 0) continue;
		CString text;
		if (mode == TRAINER_MODE_TEXT) {
			if (p_trainer_fonts != NULL)
				text = p_trainer_fonts->RecognizeBgra(&it.bgra[0], it.bw, it.bh,
					(COLORREF)it.color, it.radius, index);
		} else {
			Mat bgra(it.bh, it.bw, CV_8UC4, &it.bgra[0]);
			Mat bgr; cvtColor(bgra, bgr, COLOR_BGRA2BGR);
			Mat preview; int conf = 0;
			_ocr.Run(bgr, settings, it.region, &preview, &text, &conf);
		}
		p_sample_store->SetGuess(it.id, CStringA(text));
		if (!text.IsEmpty()) ++recognized;
	}
	return recognized;
}

void CTrainerDlg::OpenFontsWindow()
{
	if (_server == NULL || _server->port() == 0) {
		SetStatus("HTTP server is not running.");
		return;
	}
	if (_fonts_web == NULL) {
		_fonts_web = new CTrainerWebWindow();
		_fonts_web->Create(this, _server->port(), "/trainer/fonts.html", "Font Creation Tool");
	} else {
		_fonts_web->ShowWindow(SW_SHOWNORMAL);
		_fonts_web->SetForegroundWindow();
		_fonts_web->NavigateToTrainer(_server->port());
	}
}

void CTrainerDlg::CaptureFontsForEditor()
{
	if (_attached == NULL || !::IsWindow(_attached)) {
		SetStatus("Connect to a window before capturing fonts.");
		return;
	}
	if (p_font_glyph_store == NULL || p_trainer_fonts == NULL || _regions.empty()) {
		return;
	}
	// Grab a fresh live frame so the editor doesn't depend on the capture timer.
	int bw = 0, bh = 0;
	HBITMAP bmp = CaptureCompositedClientBitmap(_attached, &bw, &bh);
	if (bmp == NULL || bw <= 0 || bh <= 0) {
		if (bmp != NULL) DeleteObject(bmp);
		return;
	}

	int group = p_font_glyph_store->EditGroup();
	int added = 0;
	for (size_t i = 0; i < _regions.size(); ++i) {
		std::vector<BYTE> cur;
		int w = 0, h = 0;
		if (!CropRegionBgra(bmp, bw, bh, _regions[i].rect, &cur, &w, &h)) continue;
		// Use the latest colour/radius from the cache (may have been re-picked).
		COLORREF color = _regions[i].color; int radius = _regions[i].radius;
		RegionColors_GetByName(_regions[i].name, &color, &radius);
		std::vector<TGlyph> glyphs;
		p_trainer_fonts->SegmentBgra(&cur[0], w, h, color, radius, group, &glyphs);
		for (size_t g = 0; g < glyphs.size(); ++g) {
			if (p_font_glyph_store->Add(CStringA(_regions[i].name), group, glyphs[g],
				color, radius, cur, w, h) >= 0) ++added;
		}
	}
	DeleteObject(bmp);

	CString s;
	s.Format("Font capture: %d new glyph(s) added (group Text%d).", added, group);
	SetStatus(s);
}

void CTrainerDlg::CaptureTick()
{
	if (_attached == NULL || !::IsWindow(_attached)) {
		KillTimer(TRAINER_TIMER);
		_capturing = false;   // web Start/Stop button re-syncs via /api/capture
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
		// The table's recognition engine is whatever the React dropdown selected:
		// AutoOcr0/1 (Tesseract) or Text0..Text9 (bitmap-font hashing).
		int mode = (p_sample_store != NULL) ? p_sample_store->TransformMode() : TRAINER_MODE_AUTOOCR;
		int index = (p_sample_store != NULL) ? p_sample_store->TransformIndex() : 0;
		bool ignore_bad = (IsDlgButtonChecked(IDC_IGNORE_BAD) == BST_CHECKED);
		STrainerOcrSettings settings = ReadSettings();
		for (size_t i = 0; i < _regions.size(); ++i) {
			if (!_regions[i].enabled) {
				continue;   // field type not in the shared scrape_fields list
			}
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

			CString text;
			int conf = 100;
			Mat ocr_preview;
			if (mode == TRAINER_MODE_TEXT) {
				if (p_trainer_fonts != NULL)
					text = p_trainer_fonts->RecognizeBgra(&cur[0], w, h, _regions[i].color, _regions[i].radius, index);
			} else {
				// Name fields use the text model; decimal-split only when this field
				// type is in the shared list (matches the live Hiss scraper).
				LoadOcrModelForRegion((int)i);
				STrainerOcrSettings rs = settings;
				rs.use_decimal_split = TrainerRegionUsesDecimalSplit(_regions[i].name);
				_ocr.Run(bgr, rs, _regions[i].name, &ocr_preview, &text, &conf);
			}

			if (text.IsEmpty()) {
				continue;   // nothing recognized with the current transform
			}
			if (mode == TRAINER_MODE_AUTOOCR && ignore_bad && conf < kMinOcrConfidence) {
				continue;
			}

			std::vector<unsigned char> png, png_transformed;
			std::vector<int> params;
			params.push_back(IMWRITE_PNG_COMPRESSION);
			params.push_back(3);
			// Regular (raw) crop for display + training.
			if (!imencode(".png", bgr, png, params) || png.empty()) {
				continue;
			}
			// Transformed view: Tesseract preview for AutoOcr, foreground mask for Text.
			if (mode == TRAINER_MODE_TEXT) {
				if (p_trainer_fonts != NULL)
					p_trainer_fonts->MaskPng(&cur[0], w, h, _regions[i].color, _regions[i].radius, &png_transformed);
			} else if (!ocr_preview.empty()) {
				imencode(".png", ocr_preview, png_transformed, params);
			}
			if (p_sample_store != NULL) {
				p_sample_store->Add(CStringA(_regions[i].name), png, png_transformed, CStringA(text),
					cur, w, h, (unsigned long)_regions[i].color, _regions[i].radius);
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

	// Preview exactly how this field type is scraped: name fields use the text model
	// (general charset, no decimal split); balance/other fields use the transform model
	// and decimal-split only if their type is in the shared decimal_split_fields list.
	LoadOcrModelForRegion(_selected);
	STrainerOcrSettings settings = ReadSettings();
	settings.use_decimal_split = TrainerRegionUsesDecimalSplit(_regions[_selected].name);

	Mat preview; CString text; int conf = 0;
	_ocr.Run(bgr, settings, _regions[_selected].name, &preview, &text, &conf);

	// Per-half results: shown whenever this region actually split (follows the list,
	// not the old global checkbox).
	SetDlgItemText(IDC_SPLIT_LEFT,  _ocr.did_split() ? _ocr.split_left()  : CString(""));
	SetDlgItemText(IDC_SPLIT_RIGHT, _ocr.did_split() ? _ocr.split_right() : CString(""));

	// Scraped-region (OCR input) view + hover-zoom source: show exactly what
	// Tesseract received -- the raw crop when preprocessing is disabled, otherwise
	// the fully preprocessed image -- even when nothing was recognized.
	if (!preview.empty()) {
		_scrape_img = preview.clone();
		DrawMatToStatic(IDC_SCRAPE_PREVIEW, preview);
	} else {
		_scrape_img = Mat();
		CWnd *sp = GetDlgItem(IDC_SCRAPE_PREVIEW);
		if (sp != NULL) {
			CClientDC dc(sp); CRect rc; sp->GetClientRect(&rc);
			dc.FillSolidRect(&rc, RGB(0, 0, 0));
		}
		_zoom.HidePopup();
	}

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

// Fires once when the user finishes dragging/resizing the window (not per pixel),
// so we persist the placement here rather than on every WM_MOVE/WM_SIZE.
LRESULT CTrainerDlg::OnExitSizeMove(WPARAM, LPARAM)
{
	TrainerDB_SaveWindowRect(GetSafeHwnd(), "main");
	return 0;
}

void CTrainerDlg::OnDestroy()
{
	SaveOcrSettings();   // persist Image Processing settings for next launch
	TrainerDB_SaveWindowRect(GetSafeHwnd(), "main");   // backstop: save final placement
	KillTimer(TRAINER_TIMER);
	_capturing = false;
	if (::IsWindow(_zoom.GetSafeHwnd())) {
		_zoom.DestroyWindow();
	}
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
	if (_fonts_web != NULL) {
		_fonts_web->DestroyWindow();
		delete _fonts_web;
		_fonts_web = NULL;
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
	if (p_trainer_fonts != NULL) {
		delete p_trainer_fonts;
		p_trainer_fonts = NULL;
	}
	if (p_font_glyph_store != NULL) {
		delete p_font_glyph_store;
		p_font_glyph_store = NULL;
	}
	g_trainer_main_hwnd = NULL;
	s_instance = NULL;
	CDialog::OnDestroy();
}
