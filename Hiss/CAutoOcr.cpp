//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: AutoOcr class for OpenHoldem
//
//******************************************************************************

#include "StdAfx.h"
#include "CAutoOcr.h"
#include "..\CTablemap\CTablemapDB.h"
#include "..\Vision\DecimalSplit.h"

#include "CAutoconnector.h"
#include "..\Shared\WindowCapture.h"


CAutoOcr *p_auto_ocr = NULL;

CAutoOcr *AutoOcr() {
	static CCritSec auto_ocr_creation_critsec;
	CSLock lock(auto_ocr_creation_critsec);
	if (p_auto_ocr == NULL) {
		p_auto_ocr = new CAutoOcr;
	}
	return p_auto_ocr;
}

#define __HDC_HEADER 		HBITMAP		old_bitmap = NULL; \
	HDC				hdcScreen = CreateDC("DISPLAY", NULL, NULL, NULL); \
	HDC				hdcCompatible = CreateCompatibleDC(hdcScreen); \
	HDC				hdc = CreateCompatibleDC(hdcScreen); \
	int				window_capture_width = 0; \
	int				window_capture_height = 0; \
	HBITMAP		window_capture_bitmap = CaptureCompositedClientBitmap(p_autoconnector->attached_hwnd(), &window_capture_width, &window_capture_height); \
	HBITMAP		old_window_capture_bitmap = window_capture_bitmap ? (HBITMAP)SelectObject(hdc, window_capture_bitmap) : NULL; \
  ++_leaking_GDI_objects;

#define __HDC_FOOTER_ATTENTION_HAS_TO_BE_CALLED_ON_EVERY_FUNCTION_EXIT_OTHERWISE_MEMORY_LEAK \
	if (old_window_capture_bitmap != NULL) SelectObject(hdc, old_window_capture_bitmap); \
	if (window_capture_bitmap != NULL) DeleteObject(window_capture_bitmap); \
	DeleteDC(hdc); \
  DeleteDC(hdcCompatible); \
	DeleteDC(hdcScreen); \
  --_leaking_GDI_objects;

// Declare DNN-Recognizer
//TextRecognitionModel recognizer;
// (api / api2 are now CAutoOcr instance members, not globals, so independent
//  engines can run on parallel worker threads.)

static CString TakeTessUtf8Text(char *text)
{
	if (text == NULL) {
		return "";
	}
	CString result = regex_replace(string(text), regex("\\s"), "").c_str();
	delete[] text;
	return result;
}

static TessBaseAPI *SafeCreateTessBaseAPI()
{
	return new TessBaseAPI;
}

// Split a model spec into a Tesseract (datapath, language) pair.
// Accepts a bare language stem ("my_model" -> tessdata/my_model.traineddata) or
// a full path ("C:\x\tessdata\eng.traineddata" -> datapath "C:\x\tessdata", lang "eng").
static void SplitModelSpec(const CString &spec, CString *dir, CString *lang)
{
	CString s = spec; s.Trim();
	if (s.IsEmpty()) { *dir = "tessdata"; *lang = "my_model"; return; }
	int bslash = s.ReverseFind('\\');
	int fslash = s.ReverseFind('/');
	int cut = (bslash > fslash) ? bslash : fslash;
	CString file = (cut >= 0) ? s.Mid(cut + 1) : s;
	CString d = (cut >= 0) ? s.Left(cut) : CString("tessdata");
	int dot = file.ReverseFind('.');
	CString l = (dot > 0) ? file.Left(dot) : file;
	if (d.IsEmpty()) d = "tessdata";
	if (l.IsEmpty()) l = "my_model";
	*dir = d; *lang = l;
}

static int SafeTessInit(TessBaseAPI *tess, const char *model_spec)
{
	if (tess == NULL) {
		return -1;
	}
	CString dir, lang;
	SplitModelSpec(CString(model_spec), &dir, &lang);
	return tess->Init(CStringA(dir).GetString(), CStringA(lang).GetString());
}

static void SafeTessEnd(TessBaseAPI *tess)
{
	if (tess == NULL) {
		return;
	}
	tess->End();
}

static bool SafeTessSetImageAndRecognize(TessBaseAPI *tess, const Mat &img)
{
	if (tess == NULL || img.empty() || img.data == NULL) {
		return false;
	}
	tess->SetImage(img.data, img.cols, img.rows, img.channels(), img.step);
	tess->Recognize(0);
	return true;
}

static char *SafeTessGetUTF8Text(TessBaseAPI *tess)
{
	if (tess == NULL) {
		return NULL;
	}
	return tess->GetUTF8Text();
}

static ResultIterator *SafeTessGetIterator(TessBaseAPI *tess)
{
	if (tess == NULL) {
		return NULL;
	}
	return tess->GetIterator();
}

static bool SafeResultIteratorBoundingBox(ResultIterator *ri,
	PageIteratorLevel level, int *x1, int *y1, int *x2, int *y2)
{
	if (ri == NULL) {
		return false;
	}
	return ri->BoundingBox(level, x1, y1, x2, y2);
}

static bool SafeResultIteratorNext(ResultIterator *ri, PageIteratorLevel level)
{
	if (ri == NULL) {
		return false;
	}
	return ri->Next(level);
}

//
// Constructor and destructor
//
CAutoOcr::CAutoOcr() :
	api(NULL),
	api2(NULL),
	_api_initialized(false),
	_api2_initialized(false),
	_api_init_failed(false),
	_models_loaded(false),
	_thr_a0(kDefaultAutoOcrThreshold), _thr_a1(kDefaultAutoOcrThreshold),
	_mode_a0((int)tesseract::PSM_SINGLE_COLUMN), _mode_a1((int)tesseract::PSM_SINGLE_COLUMN),
	_nopre_a0(false), _nopre_a1(false),
	_nowl_a0(false), _nowl_a1(false),
	_nocs_a0(false), _nocs_a1(false) {
}

// Read the per-transform OCR settings (model/threshold/mode/no_preprocess/
// no_whitelist) for A0 (autoocr0) and A1 (autoocr1) from the settings table.
void CAutoOcr::LoadModelSettings() {
	_model_a0 = "my_model";
	_model_a1 = "my_model";
	_thr_a0 = _thr_a1 = kDefaultAutoOcrThreshold;
	_mode_a0 = _mode_a1 = (int)tesseract::PSM_SINGLE_COLUMN;
	_nopre_a0 = _nopre_a1 = false;
	_nowl_a0 = _nowl_a1 = false;
	_nocs_a0 = _nocs_a1 = false;
	_decimal_fields.clear();
	if (p_tablemap_db != NULL) {
		p_tablemap_db->GetSettingArray("decimal_split_fields", "fields", &_decimal_fields);
	}
	if (p_tablemap_db != NULL) {
		const char *keys[2] = { "autoocr0", "autoocr1" };
		for (int g = 0; g < 2; ++g) {
			CString model = p_tablemap_db->GetSettingString(keys[g], "model");
			CString thr   = p_tablemap_db->GetSettingString(keys[g], "threshold");
			CString mode  = p_tablemap_db->GetSettingString(keys[g], "mode");
			bool nopre = (p_tablemap_db->GetSettingString(keys[g], "no_preprocess") == "1");
			bool nowl  = (p_tablemap_db->GetSettingString(keys[g], "no_whitelist") == "1");
			bool nocs  = (p_tablemap_db->GetSettingString(keys[g], "no_char_spacing") == "1");
			CString &m = (g == 0) ? _model_a0 : _model_a1;
			int &t = (g == 0) ? _thr_a0 : _thr_a1;
			int &md = (g == 0) ? _mode_a0 : _mode_a1;
			bool &np = (g == 0) ? _nopre_a0 : _nopre_a1;
			bool &nw = (g == 0) ? _nowl_a0 : _nowl_a1;
			bool &nc = (g == 0) ? _nocs_a0 : _nocs_a1;
			if (!model.IsEmpty()) m = model;
			if (!thr.IsEmpty()) t = atoi(thr.GetString());
			if (!mode.IsEmpty()) md = atoi(mode.GetString());
			np = nopre; nw = nowl; nc = nocs;
		}
	}
	_models_loaded = true;
}

// Switch api/api2 to `model` if not already loaded. Tesseract supports
// re-Init to change the model/language.
bool CAutoOcr::EnsureModelLoaded(const CString &model) {
	if (!EnsureTesseractInitialized()) {
		return false;
	}
	CString m = model.IsEmpty() ? CString("my_model") : model;
	if (m == _current_model) {
		return true;
	}
	if (SafeTessInit(api, m.GetString()) == -1 || SafeTessInit(api2, m.GetString()) == -1) {
		_api_init_failed = true;
		return false;
	}
	_current_model = m;
	return true;
}

CAutoOcr::~CAutoOcr() {
	// Unload network
	if (api != NULL) {
		if (_api_initialized) {
			SafeTessEnd(api);
		}
		delete api;
		api = NULL;
	}
	if (api2 != NULL) {
		if (_api2_initialized) {
			SafeTessEnd(api2);
		}
		delete api2;
		api2 = NULL;
	}
}

bool CAutoOcr::EnsureTesseractInitialized() {
	if (!_models_loaded) {
		LoadModelSettings();
	}
	if (_api_initialized && _api2_initialized) {
		return true;
	}
	if (_api_init_failed) {
		return false;
	}

	// OCR is enabled by default. It still loads lazily (on first use, below) so
	// Hiss startup never blocks on Tesseract being ready. HISS_ENABLE_TESSERACT_OCR
	// is honored ONLY as an explicit kill-switch: previously a *missing* variable
	// disabled OCR permanently, but the variable is never set anywhere, so every
	// OCR region (player names, balances, bets, ...) scraped empty and the HUD --
	// which depends on PT4 name verification of those scraped names -- disappeared.
	// The concurrency crash that originally motivated gating OCR is now serialized
	// through m_critsec in get_ocr_result()/GetDetectTemplate*(), so it is safe to
	// re-enable by default.
	char disable_tesseract_ocr[16] = {0};
	if (GetEnvironmentVariable("HISS_ENABLE_TESSERACT_OCR", disable_tesseract_ocr, sizeof(disable_tesseract_ocr)) != 0) {
		CString flag(disable_tesseract_ocr);
		flag.Trim();
		flag.MakeLower();
		if (flag == "0" || flag == "false" || flag == "no" || flag == "off") {
			_api_init_failed = true;
			return false;
		}
	}

	// New automatic OCR based on tesseract-ocr. Load the recognition network
	// lazily so Hiss startup does not depend on Tesseract being ready.
	if (!_api_initialized) {
		if (api == NULL) {
			api = SafeCreateTessBaseAPI();
			if (api == NULL) {
				_api_init_failed = true;
				MessageBox(NULL, "Failed to create Tesseract OCR instance.", "AutoOcr error", MB_OK);
				return false;
			}
		}
		if (SafeTessInit(api, _model_a0.GetString()) == -1) {		// OEM_LSTM_ONLY
			_api_init_failed = true;
			MessageBox(NULL, "Failed to load tessdata files.\nMake sure tessdata folder is present and/or datas are not corrupted.", "AutoOcr error", MB_OK);
			return false;
		}
		_api_initialized = true;
		_current_model = _model_a0;
	}

	if (!_api2_initialized) {
		if (api2 == NULL) {
			api2 = SafeCreateTessBaseAPI();
			if (api2 == NULL) {
				_api_init_failed = true;
				MessageBox(NULL, "Failed to create Tesseract OCR instance.", "AutoOcr error", MB_OK);
				return false;
			}
		}
		if (SafeTessInit(api2, _model_a0.GetString()) == -1) {		// OEM_LSTM_ONLY
			_api_init_failed = true;
			MessageBox(NULL, "Failed to load tessdata files.\nMake sure tessdata folder is present and/or datas are not corrupted.", "AutoOcr error", MB_OK);
			return false;
		}
		_api2_initialized = true;
		_current_model = _model_a0;
	}

	return true;
}


////  Automatic Text Detection and Recognition functions  ///////////
Mat CAutoOcr::binarize_array_opencv(Mat image, int threshold) {
	// Binarize image from gray channel with 100 as threshold
	Mat img;
	cvtColor(image, img, COLOR_BGR2RGB);
	cvtColor(img, img, COLOR_BGR2GRAY);
	Mat thresh, blur;
	cv::threshold(img, thresh, threshold, 255, THRESH_BINARY_INV); // 100 threshold
	float kernel_data[9] = { 1, 2, 1, 2, 4, 2, 1, 2, 1 };
	Mat kernel = Mat(1, 1, CV_32F, kernel_data);
	cv::filter2D(thresh, blur, -1, kernel);
	Mat ret;
	cv::threshold(blur, ret, 250, 255, THRESH_BINARY); // 250 threshold
	return ret;
}

COLORREF CAutoOcr::AverageFourByFour(Mat img, int center_x, int center_y)
{
	long r = 0, g = 0, b = 0, samples = 0;
	for (int y = center_y - 1; y <= center_y + 2; ++y) {
		if (y < 0 || y >= img.rows) {
			continue;
		}
		for (int x = center_x - 1; x <= center_x + 2; ++x) {
			if (x < 0 || x >= img.cols) {
				continue;
			}
			if (img.channels() == 4) {
				Vec4b pixel = img.at<Vec4b>(y, x);
				b += pixel[0];
				g += pixel[1];
				r += pixel[2];
			}
			else if (img.channels() == 3) {
				Vec3b pixel = img.at<Vec3b>(y, x);
				b += pixel[0];
				g += pixel[1];
				r += pixel[2];
			}
			else if (img.channels() == 1) {
				uchar pixel = img.at<uchar>(y, x);
				r += pixel;
				g += pixel;
				b += pixel;
			}
			++samples;
		}
	}
	if (samples <= 0) {
		return RGB(0, 0, 0);
	}
	return RGB(r / samples, g / samples, b / samples);
}

static const int kDefaultColorPresetIndex = -1;

static CString ColorPresetKey(int index, const char *field)
{
	CString key;
	if (index == kDefaultColorPresetIndex) {
		key.Format("oscoloripdefault%s", field);
	} else {
		key.Format("oscolorip%d%s", index, field);
	}
	return key;
}

bool CAutoOcr::ReadColorPresetColor(int index, COLORREF *color)
{
	if (color == NULL || p_tablemap == NULL) {
		return false;
	}

	CString red_text = p_tablemap->GetTMSymbol(ColorPresetKey(index, "r"));
	CString green_text = p_tablemap->GetTMSymbol(ColorPresetKey(index, "g"));
	CString blue_text = p_tablemap->GetTMSymbol(ColorPresetKey(index, "b"));
	if (!red_text.IsEmpty() || !green_text.IsEmpty() || !blue_text.IsEmpty()) {
		int red = red_text.IsEmpty() ? 0 : max(0, min(255, atoi(red_text.GetString())));
		int green = green_text.IsEmpty() ? 0 : max(0, min(255, atoi(green_text.GetString())));
		int blue = blue_text.IsEmpty() ? 0 : max(0, min(255, atoi(blue_text.GetString())));
		*color = RGB(red, green, blue);
		return true;
	}

	CString color_text = p_tablemap->GetTMSymbol(ColorPresetKey(index, "color"));
	if (color_text.IsEmpty()) {
		return false;
	}
	*color = strtoul(color_text.GetString(), NULL, 16);
	return true;
}

bool CAutoOcr::ReadColorPresetSamplePoint(int index, RMapCI region, int *rel_x, int *rel_y)
{
	if (rel_x == NULL || rel_y == NULL || p_tablemap == NULL) {
		return false;
	}

	CString point_rel_x_text = p_tablemap->GetTMSymbol(ColorPresetKey(index, "pointrelx"));
	CString point_rel_y_text = p_tablemap->GetTMSymbol(ColorPresetKey(index, "pointrely"));
	if (!point_rel_x_text.IsEmpty() && !point_rel_y_text.IsEmpty()) {
		*rel_x = atoi(point_rel_x_text.GetString());
		*rel_y = atoi(point_rel_y_text.GetString());
		return true;
	}

	CString point_x_text = p_tablemap->GetTMSymbol(ColorPresetKey(index, "pointx"));
	CString point_y_text = p_tablemap->GetTMSymbol(ColorPresetKey(index, "pointy"));
	if (point_x_text.IsEmpty() || point_y_text.IsEmpty()) {
		return false;
	}
	*rel_x = atoi(point_x_text.GetString()) - (int)region->second.left;
	*rel_y = atoi(point_y_text.GetString()) - (int)region->second.top;
	return true;
}

bool CAutoOcr::TryColorPresetSettings(Mat img_orig, RMapCI region, SAutoOcrSettings *settings)
{
	if (settings == NULL || p_tablemap == NULL) {
		return false;
	}

	const long kMaxColorPresetDistance = 70L * 70L * 3L;
	int count = atoi(p_tablemap->GetTMSymbol("oscoloripcount").GetString());
	int best_index = -1;
	long best_distance = LONG_MAX;
	for (int i = 0; i < count; ++i) {
		COLORREF preset;
		int rel_x = 0, rel_y = 0;
		if (!ReadColorPresetColor(i, &preset) || !ReadColorPresetSamplePoint(i, region, &rel_x, &rel_y)) {
			continue;
		}

		if (rel_x < 0 || rel_y < 0 || rel_x >= img_orig.cols || rel_y >= img_orig.rows) {
			continue;
		}

		COLORREF sampled = AverageFourByFour(img_orig, rel_x, rel_y);
		long dr = (long)GetRValue(sampled) - (long)GetRValue(preset);
		long dg = (long)GetGValue(sampled) - (long)GetGValue(preset);
		long db = (long)GetBValue(sampled) - (long)GetBValue(preset);
		long distance = dr * dr + dg * dg + db * db;
		if (distance < best_distance) {
			best_distance = distance;
			best_index = i;
		}
	}

	if (best_index < 0 || best_distance > kMaxColorPresetDistance) {
		best_index = kDefaultColorPresetIndex;
	}

	CString text;
	text = p_tablemap->GetTMSymbol(ColorPresetKey(best_index, "use_default"));
	bool use_default = text.IsEmpty() ? best_index == kDefaultColorPresetIndex : atoi(text.GetString()) != 0;
	text = p_tablemap->GetTMSymbol(ColorPresetKey(best_index, "threshold"));
	if (!use_default && !text.IsEmpty()) {
		settings->threshold = atoi(text.GetString());
	}
	text = p_tablemap->GetTMSymbol(ColorPresetKey(best_index, "use_crop"));
	if (!text.IsEmpty()) {
		settings->use_cropping = atoi(text.GetString()) != 0;
	}
	text = p_tablemap->GetTMSymbol(ColorPresetKey(best_index, "crop"));
	if (!text.IsEmpty()) {
		settings->crop_size = atoi(text.GetString());
	}
	text = p_tablemap->GetTMSymbol(ColorPresetKey(best_index, "psm"));
	if (!text.IsEmpty()) {
		settings->page_seg_mode = atoi(text.GetString());
	}
	return true;
}

// Matches Vision/trainer: upscale the prepared region before binarization/OCR
// so Tesseract sees larger glyphs, and restrict output to characters that occur
// on tables (balance regions narrow this to digits + dot in get_ocr_result).
static const int kOcrScaleUpFactor = 3;
static const int kOcrCharSpacingPx = 6;   // on the upscaled image
static const char *kGeneralWhitelist =
	"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._";

// Widen the gaps between characters on a binarized image so glyphs that sit too
// close (e.g. "17.71") get separated before OCR. Builds a vertical projection
// profile and inserts gap_px blank columns into each existing gap; because it
// only widens existing gaps it never splits a single glyph. Picks the minority
// pixel value as "ink" so it works for either text polarity.
static Mat AddCharacterSpacing(const Mat& binary, int gap_px) {
	if (binary.empty() || binary.channels() != 1 || gap_px <= 0)
		return binary;

	try {
		int total = binary.rows * binary.cols;
		int white = countNonZero(binary);
		uchar fg = (white <= total - white) ? 255 : 0;

		Mat ink;
		compare(binary, fg, ink, CMP_EQ);
		Mat col_has_ink;		// 1 x cols (CV_8U)
		reduce(ink, col_has_ink, 0, REDUCE_MAX);

		std::vector<std::pair<int, int>> spans;
		bool in_char = false;
		int start = 0;
		for (int c = 0; c < binary.cols; c++) {
			bool has_ink = col_has_ink.at<uchar>(0, c) > 0;
			if (has_ink && !in_char) { in_char = true; start = c; }
			else if (!has_ink && in_char) { in_char = false; spans.push_back({ start, c }); }
		}
		if (in_char) spans.push_back({ start, binary.cols });
		if (spans.size() <= 1)
			return binary;

		uchar bg = static_cast<uchar>(255 - fg);
		int extra = gap_px * static_cast<int>(spans.size() - 1);
		Mat out(binary.rows, binary.cols + extra, binary.type(), Scalar(bg));

		int read_x = 0, write_x = 0;
		for (size_t i = 0; i < spans.size(); i++) {
			int seg_w = spans[i].second - read_x;
			binary(Rect(read_x, 0, seg_w, binary.rows)).copyTo(out(Rect(write_x, 0, seg_w, binary.rows)));
			write_x += seg_w;
			read_x = spans[i].second;
			if (i + 1 < spans.size())
				write_x += gap_px;
		}
		if (read_x < binary.cols) {
			int seg_w = binary.cols - read_x;
			binary(Rect(read_x, 0, seg_w, binary.rows)).copyTo(out(Rect(write_x, 0, seg_w, binary.rows)));
		}
		return out;
	}
	catch (const cv::Exception&) {
		return binary;			// never let preprocessing crash OCR
	}
}

Mat CAutoOcr::prepareImage(Mat img_orig, const SAutoOcrSettings &settings, bool binarize, int threshold, bool second_pass) {
	// Prepare image for OCR
	//  !!  Do not change those settings and values !!   //
	//  Or display on OCR view control will be distorded //
	Mat img_resized;
	int basewidth, hsize;
	float wpercent;
	if (img_orig.cols > img_orig.rows * 1.25) {
		basewidth = MAT_WIDTH * kOcrScaleUpFactor;
		wpercent = (basewidth / static_cast<float>(img_orig.cols));
		hsize = static_cast<int>(static_cast<float>(img_orig.rows) * wpercent);
	}
	else {
		hsize = MAT_HEIGHT * kOcrScaleUpFactor;
		wpercent = (hsize / static_cast<float>(img_orig.rows));
		basewidth = static_cast<int>(static_cast<float>(img_orig.cols) * wpercent);
	}
	cvtColor(img_orig, img_resized, COLOR_BGR2GRAY);
	// The upscale is enhancement: skip it when "no preprocessing" is set.
	// (Sharpen and cropping were removed from the pipeline.)
	if (!settings.no_preprocess) {
		resize(img_resized, img_resized, Size(basewidth, hsize), INTER_LANCZOS4);
	}

	// Threshold binarization ALWAYS applies (independent of "no preprocessing").
	// Character spacing is enhancement and is skipped under "no preprocessing".
	if (binarize) {
		img_resized = binarize_array_opencv(img_resized, threshold);
		if (!settings.no_preprocess && !settings.no_char_spacing) {
			img_resized = AddCharacterSpacing(img_resized, kOcrCharSpacingPx);
		}
	}
	Mat img_bounded = img_resized.clone();

	// Cropping management ///////////////////////////
	if (settings.use_cropping == true) {
		double crop_size = (double)settings.crop_size / 100;
		if (crop_size < 0.01)
			return img_resized;

		process_ocr(img_resized, settings, false, second_pass);
		vector<pair<Rect, CString>> resBoxes;
		if (second_pass)
			resBoxes = ResultBoxes2;
		else
			resBoxes = ResultBoxes;

		// Exit here if no box found (generally an empty region)
		if (resBoxes.empty())
			return img_resized;

		// filter contours
		vector<Rect> boundRect, boundRect2;
		img_bounded.convertTo(img_bounded, CV_8UC3);
		cvtColor(img_bounded, img_bounded, COLOR_GRAY2BGR);
		//m_crColor = m_BoxColor.GetSelectedColorValue();
		for (size_t idx = 0; idx < resBoxes.size(); idx++)
		{
			Rect rect = resBoxes[idx].first;
			if (rect.area() > 50) {
				boundRect.push_back(rect);
			}
		}

		// Exit if no valid box found (generally an empty region)
		if (boundRect.empty())
			return img_resized;

		vector<double> boxArea, boxDist;
		double wCenter = img_bounded.cols / 2;
		double hCenter = img_bounded.rows / 2;
		Point pCenter(wCenter, hCenter);
		Rect best_rect = Rect();
		if (second_pass)
			bestRect2 = Rect();
		else
			bestRect = Rect();
		//  Find best box match for recognition
		// First find biggest box(es)
		for (size_t i = 0; i < boundRect.size(); i++) {
			boxArea.push_back(boundRect[i].width * boundRect[i].height);
		}
		// Select the element with the maximum value
		auto ita = max_element(boxArea.begin(), boxArea.end());
		int maxArea = *ita;
		vector<int> maxIndex;
		for (size_t i = 0; i < boxArea.size(); i++) {
			if (boxArea[i] >  maxArea*(1 - crop_size)) {  // default to 40% diff factor for concurrent boxes
				maxIndex.push_back(i);
			}
		}
		for (size_t i = 0; i < maxIndex.size(); i++) {
			int j = maxIndex[i];
			best_rect = boundRect[j];
			boundRect2.push_back(best_rect);
		}
		// Second find nearest big box from region center
		if (boundRect2.size() > 1) {
			for (size_t i = 0; i < boundRect2.size(); i++) {
				double wcenter = boundRect2[i].x + boundRect2[i].width / 2;
				double hcenter = boundRect2[i].y + boundRect2[i].height / 2;
				Point pcenter(wcenter, hcenter);
				double dist = abs(norm(pCenter - pcenter));
				boxDist.push_back(dist);
			}
			// Select the element with the minimum value
			auto itd = min_element(boxDist.begin(), boxDist.end());
			int minDist = *itd;
			for (size_t i = 0; i < boxDist.size(); i++) {
				if (boxDist[i] == minDist) {
					best_rect = boundRect2[i];
					break;
				}
			}
		}

		// Set the corresponding global bestRect
		if (second_pass)
			bestRect2 = best_rect;
		else
			bestRect = best_rect;

		return img_bounded;
	}
	/////////////////////////////////////////////////

	process_ocr(img_resized, settings, false, second_pass);
	return img_resized;
}

void CAutoOcr::process_ocr(Mat img_orig, const SAutoOcrSettings &settings, bool fast, bool second_pass) {
	if (!EnsureTesseractInitialized() || img_orig.empty() || img_orig.data == NULL) {
		return;
	}

	tesseract::PageSegMode page_seg_mode = static_cast<tesseract::PageSegMode>(settings.page_seg_mode);
	api->SetPageSegMode(page_seg_mode);
	api2->SetPageSegMode(page_seg_mode);
	api->SetVariable("user_defined_dpi", "300");
	api2->SetVariable("user_defined_dpi", "300");
	if (!settings.whitelist.IsEmpty()) {
		api->SetVariable("tessedit_char_whitelist", settings.whitelist.GetString());
		api2->SetVariable("tessedit_char_whitelist", settings.whitelist.GetString());
	}
	else {
		// Clear any whitelist left over from a previous balance region.
		api->SetVariable("tessedit_char_whitelist", "");
		api2->SetVariable("tessedit_char_whitelist", "");
	}
	if (!SafeTessSetImageAndRecognize(api, img_orig)) {
		_api_init_failed = true;
		return;
	}

	if (settings.use_cropping == true) {
		ResultIterator* ri = SafeTessGetIterator(api);
		PageIteratorLevel level = tesseract::RIL_WORD;
		if (ri != 0) {
			do {
				int x1, y1, x2, y2;
				if (!SafeResultIteratorBoundingBox(ri, level, &x1, &y1, &x2, &y2)) {
					continue;
				}
				if (x1 < 0 || y1 < 0 || x2 <= x1 || y2 <= y1 || x2 > img_orig.cols || y2 > img_orig.rows) {
					continue;
				}
				Mat img_cropped;
				try {
					img_cropped = img_orig({ x1, y1, x2 - x1, y2 - y1 });
				}
				catch (exception e) {
					continue;
				}
				if (!SafeTessSetImageAndRecognize(api2, img_cropped)) {
					_api_init_failed = true;
					return;
				}
				CString word = TakeTessUtf8Text(SafeTessGetUTF8Text(api2));
				pair<Rect, CString> matchPair({ x1, y1, x2 - x1, y2 - y1 }, word);
				if (second_pass)
					ResultBoxes2.push_back(matchPair);
				else
					ResultBoxes.push_back(matchPair);
			} while (SafeResultIteratorNext(ri, level));
		}
	}
	else {
		if (second_pass)
			ResultString2 = TakeTessUtf8Text(SafeTessGetUTF8Text(api));
		else
			ResultString = TakeTessUtf8Text(SafeTessGetUTF8Text(api));
	}
}

// Balance/stack fields are shown on the table in big blinds, e.g. "2.28 BB".
// Strip a trailing "BB" unit. Tesseract frequently misreads "BB" as "88", so a
// trailing "88" is also dropped, but only when a numeric value remains so a
// genuine value is never destroyed.
static CString StripBalanceUnitSuffix(CString s) {
	s.Trim();
	int n = s.GetLength();
	int end = n;
	while (end > 0 && (s[end - 1] == 'B' || s[end - 1] == 'b'))
		end--;
	if (end < n) {
		s = s.Left(end);
	}
	else if (s.GetLength() >= 2 && s.Right(2) == "88") {
		CString candidate = s.Left(s.GetLength() - 2);
		if (candidate.FindOneOf("0123456789") != -1)
			s = candidate;
	}
	s.Trim(" .");
	return s;
}

// True when this region's name matches one of the field types selected for
// decimal splitting in Vision (Settings > Fields), e.g. "balance", "pot".
bool CAutoOcr::RegionUsesDecimalSplit(const CString &region_name) {
	CString lower = region_name; lower.MakeLower();
	for (size_t i = 0; i < _decimal_fields.size(); ++i) {
		CString f = _decimal_fields[i]; f.MakeLower(); f.Trim();
		if (!f.IsEmpty() && lower.Find(f) != -1) return true;
	}
	return false;
}

// Add a 2px border of the image's darkest colour to the chosen side(s). Applied to the
// colour half BEFORE OCR (always, independent of "no preprocessing"), so each decimal
// half gets an outer margin in the image's own background colour.
static Mat PadDarkestSides(const Mat &bgr, bool left, bool right)
{
	if (bgr.empty() || bgr.type() != CV_8UC3) return bgr;
	Vec3b dark(0, 0, 0);
	double min_lum = 1e30;
	for (int y = 0; y < bgr.rows; ++y) {
		const Vec3b *row = bgr.ptr<Vec3b>(y);
		for (int x = 0; x < bgr.cols; ++x) {
			const Vec3b &p = row[x];
			double lum = 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2];
			if (lum < min_lum) { min_lum = lum; dark = p; }
		}
	}
	Mat out;
	copyMakeBorder(bgr, out, 0, 0, left ? 2 : 0, right ? 2 : 0,
		BORDER_CONSTANT, Scalar(dark[0], dark[1], dark[2]));
	return out;
}

CString CAutoOcr::get_ocr_result(Mat img_orig, RMapCI region, bool fast, SAutoOcrSplit *split_out) {
	// Tesseract's TessBaseAPI is NOT thread-safe and CAutoOcr's instance
	// members (ResultBoxes, ResultString, bestRect, ...) are shared across
	// calls. CScraper drives OCR from the heartbeat thread while the scraper-
	// output dialog can drive it from the UI thread; concurrent calls were
	// crashing inside tesseract55.dll (0xc0000005). Serialize through the
	// existing critical section.
	CSLock ocr_lock(m_critsec);

	if (split_out != NULL) *split_out = SAutoOcrSplit();   // reset (race-free under the lock)

	// Return string value from image. "" when OCR failed
	if (!EnsureTesseractInitialized() || img_orig.empty() || img_orig.data == NULL) {
		return "";
	}

	Mat img_resized, img_resized2;
	ResultBoxes.clear(); ResultBoxes2.clear();
	ResultString = "";
	ResultString2 = "";

	// OCR parameters come from the per-transform settings groups (autoocr0 for
	// "A0", autoocr1 for "A1"), not from the region. Threshold and page-seg mode
	// always apply, regardless of "no preprocessing".
	bool isA1 = (region->second.transform == "A1");
	int tablemap_threshold = isA1 ? _thr_a1 : _thr_a0;
	if (tablemap_threshold <= 0) tablemap_threshold = kDefaultAutoOcrThreshold;
	SAutoOcrSettings settings;
	settings.threshold = tablemap_threshold;
	settings.use_cropping = false;   // cropping removed from the pipeline
	settings.crop_size = 0;
	settings.page_seg_mode = isA1 ? _mode_a1 : _mode_a0;
	settings.sharpen = 0;            // sharpen removed from the pipeline
	settings.no_preprocess = isA1 ? _nopre_a1 : _nopre_a0;
	settings.no_char_spacing = isA1 ? _nocs_a1 : _nocs_a0;
	// Balance fields are pure numbers; restrict OCR to digits and a dot. Other
	// regions use the general character set.
	if (CString(region->first).MakeLower().Find("balance") != -1)
		settings.whitelist = "0123456789.";
	else
		settings.whitelist = kGeneralWhitelist;
	if (isA1 ? _nowl_a1 : _nowl_a0) {
		settings.whitelist = "";
	}
	TryColorPresetSettings(img_orig, region, &settings);
	settings.use_cropping = false;   // never crop, even if a colour preset set it
	tablemap_threshold = settings.threshold;

	// The region's transform field (A0/A1) decides which model OCRs it.
	if (region->second.transform == "A0") {
		EnsureModelLoaded(_model_a0);
	} else if (region->second.transform == "A1") {
		EnsureModelLoaded(_model_a1);
	}

	vector<CString> lst;
	CString ocr_result, ocr_result2;

	// Decimal splitting (Vision's Settings > Fields list). When the region's name
	// matches a selected field type, find the decimal separator, OCR each half
	// independently and join them with ".". Falls back to whole-image OCR if no
	// separator is found.
	bool did_decimal = false;
	if (RegionUsesDecimalSplit(CString(region->first))
			&& img_orig.type() == CV_8UC3 && img_orig.cols >= 3 && img_orig.rows >= 3) {
		int sx = FindDecimalSplitX(img_orig.data, img_orig.cols, img_orig.rows, (int)img_orig.step);
		if (sx > 0 && sx < img_orig.cols) {
			Mat left = img_orig(Rect(0, 0, sx, img_orig.rows)).clone();
			Mat right = img_orig(Rect(sx, 0, img_orig.cols - sx, img_orig.rows)).clone();
			// Pad each half's OUTER edge with 2px of that half's darkest colour before
			// OCR (always applied, not a preprocessing step): left half on its left,
			// right half on its right.
			left = PadDarkestSides(left, true, false);
			right = PadDarkestSides(right, false, true);
			ResultString = ""; ResultString2 = "";
			Mat left_proc = prepareImage(left, settings, true, tablemap_threshold);
			CString left_text = ResultString;
			ResultString = ""; ResultString2 = "";
			Mat right_proc = prepareImage(right, settings, true, tablemap_threshold);
			CString right_text = ResultString;
			left_text.Trim(); right_text.Trim();
			ocr_result = left_text + "." + right_text;
			ocr_result2 = "";
			did_decimal = true;
			if (split_out != NULL) {
				split_out->did = true;
				split_out->left = left_text;
				split_out->right = right_text;
				split_out->left_img = left_proc.clone();
				split_out->right_img = right_proc.clone();
				split_out->split_x = sx;
			}
		}
	}

	if (!did_decimal) {
		img_resized = prepareImage(img_orig, settings, true, tablemap_threshold);
		img_resized2 = prepareImage(img_orig, settings, true, tablemap_threshold, true);
		if (!img_resized.empty()) {
			img_resized.convertTo(img_resized, CV_8UC3);
			cvtColor(img_resized, img_resized, COLOR_GRAY2BGR);
		}
		if (!img_resized2.empty()) {
			img_resized2.convertTo(img_resized2, CV_8UC3);
			cvtColor(img_resized2, img_resized2, COLOR_GRAY2BGR);
		}
		ocr_result = ResultString;
		ocr_result2 = ResultString2;
	}

	// Clean OCR noise from unwanted chars.
	const char* blacklist = "��??�!%&*+;=?@�^���������������������������������܀��P�~����������\"`#<{([])}>|�������++��++++++--+-+��++--�-+----++++++++�_���a�GpSs�tFTOd8fen=�==()�����vn��";
	for (size_t i = 0; i < strlen(blacklist); i++) {
		char c = blacklist[i];
		if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
			(c >= 'a' && c <= 'z') || c == '.' || c == '_')
			continue;	// keep valid OCR characters (fixes stripped digits/letters)
		if (ocr_result.Find(c) != -1)
			ocr_result.Remove(c);
		if (ocr_result2.Find(c) != -1)
			ocr_result2.Remove(c);
	}
	ocr_result.Trim();
	ocr_result2.Trim();

	// Balance/stack fields carry a "BB" unit suffix that must be dropped.
	if (CString(region->first).MakeLower().Find("balance") != -1) {
		ocr_result = StripBalanceUnitSuffix(ocr_result);
		ocr_result2 = StripBalanceUnitSuffix(ocr_result2);
	}

	if (ocr_result != "")
		lst.push_back(ocr_result);
	if (ocr_result2 != "")
		lst.push_back(ocr_result2);


	// Display OCR recognition result
	try {
		if (!lst.empty()) {
			return lst.back();
		}
		else {
			return "";
		}
	}
	catch (invalid_argument) {
		if (fast) {
			return "";
		}
		// , img_min, img_mod, img_med, img_sharp]
		vector<Mat> images = { img_orig, img_resized };
		int i = 0;
		while (i < 2) {
			size_t j = 0;
			while (j < images.size()) {
				CString ocr_str;
				process_ocr(images[j], settings);
				for (auto & element : ResultBoxes) {
					if (element.first == bestRect) {
						ocr_str = element.second;
						break;
					}
				}
				if (ocr_str != "")
					lst.push_back(ocr_str);
				j++;
			}
			i++;
		}
	}
#pragma warning(push)
#pragma warning(disable:4101)
	for (const auto& element : lst) {
		try {
			return element;
		}
		catch (const invalid_argument& e) {
			continue;
			// log.warning(f"Not recognized: {element}")
		}
	}
#pragma warning(pop)

	return "";
}

#define ENT CSLock lock(m_critsec);
RECT CAutoOcr::detectTemplate(Mat area, Mat tpl, int match_mode) {
	//  Detect template	
	RECT result;
	int result_cols = area.cols - tpl.cols + 1;
	int result_rows = area.rows - tpl.rows + 1;
	// Invalid template size > area size
	if (result_cols < 1 || result_rows < 1)
		return result = RECT{ 0 };
	Mat match_roi = Mat(result_rows, result_cols, CV_32FC1);
	// Do the Matching and Normalize
	ENT matchTemplate(area, tpl, match_roi, match_mode);
	//normalize(match_roi, match_roi, 0, 1, NORM_MINMAX, -1, Mat());_CrtMemCheckpoint(&sNew); 

	/// Localizing the best match with minMaxLoc
	double matchVal, minVal, maxVal;
	Point minLoc, maxLoc, matchLoc;

	minMaxLoc(match_roi, &minVal, &maxVal, &minLoc, &maxLoc);

	/// For SQDIFF and SQDIFF_NORMED, the best matches are lower values. For all the other methods, the higher the better
	if (match_mode == TM_SQDIFF || match_mode == TM_SQDIFF_NORMED)
	{
		matchLoc = minLoc;
		matchVal = minVal;
	}
	else
	{
		matchLoc = maxLoc;
		matchVal = maxVal;
	}

	// Exact match found
	if (-0.02 <= matchVal && matchVal <= 0.02 || 0.98 <= matchVal && matchVal <= 1.02) {  // 0.02% tolerance
		result = RECT{ matchLoc.x, matchLoc.y , matchLoc.x + tpl.cols, matchLoc.y + tpl.rows };
	}
	else
		// No match found
		result = RECT{ 0 };

	return result;
}
#undef ENT

CString CAutoOcr::GetDetectTemplateResult(CString area_name, CString tpl_name, RECT* rect_result) {
	CSLock ocr_lock(m_critsec);
	//  Detect template
	CString				text, selected_transform, separation;
	RMapCI				r_iter = p_tablemap->r$()->find(area_name.GetString());
	TPLMapCI			sel_template = p_tablemap->tpl$()->end();
	int		 width, height, match_mode;

	// Exit because the area doesn't exist
	if (r_iter == p_tablemap->r$()->end()) {
		return "false";
	}

	__HDC_HEADER
	for (sel_template = p_tablemap->tpl$()->begin(); sel_template != p_tablemap->tpl$()->end(); sel_template++) {
		// Pass if template isn't created
		if (!sel_template->second.created)
			continue;

		// Pass if it isn't the target template
		CString name = sel_template->second.name;
		name.Replace("button_action_", ""); name.Replace("button_", "");
		if (name.Left(tpl_name.GetLength()) != tpl_name)
			continue;

		// Get template size
		width = sel_template->second.width;
		height = sel_template->second.height;
		match_mode = sel_template->second.match_mode;
		if (width < 1 || height < 1 || match_mode < 0)
			continue;

		// Bitblt the area into a HDC
		// Get bitmap size
		int w = r_iter->second.right - r_iter->second.left + 1;
		int h = r_iter->second.bottom - r_iter->second.top + 1;

		old_bitmap = (HBITMAP)SelectObject(hdcCompatible, r_iter->second.cur_bmp);

		BitBlt(hdcCompatible, 0, 0, w, h,
			hdc,
			r_iter->second.left - 1, r_iter->second.top - 1,
			SRCCOPY);
		SelectObject(hdcCompatible, old_bitmap);

		Mat area_mat(h, w, CV_8UC4);
		BITMAPINFOHEADER bi = { sizeof(bi), w, -h, 1, 32, BI_RGB };
		GetDIBits(hdc, r_iter->second.cur_bmp, 0, h, area_mat.data, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
		//SaveHBITMAPToFile(bitmap_transform, "output.bmp");

		//imshow("Area Image", area_mat);
		//waitKey();

		int					x, y;
		HBITMAP				bitmap_image;
		BYTE				*pBits, alpha, red, green, blue;
		RECT roi, zero_rect = RECT{ 0 };

		bitmap_image = CreateCompatibleBitmap(hdcScreen, width, height);
		old_bitmap = (HBITMAP)SelectObject(hdcCompatible, bitmap_image);

		// Setup BITMAPINFO
		BITMAPINFO	bmi;
		ZeroMemory(&bmi.bmiHeader, sizeof(BITMAPINFOHEADER));
		bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
		bmi.bmiHeader.biWidth = width;
		bmi.bmiHeader.biHeight = -height;
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB; //BI_BITFIELDS;
		bmi.bmiHeader.biSizeImage = width * height * 4;

		// Copy saved image info into pBits array
		pBits = new BYTE[bmi.bmiHeader.biSizeImage];
		for (y = 0; y < (int)height; y++) {
			for (x = 0; x < (int)width; x++) {
				// image record is stored internally in ABGR format
				alpha = (sel_template->second.pixel[y*width + x] >> 24) & 0xff;
				red = (sel_template->second.pixel[y*width + x] >> 0) & 0xff;
				green = (sel_template->second.pixel[y*width + x] >> 8) & 0xff;
				blue = (sel_template->second.pixel[y*width + x] >> 16) & 0xff;

				// SetDIBits format is BGRA
				pBits[y*width * 4 + x * 4 + 0] = blue;
				pBits[y*width * 4 + x * 4 + 1] = green;
				pBits[y*width * 4 + x * 4 + 2] = red;
				pBits[y*width * 4 + x * 4 + 3] = alpha;
			}
		}
		::SetDIBits(hdcCompatible, bitmap_image, 0, height, pBits, &bmi, DIB_RGB_COLORS);

		Mat template_mat(height, width, CV_8UC4);
		template_mat.data = pBits;
		//imshow("Template Image", template_mat);
		//waitKey();

		roi = detectTemplate(area_mat, template_mat, match_mode);
		if (!EqualRect(&roi, &zero_rect)) {
			if (area_name.Find("area_buttons_zone") != -1) {
				*rect_result = RECT{ (int)r_iter->second.left + roi.left, (int)r_iter->second.top + roi.top,
					(int)r_iter->second.left + roi.right, (int)r_iter->second.top + roi.bottom };

				// Clean up
				SelectObject(hdcCompatible, old_bitmap);
				DeleteObject(bitmap_image);
				// Free memory
				delete []pBits;
			__HDC_FOOTER_ATTENTION_HAS_TO_BE_CALLED_ON_EVERY_FUNCTION_EXIT_OTHERWISE_MEMORY_LEAK
				return "true";
			}
		}
		// Clean up
		SelectObject(hdcCompatible, old_bitmap);
		DeleteObject(bitmap_image);
		// Free memory
		delete[]pBits;
	}

	// Clean up
	__HDC_FOOTER_ATTENTION_HAS_TO_BE_CALLED_ON_EVERY_FUNCTION_EXIT_OTHERWISE_MEMORY_LEAK

	*rect_result = RECT{ 0 };
	return "false";
}

vector<CString> CAutoOcr::GetDetectTemplatesResult(CString area_name) {
	CSLock ocr_lock(m_critsec);
	//  Detect template
	CString				text, selected_transform, separation;
	RMapCI				r_iter = p_tablemap->r$()->find(area_name.GetString());
	TPLMapCI			sel_template = p_tablemap->tpl$()->end();

	// Exit because the area doesn't exist
	if (r_iter == p_tablemap->r$()->end()) {
		return{ "" };
	}

	__HDC_HEADER
	// Bitblt the area bitmap into a HDC
	// Get bitmap size
	int w = r_iter->second.right - r_iter->second.left + 1;
	int h = r_iter->second.bottom - r_iter->second.top + 1;

	old_bitmap = (HBITMAP)SelectObject(hdcCompatible, r_iter->second.cur_bmp);

	BitBlt(hdcCompatible, 0, 0, w, h,
		hdc,
		r_iter->second.left - 1, r_iter->second.top - 1,
		SRCCOPY);
	SelectObject(hdcCompatible, old_bitmap);

	Mat area_mat(h, w, CV_8UC4);
	BITMAPINFOHEADER bi = { sizeof(bi), w, -h, 1, 32, BI_RGB };
	GetDIBits(hdc, r_iter->second.cur_bmp, 0, h, area_mat.data, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
	//SaveHBITMAPToFile(bitmap_transform, "output.bmp");

	//imshow("Area Image", area_mat);
	//waitKey();


	vector<pair<int, CString>> listROI;

	// Get search templates in area
	for (sel_template = p_tablemap->tpl$()->begin(); sel_template != p_tablemap->tpl$()->end(); sel_template++) {
		int					x, y, width, height, match_mode;
		HBITMAP				bitmap_image;
		BYTE				*pBits, alpha, red, green, blue;
		RECT roi, zero_rect = RECT{ 0 };
		if (area_name == "area_cards_common" && sel_template->second.name.Find("card_common_") == -1 || !sel_template->second.created)
			continue;
		if (area_name.Find("area_cards_player") != -1 && sel_template->second.name.Find("card_player_") == -1 || !sel_template->second.created) {
			if (area_name.Find("area_cards_player") != -1 && sel_template->second.name.Find("card_common_") == -1 || !sel_template->second.created)
				continue;
		}
		// Get template size
		width = sel_template->second.width;
		height = sel_template->second.height;
		match_mode = sel_template->second.match_mode;
		if (width < 1 || height < 1 || match_mode < 0)
			continue;

		bitmap_image = CreateCompatibleBitmap(hdcScreen, width, height);
		old_bitmap = (HBITMAP)SelectObject(hdcCompatible, bitmap_image);

		// Setup BITMAPINFO
		BITMAPINFO	bmi;
		ZeroMemory(&bmi.bmiHeader, sizeof(BITMAPINFOHEADER));
		bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
		bmi.bmiHeader.biWidth = width;
		bmi.bmiHeader.biHeight = -height;
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB; //BI_BITFIELDS;
		bmi.bmiHeader.biSizeImage = width * height * 4;

		// Copy saved image info into pBits array
		pBits = new BYTE[bmi.bmiHeader.biSizeImage];
		for (y = 0; y < (int)height; y++) {
			for (x = 0; x < (int)width; x++) {
				// image record is stored internally in ABGR format
				alpha = (sel_template->second.pixel[y*width + x] >> 24) & 0xff;
				red = (sel_template->second.pixel[y*width + x] >> 0) & 0xff;
				green = (sel_template->second.pixel[y*width + x] >> 8) & 0xff;
				blue = (sel_template->second.pixel[y*width + x] >> 16) & 0xff;

				// SetDIBits format is BGRA
				pBits[y*width * 4 + x * 4 + 0] = blue;
				pBits[y*width * 4 + x * 4 + 1] = green;
				pBits[y*width * 4 + x * 4 + 2] = red;
				pBits[y*width * 4 + x * 4 + 3] = alpha;
			}
		}
		::SetDIBits(hdcCompatible, bitmap_image, 0, height, pBits, &bmi, DIB_RGB_COLORS);

		Mat template_mat(height, width, CV_8UC4);
		template_mat.data = pBits;
		//imshow("Template Image", template_mat);
		//waitKey();

		roi = detectTemplate(area_mat, template_mat, match_mode);
		if (!EqualRect(&roi, &zero_rect)) {
			if (area_name == "area_cards_common") {
				CString value = sel_template->second.name;
				value.Replace("card_common_", "");
				pair<int, CString> matchPair(roi.left, value);
				listROI.push_back(matchPair);
			}
			if (area_name.Find("area_cards_player") != -1) {
				CString value = sel_template->second.name;
				value.Replace("card_player_", "");
				pair<int, CString> matchPair(roi.left, value);
				listROI.push_back(matchPair);
			}
		}
		// Clean up
		SelectObject(hdcCompatible, old_bitmap);
		DeleteObject(bitmap_image);
		// Free memory
		delete []pBits;
	}

	// Order ROI array
	sort(listROI.begin(), listROI.end());

	// Display result
	vector<CString> result;
	for (auto & element : listROI) {
		result.push_back(element.second);
	}
	//m_Result.SetWindowText(result);


	// Clean up
	__HDC_FOOTER_ATTENTION_HAS_TO_BE_CALLED_ON_EVERY_FUNCTION_EXIT_OTHERWISE_MEMORY_LEAK

	return result;
}

//////////////////////////////////////////////////////////////////////////
