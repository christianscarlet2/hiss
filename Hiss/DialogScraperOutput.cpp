//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: DialogScraperOutput.cpp : implementation file
//
//******************************************************************************

#include "stdafx.h"
#include "DialogScraperOutput.h"

#include "CFlagsToolbar.h"
#include "CHeartbeatThread.h"

#include "CAutoOcr.h"
#include "CScraper.h"
#include "..\AutoCrop.h"
#include "..\CTransform\CTransform.h"
#include "..\CTablemap\CTablemapDB.h"
#include "MainFrm.h"
#include "OpenHoldem.h"


// CDlgScraperOutput dialog
CDlgScraperOutput	*m_ScraperOutputDlg = NULL;

#define ID_SCRAPEROUTPUT_SIZERBAR 5555

// Window map tells CWinMgr how to position dialog controls
BEGIN_WINDOW_MAP(ScraperOutputFormulaMap)
BEGINCOLS(WRCT_REST,0,RCMARGINS(4,4))

RCTOFIT(IDC_REGIONLIST)					// list box

RCFIXED(ID_SCRAPEROUTPUT_SIZERBAR,4)	// vertical sizer bar

BEGINROWS(WRCT_REST,0,0)
BEGINCOLS(WRCT_REST,0,0)
RCREST(IDC_SCRAPERBITMAP)			// bitmap control
ENDGROUP()

RCSPACE(8)

BEGINCOLS(WRCT_TOFIT,0,0)
BEGINROWS(WRCT_TOFIT,0,0)
RCSPACE(4)
RCTOFIT(IDC_STATIC_ZOOM)			// "Zoom:"
ENDGROUP()
RCSPACE(4)
RCTOFIT(IDC_ZOOM)					// zoom selection
RCSPACE(8)
BEGINROWS(WRCT_TOFIT,0,0)
RCSPACE(4)
RCTOFIT(IDC_STATIC_RESULT)		// "Result:"
ENDGROUP()
RCSPACE(4)
RCREST(IDC_SCRAPERRESULT)			// scraper result
ENDGROUP()
ENDGROUP()
END_WINDOW_MAP()

CDlgScraperOutput::CDlgScraperOutput(CWnd* pParent /*=NULL*/)
		: CDialog(CDlgScraperOutput::IDD, pParent) {
	in_startup = true;
}

CDlgScraperOutput::~CDlgScraperOutput() {
}

void CDlgScraperOutput::DoDataExchange(CDataExchange* pDX) {
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_REGIONLIST, m_RegionList);
	DDX_Control(pDX, IDC_SCRAPERBITMAP, m_ScraperBitmap);
	DDX_Control(pDX, IDC_ZOOM, m_Zoom);
	DDX_Control(pDX, IDC_SCRAPERRESULT, m_ScraperResult);
	DDX_Control(pDX, IDC_SCRAPER_LEFT, m_ScraperLeft);
	DDX_Control(pDX, IDC_SCRAPER_RIGHT, m_ScraperRight);
	DDX_Control(pDX, IDC_RESULT_LEFT, m_ResultLeft);
	DDX_Control(pDX, IDC_RESULT_RIGHT, m_ResultRight);
}

// Paint an OpenCV image (grayscale or BGR) into a static control, scaled to fit. Used
// for the decimal-split left/right half previews; pass an empty Mat to clear to gray.
static void BlitMatToStatic(CStatic &ctrl, const cv::Mat &img) {
	if (ctrl.GetSafeHwnd() == NULL) return;
	CDC *pDC = ctrl.GetDC();
	if (pDC == NULL) return;
	RECT rc; ctrl.GetClientRect(&rc);
	int W = rc.right - rc.left, H = rc.bottom - rc.top;
	CBrush gray; gray.CreateSolidBrush(COLOR_GRAY);
	CBrush *oldb = pDC->SelectObject(&gray);
	pDC->PatBlt(0, 0, W, H, PATCOPY);
	pDC->SelectObject(oldb);
	if (!img.empty()) {
		cv::Mat disp;
		if (img.channels() == 1) cv::cvtColor(img, disp, cv::COLOR_GRAY2BGRA);
		else if (img.channels() == 3) cv::cvtColor(img, disp, cv::COLOR_BGR2BGRA);
		else disp = img;
		if (!disp.isContinuous()) disp = disp.clone();
		BITMAPINFOHEADER bi = { sizeof(bi), disp.cols, -disp.rows, 1, 32, BI_RGB };
		::SetStretchBltMode(pDC->GetSafeHdc(), COLORONCOLOR);
		::StretchDIBits(pDC->GetSafeHdc(), 1, 1, W - 2, H - 2, 0, 0, disp.cols, disp.rows,
			disp.data, (BITMAPINFO *)&bi, DIB_RGB_COLORS, SRCCOPY);
	}
	ctrl.ReleaseDC(pDC);
}

BEGIN_MESSAGE_MAP(CDlgScraperOutput, CDialog)
  // WinMgr
 	ON_WM_SIZE()
	ON_LBN_SELCHANGE(IDC_REGIONLIST, &CDlgScraperOutput::OnLbnSelchangeRegionlist)
	ON_CBN_SELCHANGE(IDC_ZOOM, &CDlgScraperOutput::OnCbnSelchangeZoom)
	ON_WM_PAINT()
END_MESSAGE_MAP()

// CDlgScraperOutput message handlers
BOOL CDlgScraperOutput::OnInitDialog() {
	RECT		rect = {0};

	in_startup = true;
  CDialog::OnInitDialog();
	// The tablemap + OCR settings were snapshotted at connect time; refresh them from
	// the hiss database so the Scraper Output reflects edits made in the trainer/Vision.
	ReloadFromDatabase();
	// Populate list box with all interesting regions
	AddListboxItems();

	m_Zoom.AddString("None");
	m_Zoom.AddString("2x");
	m_Zoom.AddString("4x");
	m_Zoom.AddString("8x");
	m_Zoom.AddString("16x");
	m_Zoom.SetCurSel(0);

	// Set dialog icon
	HICON hIcon = LoadIcon(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDI_ICON1));
	this->SetIcon(hIcon, FALSE);
	m_Zoom.SetCurSel(Preferences()->scraper_zoom());
	m_Zoom.GetWindowRect(&rect);
	m_Zoom.SetWindowPos(NULL, 0, 0, rect.right-rect.left, 9999, SWP_NOMOVE);

	in_startup = false;
  return TRUE;  // return TRUE unless you set the focus to a control
	// EXCEPTION: OCX Property Pages should return FALSE
}

void CDlgScraperOutput::DestroyWindowSafely() {
  if (m_ScraperOutputDlg) {
    m_ScraperOutputDlg->DestroyWindow();
		delete m_ScraperOutputDlg;
  }
	m_ScraperOutputDlg = NULL;
}

BOOL CDlgScraperOutput::DestroyWindow() {
	WINDOWPLACEMENT		wp;

	// Save settings to registry
	GetWindowPlacement(&wp);
  Preferences()->SetValue(k_prefs_scraper_zoom, m_Zoom.GetCurSel());
	// Uncheck scraper output button on main toolbar
	p_flags_toolbar->CheckButton(ID_MAIN_TOOLBAR_SCRAPER_OUTPUT, false);

	return CDialog::DestroyWindow();
}

void CDlgScraperOutput::PostNcDestroy() {
	delete m_ScraperOutputDlg;
	m_ScraperOutputDlg	=	NULL;
  CDialog::PostNcDestroy();
}


// Re-pull the connected tablemap (regions/transforms) and the OCR settings from the
// hiss database, so the Scraper Output shows the CURRENT settings rather than the
// connect-time snapshot. Done under the heartbeat lock so we never reload mid-scrape.
void CDlgScraperOutput::ReloadFromDatabase() {
	if (p_tablemap == NULL || p_tablemap_db == NULL) {
		return;
	}
	CString name = p_tablemap->filename();   // DB key (set when loaded from the DB)
	if (name.IsEmpty()) {
		return;
	}
	// LoadTablemapFromDB clears the region map, discarding the scraper's per-region
	// GDI bitmaps; free + re-create them around the reload (under the lock) so the next
	// scrape doesn't dereference NULL HBITMAPs and crash.
	if (p_heartbeat_thread != NULL) {
		EnterCriticalSection(&p_heartbeat_thread->cs_update_in_progress);
		if (p_scraper != NULL) p_scraper->DeleteBitmaps();
		p_tablemap_db->LoadTablemapFromDB(name, p_tablemap);
		if (p_scraper != NULL) p_scraper->CreateBitmaps();
		LeaveCriticalSection(&p_heartbeat_thread->cs_update_in_progress);
	} else {
		if (p_scraper != NULL) p_scraper->DeleteBitmaps();
		p_tablemap_db->LoadTablemapFromDB(name, p_tablemap);
		if (p_scraper != NULL) p_scraper->CreateBitmaps();
	}
	// Re-read the per-transform OCR settings + decimal-split list (CAutoOcr caches them
	// once per session otherwise).
	AutoOcr()->LoadModelSettings();
}

void CDlgScraperOutput::AddListboxItems() {
	m_RegionList.ResetContent();
	m_RegionList.SetCurSel(-1);

	for (RMapCI r_iter=p_tablemap->r$()->begin(); r_iter!=p_tablemap->r$()->end(); r_iter++) {
		m_RegionList.AddString(r_iter->second.name);
  }
}

void CDlgScraperOutput::OnLbnSelchangeRegionlist() {
	UpdateDisplay();
}

void CDlgScraperOutput::OnCbnSelchangeZoom() {
	UpdateDisplay();
}

void CDlgScraperOutput::OnPaint() {
	CPaintDC dc(this); // device context for painting
	UpdateDisplay();
}

void CDlgScraperOutput::UpdateDisplay() {
	CString curtext = "";

	if (in_startup) return;
  // Only do this if we are not in the middle of a scraper/symbol update
	if (TryEnterCriticalSection(&p_heartbeat_thread->cs_update_in_progress)) 	{
		if (m_RegionList.GetCurSel() == kUndefined) {
			DoBitblt(NULL, p_tablemap->r$()->end());  // Clear display
			LeaveCriticalSection(&p_heartbeat_thread->cs_update_in_progress);
			return;
		}

		m_RegionList.GetText(m_RegionList.GetCurSel(), curtext);
    RMapCI r_iter = p_tablemap->r$()->find(curtext.GetString());

		if (r_iter != p_tablemap->r$()->end()) {
			DoBitblt(r_iter->second.last_bmp, r_iter);
    }	else {
			DoBitblt(NULL, p_tablemap->r$()->end());  // Clear display
    }
		LeaveCriticalSection(&p_heartbeat_thread->cs_update_in_progress);
	}
}

void CDlgScraperOutput::DoBitblt(HBITMAP bitmap, RMapCI r_iter) {
	CDC			*pDC = m_ScraperBitmap.GetDC();
	HDC			hdcControl = *pDC;
	HDC			hdcScreen = CreateDC("DISPLAY", NULL, NULL, NULL);
	HDC			hdcCompat1 = CreateCompatibleDC(hdcScreen);
	HDC			hdcCompat2 = CreateCompatibleDC(hdcScreen);
	HBITMAP		hbm2 = NULL, old_bitmap1 = NULL, old_bitmap2 = NULL;
	int			w = 0, h = 0, zoom = 0;
	RECT		rect = {0};
	CBrush		gray_brush, *pTempBrush = NULL, oldbrush;
	CPen		null_pen, *pTempPen = NULL, oldpen;
	CString		res = "";
	CTransform	trans;

	if (in_startup)	{
		DeleteDC(hdcCompat1);
		DeleteDC(hdcCompat2);
		DeleteDC(hdcScreen);
		ReleaseDC(pDC);
		return;
	}

	m_ScraperBitmap.GetWindowRect(&rect);

	// Erase control area
	gray_brush.CreateSolidBrush(COLOR_GRAY);
	pTempBrush = (CBrush*)pDC->SelectObject(&gray_brush);
	oldbrush.FromHandle((HBRUSH)pTempBrush);			// Save old brush
	null_pen.CreatePen(PS_NULL, 0, COLOR_BLACK);
	pTempPen = (CPen*)pDC->SelectObject(&null_pen);
	oldpen.FromHandle((HPEN)pTempPen);					// Save old pen
	pDC->Rectangle(1, 1, rect.right-rect.left, rect.bottom-rect.top);
	pDC->SelectObject(oldbrush);
	pDC->SelectObject(oldpen);

	// return if all we needed to do was erase display
	if (bitmap == NULL)	{
		// Also clear the decimal-split previews/results.
		m_ResultLeft.SetWindowText("");
		m_ResultRight.SetWindowText("");
		BlitMatToStatic(m_ScraperLeft, cv::Mat());
		BlitMatToStatic(m_ScraperRight, cv::Mat());
		DeleteDC(hdcCompat1);
		DeleteDC(hdcCompat2);
		DeleteDC(hdcScreen);
		ReleaseDC(pDC);
		return;
	}

	// load bitmap into 1st DC and stretchblt to 2nd DC
	old_bitmap1 = (HBITMAP) SelectObject(hdcCompat1, bitmap);
	zoom = m_Zoom.GetCurSel()==0 ? 1 :
		     m_Zoom.GetCurSel()==1 ? 2 :
		     m_Zoom.GetCurSel()==2 ? 4 :
		     m_Zoom.GetCurSel()==3 ? 8 :
		     m_Zoom.GetCurSel()==4 ? 16 : 1;

	int region_width = r_iter->second.right - r_iter->second.left + 1;
	int region_height = r_iter->second.bottom - r_iter->second.top + 1;
	w = region_width * zoom;
	h = region_height * zoom;

	// Capture the region pixels (BGRA) and compute the Auto Cropper's FINAL image so
	// the preview matches exactly what the OCR pipeline sees (cropped bbox, or an
	// all-white image when colours are absent and "blank" is on).
	Mat region_bgra;
	bool ac_blanked = false;
	if (region_width > 0 && region_height > 0) {
		region_bgra.create(region_height, region_width, CV_8UC4);
		BITMAPINFOHEADER bi = { sizeof(bi), region_width, -region_height, 1, 32, BI_RGB };
		GetDIBits(hdcCompat1, bitmap, 0, region_height, region_bgra.data, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
	}
	SAutoCropColor accols[3] = {
		{ r_iter->second.autocrop_color1, r_iter->second.autocrop_tol1, r_iter->second.autocrop_c1_enabled },
		{ r_iter->second.autocrop_color2, r_iter->second.autocrop_tol2, r_iter->second.autocrop_c2_enabled },
		{ r_iter->second.autocrop_color3, r_iter->second.autocrop_tol3, r_iter->second.autocrop_c3_enabled },
	};
	Mat disp = region_bgra.empty() ? region_bgra
		: AutoCropToColors(region_bgra, r_iter->second.autocrop_enabled, accols,
			r_iter->second.autocrop_blank, &ac_blanked);

	hbm2 = CreateCompatibleBitmap(hdcScreen, w, h);
	old_bitmap2 = (HBITMAP) SelectObject(hdcCompat2, hbm2);
	if (r_iter->second.autocrop_enabled && !disp.empty() && disp.data != region_bgra.data) {
		// Auto-crop changed the image (cropped bbox or white): render that scaled image.
		Mat dispc = disp.isContinuous() ? disp : disp.clone();
		HDC hdcDisp = CreateCompatibleDC(hdcScreen);
		HBITMAP dbm = CreateCompatibleBitmap(hdcScreen, disp.cols, disp.rows);
		HBITMAP oldd = (HBITMAP) SelectObject(hdcDisp, dbm);
		BITMAPINFOHEADER dbi = { sizeof(dbi), disp.cols, -disp.rows, 1, 32, BI_RGB };
		SetDIBits(hdcDisp, dbm, 0, disp.rows, dispc.data, (BITMAPINFO*)&dbi, DIB_RGB_COLORS);
		SetStretchBltMode(hdcCompat2, COLORONCOLOR);
		StretchBlt(hdcCompat2, 0, 0, w, h, hdcDisp, 0, 0, disp.cols, disp.rows, SRCCOPY);
		SelectObject(hdcDisp, oldd);
		DeleteObject(dbm);
		DeleteDC(hdcDisp);
	} else {
		StretchBlt(	hdcCompat2, 0, 0, w, h,
					hdcCompat1, 0, 0, region_width, region_height, SRCCOPY );
	}

	// Copy 2nd DC to control
	BitBlt( hdcControl, 1, 1, rect.right-rect.left-2, rect.bottom-rect.top-2,
			hdcCompat2, 0, 0, SRCCOPY );

	// Output result -- full OCR pipeline (get_ocr_result auto-crops + blanks itself, so
	// pass the FULL region; the result matches the live scraper exactly).
	SAutoOcrSplit split;
	if (r_iter->second.transform[0] == 'A') {
		if (!region_bgra.empty()) {
			res = AutoOcr()->get_ocr_result(region_bgra.clone(), r_iter, false, &split);
		}
	} else {
		trans.DoTransform(r_iter, hdcCompat1, &res);
	}
	m_ScraperResult.SetWindowText(res);   // final result

	// Decimal-split parity with trainer.exe: show the left/right halves + their text,
	// and a red line on the region preview where the split happened.
	if (split.did) {
		m_ResultLeft.SetWindowText(split.left);
		m_ResultRight.SetWindowText(split.right);
		BlitMatToStatic(m_ScraperLeft, split.left_img);
		BlitMatToStatic(m_ScraperRight, split.right_img);
		// Red vertical separator at the split x (region pixels -> zoomed control coords).
		int lx = 1 + split.split_x * zoom;
		CPen redpen(PS_SOLID, 1, RGB(255, 0, 0));
		CPen *oldpen2 = pDC->SelectObject(&redpen);
		pDC->MoveTo(lx, 1);
		pDC->LineTo(lx, rect.bottom - rect.top - 2);
		pDC->SelectObject(oldpen2);
	} else {
		m_ResultLeft.SetWindowText("");
		m_ResultRight.SetWindowText("");
		BlitMatToStatic(m_ScraperLeft, cv::Mat());
		BlitMatToStatic(m_ScraperRight, cv::Mat());
	}

	// Clean up
	SelectObject(hdcCompat1, old_bitmap1);
	SelectObject(hdcCompat2, old_bitmap2);
	DeleteObject(hbm2);
	DeleteDC(hdcCompat1);
	DeleteDC(hdcCompat2);
	DeleteDC(hdcScreen);
	ReleaseDC(pDC);
}

void CDlgScraperOutput::OnCancel() {
	// Uncheck scraper output button on main toolbar
	p_flags_toolbar->CheckButton(ID_MAIN_TOOLBAR_SCRAPER_OUTPUT, false);
  CDialog::OnCancel();
}

void CDlgScraperOutput::Reset() {
	AddListboxItems();
	UpdateDisplay();
}
