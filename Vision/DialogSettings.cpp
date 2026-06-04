//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Modeless Settings dialog implementation. See DialogSettings.h.
//
//******************************************************************************

#include "stdafx.h"
#include "OpenScrape.h"
#include "OpenScrapeDoc.h"
#include "OpenScrapeView.h"
#include "DialogSettings.h"
#include "DecimalSplit.h"
#include "../CTablemap/CTablemap.h"
#include "../CTablemap/CTablemapDB.h"

using namespace cv;

// Canonical list of numeric field types that can use decimal splitting.
static const char *kDecimalFieldTypes[] = {
	"balance", "pot", "bet", "stack", "call", "raise", "blinds", "ante"
};
static const char *kDefaultModelPath = "tessdata\\eng.traineddata";

// Page-seg modes (mirrors trainer's list).
struct SettingsPsm { const char *label; int value; };
static const SettingsPsm kPsm[] = {
	{ "Single block", PSM_SINGLE_BLOCK },
	{ "Single line", PSM_SINGLE_LINE },
	{ "Single word", PSM_SINGLE_WORD },
	{ "Single char", PSM_SINGLE_CHAR },
	{ "Sparse text", PSM_SPARSE_TEXT },
	{ "Sparse text + OSD", PSM_SPARSE_TEXT_OSD },
	{ "Auto", PSM_AUTO },
	{ "Single column", PSM_SINGLE_COLUMN },
};
static const int kNumPsm = sizeof(kPsm) / sizeof(kPsm[0]);

#define SETTINGS_POLL_TIMER 0xA017

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Splits a model path/stem into a Tesseract (datapath, language) pair.
static void SplitModel(const CString &spec, CString *dir, CString *lang)
{
	CString s = spec; s.Trim();
	if (s.IsEmpty()) { *dir = "tessdata"; *lang = "eng"; return; }
	int b = s.ReverseFind('\\'), f = s.ReverseFind('/');
	int cut = (b > f) ? b : f;
	CString file = (cut >= 0) ? s.Mid(cut + 1) : s;
	CString d = (cut >= 0) ? s.Left(cut) : CString("tessdata");
	int dot = file.ReverseFind('.');
	CString l = (dot > 0) ? file.Left(dot) : file;
	if (d.IsEmpty()) d = "tessdata";
	if (l.IsEmpty()) l = "eng";
	*dir = d; *lang = l;
}

// Reads a region's pixels out of the attached frame as a BGR Mat.
static bool GetRegionMat(COpenScrapeDoc *pDoc, const RECT &r, Mat *out)
{
	if (pDoc == NULL || pDoc->attached_bitmap == NULL || out == NULL) return false;
	int fw = pDoc->attached_rect.right - pDoc->attached_rect.left;
	int fh = pDoc->attached_rect.bottom - pDoc->attached_rect.top;
	int rl = r.left, rt = r.top, rw = r.right - r.left + 1, rh = r.bottom - r.top + 1;
	if (fw <= 0 || fh <= 0 || rl < 0 || rt < 0 || rw < 1 || rh < 1 || rl + rw > fw || rt + rh > fh)
		return false;
	int stride = ((fw * 3 + 3) & ~3);
	std::vector<BYTE> buf((size_t)stride * fh);
	BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = fw; bi.bmiHeader.biHeight = -fh;
	bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24; bi.bmiHeader.biCompression = BI_RGB;
	HDC hdc = CreateDC("DISPLAY", NULL, NULL, NULL);
	int got = GetDIBits(hdc, pDoc->attached_bitmap, 0, fh, &buf[0], &bi, DIB_RGB_COLORS);
	DeleteDC(hdc);
	if (got != fh) return false;
	Mat full(fh, fw, CV_8UC3, &buf[0], (size_t)stride);
	*out = full(Rect(rl, rt, rw, rh)).clone();
	return true;
}

// Stretches a Mat into a static control (transient; redrawn each poll tick).
static void BlitMatToCtrl(CWnd *ctrl, const Mat &src)
{
	if (ctrl == NULL || !::IsWindow(ctrl->GetSafeHwnd())) return;
	CRect rc; ctrl->GetClientRect(&rc);
	CDC *dc = ctrl->GetDC();
	if (dc == NULL) return;
	if (src.empty() || rc.Width() < 1 || rc.Height() < 1) {
		dc->FillSolidRect(&rc, RGB(0, 0, 0));
		ctrl->ReleaseDC(dc);
		return;
	}
	Mat bgr;
	if (src.channels() == 1) cvtColor(src, bgr, COLOR_GRAY2BGR);
	else if (src.channels() == 4) cvtColor(src, bgr, COLOR_BGRA2BGR);
	else bgr = src;
	int stride = ((bgr.cols * 3 + 3) & ~3);
	std::vector<BYTE> buf((size_t)stride * bgr.rows);
	for (int y = 0; y < bgr.rows; ++y)
		memcpy(&buf[(size_t)y * stride], bgr.ptr(y), (size_t)bgr.cols * 3);
	BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = bgr.cols; bi.bmiHeader.biHeight = -bgr.rows;
	bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24; bi.bmiHeader.biCompression = BI_RGB;
	dc->SetStretchBltMode(COLORONCOLOR);
	StretchDIBits(dc->GetSafeHdc(), 0, 0, rc.Width(), rc.Height(),
		0, 0, bgr.cols, bgr.rows, &buf[0], &bi, DIB_RGB_COLORS, SRCCOPY);
	ctrl->ReleaseDC(dc);
}

// ---------------------------------------------------------------------------

IMPLEMENT_DYNAMIC(CDlgSettings, CDialog)

CDlgSettings::CDlgSettings(CWnd* pParent /*=NULL*/)
	: CDialog(CDlgSettings::IDD, pParent), m_current_group(0)
{
}

CDlgSettings::~CDlgSettings()
{
}

void CDlgSettings::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_SETTINGS_GROUPS, m_groups);
	DDX_Control(pDX, IDC_EDIT_A0_MODEL, m_model);
	DDX_Control(pDX, IDC_SET_THRESHOLD, m_threshold);
	DDX_Control(pDX, IDC_SET_THRESHOLD_SPIN, m_threshold_spin);
	DDX_Control(pDX, IDC_SET_MODE, m_mode);
	DDX_Control(pDX, IDC_LST_DECIMAL_FIELDS, m_decimal);
	DDX_Control(pDX, IDC_LST_SCRAPE_FIELDS, m_scrape);
}

BEGIN_MESSAGE_MAP(CDlgSettings, CDialog)
	ON_LBN_SELCHANGE(IDC_SETTINGS_GROUPS, &CDlgSettings::OnSelchangeGroups)
	ON_BN_CLICKED(IDC_BTN_A0_BROWSE, &CDlgSettings::OnBrowseModel)
	ON_BN_CLICKED(IDC_CHK_LIVE_POLL, &CDlgSettings::OnLivePollToggled)
	ON_BN_CLICKED(IDC_CHK_NO_PREPROCESS, &CDlgSettings::OnNoPreprocessToggled)
	ON_WM_TIMER()
END_MESSAGE_MAP()

const char *CDlgSettings::GroupKey(int group)
{
	if (group == 2) return "autoocr0";
	if (group == 3) return "autoocr1";
	return NULL;
}

BOOL CDlgSettings::OnInitDialog()
{
	CDialog::OnInitDialog();

	m_groups.AddString("General");
	m_groups.AddString("Fields");
	m_groups.AddString("AutoOcr0");
	m_groups.AddString("AutoOcr1");
	m_groups.AddString("Advanced");
	m_groups.SetCurSel(0);

	m_threshold_spin.SetRange(0, 300);
	m_threshold_spin.SetBuddy(&m_threshold);

	for (int i = 0; i < kNumPsm; ++i) {
		int item = m_mode.AddString(kPsm[i].label);
		m_mode.SetItemData(item, (DWORD_PTR)kPsm[i].value);
	}
	m_mode.SetCurSel(kNumPsm - 1);   // Single column

	// Decimal-split field types (the Fields multi-select).
	for (int i = 0; i < (int)(sizeof(kDecimalFieldTypes) / sizeof(kDecimalFieldTypes[0])); ++i) {
		m_field_types.push_back(CString(kDecimalFieldTypes[i]));
		m_decimal.AddString(kDecimalFieldTypes[i]);
	}
	if (p_tablemap_db != NULL) {
		std::vector<CString> selected;
		p_tablemap_db->GetSettingArray("decimal_split_fields", "fields", &selected);
		for (size_t i = 0; i < selected.size(); ++i)
			for (int j = 0; j < (int)m_field_types.size(); ++j)
				if (m_field_types[j].CompareNoCase(selected[i]) == 0) { m_decimal.SetSel(j, TRUE); break; }
	}

	// Per-field-type scrape enable list (shared with the trainer via scrape_fields).
	{
		const char *scrape_types[] = { "balance", "name" };
		for (int i = 0; i < (int)(sizeof(scrape_types) / sizeof(scrape_types[0])); ++i) {
			m_scrape_types.push_back(CString(scrape_types[i]));
			m_scrape.AddString(scrape_types[i]);
		}
		if (p_tablemap_db != NULL) {
			std::vector<CString> on;
			p_tablemap_db->GetSettingArray("scrape_fields", "fields", &on);
			for (size_t i = 0; i < on.size(); ++i)
				for (int j = 0; j < (int)m_scrape_types.size(); ++j)
					if (m_scrape_types[j].CompareNoCase(on[i]) == 0) { m_scrape.SetSel(j, TRUE); break; }
		}
	}

	m_current_group = 0;
	ShowPage(0);
	return TRUE;
}

void CDlgSettings::LoadGroup(int group)
{
	const char *key = GroupKey(group);
	if (key == NULL || p_tablemap_db == NULL) return;
	CString model = p_tablemap_db->GetSettingString(key, "model");
	CString thr   = p_tablemap_db->GetSettingString(key, "threshold");
	CString mode  = p_tablemap_db->GetSettingString(key, "mode");
	bool nopre = (p_tablemap_db->GetSettingString(key, "no_preprocess") == "1");
	bool nowl  = (p_tablemap_db->GetSettingString(key, "no_whitelist") == "1");
	bool nocs  = (p_tablemap_db->GetSettingString(key, "no_char_spacing") == "1");

	m_model.SetWindowText(model.IsEmpty() ? CString(kDefaultModelPath) : model);
	m_threshold.SetWindowText(thr.IsEmpty() ? CString("150") : thr);
	int modeval = mode.IsEmpty() ? (int)PSM_SINGLE_COLUMN : atoi(mode.GetString());
	for (int i = 0; i < m_mode.GetCount(); ++i)
		if ((int)m_mode.GetItemData(i) == modeval) { m_mode.SetCurSel(i); break; }
	CheckDlgButton(IDC_CHK_NO_PREPROCESS, nopre ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHK_NO_WHITELIST, nowl ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(IDC_CHK_NO_CHARSPACING, nocs ? BST_CHECKED : BST_UNCHECKED);
	UpdateCharSpacingEnable();
}

void CDlgSettings::SaveGroup(int group)
{
	const char *key = GroupKey(group);
	if (key == NULL || p_tablemap_db == NULL) return;
	CString model, thr;
	m_model.GetWindowText(model);
	m_threshold.GetWindowText(thr);
	int sel = m_mode.GetCurSel();
	CString mode;
	mode.Format("%d", (sel >= 0) ? (int)m_mode.GetItemData(sel) : (int)PSM_SINGLE_COLUMN);
	p_tablemap_db->SetSettingString(key, "model", model);
	p_tablemap_db->SetSettingString(key, "threshold", thr);
	p_tablemap_db->SetSettingString(key, "mode", mode);
	p_tablemap_db->SetSettingString(key, "no_preprocess",
		IsDlgButtonChecked(IDC_CHK_NO_PREPROCESS) == BST_CHECKED ? "1" : "0");
	p_tablemap_db->SetSettingString(key, "no_whitelist",
		IsDlgButtonChecked(IDC_CHK_NO_WHITELIST) == BST_CHECKED ? "1" : "0");
	p_tablemap_db->SetSettingString(key, "no_char_spacing",
		IsDlgButtonChecked(IDC_CHK_NO_CHARSPACING) == BST_CHECKED ? "1" : "0");
}

// Character spacing is moot when preprocessing is off (the upscale/spacing step
// is skipped entirely), so disable and ignore the checkbox in that case.
void CDlgSettings::UpdateCharSpacingEnable()
{
	bool pre_off = (IsDlgButtonChecked(IDC_CHK_NO_PREPROCESS) == BST_CHECKED);
	CWnd *w = GetDlgItem(IDC_CHK_NO_CHARSPACING);
	if (w) w->EnableWindow(!pre_off);
}

void CDlgSettings::OnNoPreprocessToggled()
{
	UpdateCharSpacingEnable();
}

void CDlgSettings::SaveDecimalFields()
{
	if (p_tablemap_db == NULL) return;
	std::vector<CString> chosen;
	for (int j = 0; j < (int)m_field_types.size(); ++j)
		if (m_decimal.GetSel(j) > 0) chosen.push_back(m_field_types[j]);
	p_tablemap_db->SetSettingArray("decimal_split_fields", "fields", chosen);
}

void CDlgSettings::SaveScrapeFields()
{
	if (p_tablemap_db == NULL) return;
	std::vector<CString> chosen;
	for (int j = 0; j < (int)m_scrape_types.size(); ++j)
		if (m_scrape.GetSel(j) > 0) chosen.push_back(m_scrape_types[j]);
	p_tablemap_db->SetSettingArray("scrape_fields", "fields", chosen);
}

void CDlgSettings::ShowPage(int index)
{
	int general[]  = { IDC_SET_GENERAL_TEXT };
	int fields[]   = { IDC_SET_FIELDS_DECLBL, IDC_LST_DECIMAL_FIELDS, IDC_SET_SCRAPE_LBL, IDC_LST_SCRAPE_FIELDS };
	int autoocr[]  = { IDC_SET_MODEL_LBL, IDC_EDIT_A0_MODEL, IDC_BTN_A0_BROWSE,
		IDC_SET_THRESHOLD_LBL, IDC_SET_THRESHOLD, IDC_SET_THRESHOLD_SPIN,
		IDC_SET_MODE_LBL, IDC_SET_MODE, IDC_CHK_NO_PREPROCESS, IDC_CHK_NO_WHITELIST,
		IDC_CHK_NO_CHARSPACING, IDC_CHK_LIVE_POLL, IDC_SET_LIVE_HINT, IDC_SET_ORIG_LBL,
		IDC_SET_ORIG_VIEW, IDC_SET_OCR_LBL, IDC_SET_OCR_VIEW, IDC_SET_SPLIT_LBL,
		IDC_SET_ORIG_LEFT, IDC_SET_ORIG_RIGHT, IDC_SET_RESULT_LBL, IDC_SET_RESULT,
		IDC_SET_RESULT_LR_LBL, IDC_SET_RESULT_LEFT, IDC_SET_RESULT_RIGHT };
	int advanced[] = { IDC_SET_ADVANCED_TEXT };

	const int *show = general; int show_n = sizeof(general) / sizeof(int);
	CString title = "General";
	if (index == 1)      { show = fields;  show_n = sizeof(fields) / sizeof(int);  title = "Fields"; }
	else if (index == 2) { show = autoocr; show_n = sizeof(autoocr) / sizeof(int); title = "AutoOcr0"; }
	else if (index == 3) { show = autoocr; show_n = sizeof(autoocr) / sizeof(int); title = "AutoOcr1"; }
	else if (index == 4) { show = advanced;show_n = sizeof(advanced)/ sizeof(int); title = "Advanced"; }

	int all[] = { IDC_SET_GENERAL_TEXT, IDC_SET_FIELDS_DECLBL, IDC_LST_DECIMAL_FIELDS,
		IDC_SET_SCRAPE_LBL, IDC_LST_SCRAPE_FIELDS,
		IDC_SET_MODEL_LBL, IDC_EDIT_A0_MODEL, IDC_BTN_A0_BROWSE,
		IDC_SET_THRESHOLD_LBL, IDC_SET_THRESHOLD, IDC_SET_THRESHOLD_SPIN,
		IDC_SET_MODE_LBL, IDC_SET_MODE, IDC_CHK_NO_PREPROCESS, IDC_CHK_NO_WHITELIST,
		IDC_CHK_NO_CHARSPACING, IDC_CHK_LIVE_POLL, IDC_SET_LIVE_HINT, IDC_SET_ORIG_LBL,
		IDC_SET_ORIG_VIEW, IDC_SET_OCR_LBL, IDC_SET_OCR_VIEW, IDC_SET_SPLIT_LBL,
		IDC_SET_ORIG_LEFT, IDC_SET_ORIG_RIGHT, IDC_SET_RESULT_LBL, IDC_SET_RESULT,
		IDC_SET_RESULT_LR_LBL, IDC_SET_RESULT_LEFT, IDC_SET_RESULT_RIGHT, IDC_SET_ADVANCED_TEXT };
	for (int i = 0; i < (int)(sizeof(all) / sizeof(int)); ++i) {
		CWnd *w = GetDlgItem(all[i]); if (w) w->ShowWindow(SW_HIDE);
	}
	for (int i = 0; i < show_n; ++i) {
		CWnd *w = GetDlgItem(show[i]); if (w) w->ShowWindow(SW_SHOW);
	}
	SetDlgItemText(IDC_SET_PAGE_TITLE, title);
}

void CDlgSettings::OnSelchangeGroups()
{
	int g = m_groups.GetCurSel();
	if (g < 0) return;
	if (m_current_group == 2 || m_current_group == 3) SaveGroup(m_current_group);
	m_current_group = g;
	if (g == 2 || g == 3) LoadGroup(g);
	ShowPage(g);
}

void CDlgSettings::OnBrowseModel()
{
	CString current; m_model.GetWindowText(current);
	CString dir = "tessdata";
	int slash = current.ReverseFind('\\');
	if (slash > 0) dir = current.Left(slash);
	CFileDialog dlg(TRUE, "traineddata", NULL, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
		"Tesseract models (*.traineddata;*.checkpoint)|*.traineddata;*.checkpoint|All files (*.*)|*.*||", this);
	dlg.m_ofn.lpstrInitialDir = dir;
	if (dlg.DoModal() == IDOK) m_model.SetWindowText(dlg.GetPathName());
}

void CDlgSettings::OnLivePollToggled()
{
	if (IsDlgButtonChecked(IDC_CHK_LIVE_POLL) == BST_CHECKED) {
		SetTimer(SETTINGS_POLL_TIMER, 400, NULL);
		RunPreview();
	} else {
		KillTimer(SETTINGS_POLL_TIMER);
	}
}

void CDlgSettings::OnTimer(UINT_PTR nIDEvent)
{
	if (nIDEvent == SETTINGS_POLL_TIMER) {
		if (IsDlgButtonChecked(IDC_CHK_LIVE_POLL) == BST_CHECKED
				&& (m_current_group == 2 || m_current_group == 3)) {
			RunPreview();
		}
	}
	CDialog::OnTimer(nIDEvent);
}

bool CDlgSettings::RegionUsesDecimalSplit(const CString &name)
{
	CString lower = name; lower.MakeLower();
	for (int j = 0; j < (int)m_field_types.size(); ++j) {
		CString f = m_field_types[j]; f.MakeLower();
		if (m_decimal.GetSel(j) > 0 && !f.IsEmpty() && lower.Find(f) != -1) return true;
	}
	return false;
}

void CDlgSettings::RunPreview()
{
	COpenScrapeView *view = COpenScrapeView::GetView();
	COpenScrapeDoc *pDoc = (view != NULL) ? view->GetDocument() : NULL;
	if (view == NULL || pDoc == NULL) return;

	if (pDoc->attached_hwnd == NULL || pDoc->attached_bitmap == NULL) {
		SetDlgItemText(IDC_SET_LIVE_HINT, "Connect to a table first (View > Connect To Window).");
		return;
	}
	// The selected region comes from the TableMap tree dialog.
	CString name;
	if (theApp.m_TableMapDlg != NULL)
		name = theApp.m_TableMapDlg->SelectedRegionName();
	if (name.IsEmpty())
		name = view->PreviewRegionName();   // fallback: scrape-view selection
	if (name.IsEmpty()) {
		SetDlgItemText(IDC_SET_LIVE_HINT, "Select a region in the TableMap tree to preview these settings.");
		return;
	}
	CString hint;
	hint.Format("Live: region '%s'  (transform %s)", name.GetString(),
		(m_current_group == 3) ? "A1" : "A0");
	SetDlgItemText(IDC_SET_LIVE_HINT, hint);

	// Re-capture the live window so the preview tracks the table.
	if (theApp.m_pMainWnd != NULL)
		theApp.m_pMainWnd->SendMessage(WM_COMMAND, ID_VIEW_REFRESH, 0);

	RMapCI r = p_tablemap->r$()->find(name.GetString());
	if (r == p_tablemap->r$()->end()) return;
	RECT rect = { (LONG)r->second.left, (LONG)r->second.top,
		(LONG)r->second.right, (LONG)r->second.bottom };

	Mat crop;
	if (!GetRegionMat(pDoc, rect, &crop) || crop.empty()) return;

	// (Re)load the model for this group if it changed.
	CString modeltext; m_model.GetWindowText(modeltext);
	if (modeltext != m_loaded_model) {
		CString dir, lang; SplitModel(modeltext, &dir, &lang);
		m_ocr.Init(CStringA(dir).GetString(), CStringA(lang).GetString());
		m_loaded_model = modeltext;
	}

	STrainerOcrSettings s = DefaultOcrSettings();
	CString thr; m_threshold.GetWindowText(thr);
	s.threshold = atoi(thr.GetString());
	int sel = m_mode.GetCurSel();
	s.page_seg_mode = (sel >= 0) ? (int)m_mode.GetItemData(sel) : (int)PSM_SINGLE_COLUMN;
	s.no_preprocess = (IsDlgButtonChecked(IDC_CHK_NO_PREPROCESS) == BST_CHECKED);
	s.no_whitelist = (IsDlgButtonChecked(IDC_CHK_NO_WHITELIST) == BST_CHECKED);
	// "Disable character spacing" -> 0px gap (ignored anyway when preprocessing off).
	if (IsDlgButtonChecked(IDC_CHK_NO_CHARSPACING) == BST_CHECKED)
		s.char_spacing = 0;
	s.transform = (m_current_group == 3) ? "AutoOcr1" : "AutoOcr0";
	bool decimal = RegionUsesDecimalSplit(name);
	s.use_decimal_split = decimal;

	Mat preview; CString text; int conf = 0;
	m_ocr.Run(crop, s, name, &preview, &text, &conf);

	// Original (4x) + Auto OCR view.
	Mat orig4x;
	resize(crop, orig4x, Size(crop.cols * 4, crop.rows * 4), 0, 0, INTER_NEAREST);
	BlitMatToCtrl(GetDlgItem(IDC_SET_ORIG_VIEW), orig4x);
	BlitMatToCtrl(GetDlgItem(IDC_SET_OCR_VIEW), preview);

	// Full result.
	SetDlgItemText(IDC_SET_RESULT, text);

	// Decimal split: original left/right images + per-half results.
	if (decimal && m_ocr.did_split()) {
		int sx = FindDecimalSplitX(crop.data, crop.cols, crop.rows, (int)crop.step);
		if (sx > 0 && sx < crop.cols) {
			Mat left = crop(Rect(0, 0, sx, crop.rows)).clone();
			Mat right = crop(Rect(sx, 0, crop.cols - sx, crop.rows)).clone();
			Mat l4, r4;
			resize(left, l4, Size(left.cols * 4, left.rows * 4), 0, 0, INTER_NEAREST);
			resize(right, r4, Size(right.cols * 4, right.rows * 4), 0, 0, INTER_NEAREST);
			BlitMatToCtrl(GetDlgItem(IDC_SET_ORIG_LEFT), l4);
			BlitMatToCtrl(GetDlgItem(IDC_SET_ORIG_RIGHT), r4);
		}
		SetDlgItemText(IDC_SET_RESULT_LEFT, m_ocr.split_left());
		SetDlgItemText(IDC_SET_RESULT_RIGHT, m_ocr.split_right());
	} else {
		Mat empty;
		BlitMatToCtrl(GetDlgItem(IDC_SET_ORIG_LEFT), empty);
		BlitMatToCtrl(GetDlgItem(IDC_SET_ORIG_RIGHT), empty);
		SetDlgItemText(IDC_SET_RESULT_LEFT, decimal ? "" : "n/a");
		SetDlgItemText(IDC_SET_RESULT_RIGHT, decimal ? "" : "n/a");
	}
}

void CDlgSettings::OnOK()
{
	if (m_current_group == 2 || m_current_group == 3) SaveGroup(m_current_group);
	SaveDecimalFields();
	SaveScrapeFields();
	KillTimer(SETTINGS_POLL_TIMER);
	DestroyWindow();
}

void CDlgSettings::OnCancel()
{
	KillTimer(SETTINGS_POLL_TIMER);
	DestroyWindow();
}

void CDlgSettings::PostNcDestroy()
{
	if (theApp.m_SettingsDlg == this) theApp.m_SettingsDlg = NULL;
	delete this;
}
