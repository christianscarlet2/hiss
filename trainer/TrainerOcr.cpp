#include "stdafx.h"
#include "TrainerOcr.h"

using namespace cv;
using namespace tesseract;

// --- Constants ported verbatim from Vision/DialogTableMap.cpp ---------------
static const int kMatWidth = 268;
static const int kMatHeight = 115;
static const int kOcrScaleUpFactor = 3;
static const int kOcrViewScaleFactor = 1;
static const double kOcrSharpenSigma = 1.0;
static const int kOcrCharSpacingPx = 6;
static const int kTOcrDefaultThreshold = 65;
static const int kTOcrDefaultCropSize = 30;
static const char *kGeneralWhitelist =
	"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._";

STrainerOcrSettings DefaultOcrSettings()
{
	STrainerOcrSettings s;
	s.use_default = true;
	s.threshold = kTOcrDefaultThreshold;
	s.page_seg_mode = (int)PSM_SINGLE_COLUMN;
	s.use_cropping = false;
	s.crop_size = kTOcrDefaultCropSize;
	s.sharpen = 100;
	s.char_spacing = kOcrCharSpacingPx;
	s.transform = "AutoOcr0";
	s.no_preprocess = false;
	s.no_whitelist = false;
	s.use_decimal_split = false;
	return s;
}

static std::string StripAllWhitespace(const char *str)
{
	std::string out;
	if (str == NULL) return out;
	for (const char *c = str; *c; ++c) {
		if (*c != ' ' && *c != '\t' && *c != '\r' && *c != '\n' && *c != '\f' && *c != '\v') {
			out += *c;
		}
	}
	return out;
}

// Balance fields show "2.28 BB"; drop a trailing "BB" (or "88" misread of BB).
static CString StripBalanceUnitSuffix(CString s)
{
	s.Trim();
	int n = s.GetLength();
	int end = n;
	while (end > 0 && (s[end - 1] == 'B' || s[end - 1] == 'b')) end--;
	if (end < n) {
		s = s.Left(end);
	} else if (s.GetLength() >= 2 && s.Right(2) == "88") {
		CString candidate = s.Left(s.GetLength() - 2);
		if (candidate.FindOneOf("0123456789") != -1) s = candidate;
	}
	s.Trim(" .");
	return s;
}

CTrainerOcr::CTrainerOcr()
{
	_api = NULL;
	_api2 = NULL;
	_ready = false;
	_last_conf = 0;
	_did_split = false;
	_s = DefaultOcrSettings();
}

CTrainerOcr::~CTrainerOcr()
{
	if (_api != NULL) { _api->End(); delete _api; _api = NULL; }
	if (_api2 != NULL) { _api2->End(); delete _api2; _api2 = NULL; }
}

bool CTrainerOcr::Init(const char *tessdata_dir, const char *lang)
{
	// Re-entrant: tear down any previously loaded model so the user can switch
	// models at runtime via "Select Model..." without leaking the old API.
	if (_api != NULL) { _api->End(); delete _api; _api = NULL; }
	if (_api2 != NULL) { _api2->End(); delete _api2; _api2 = NULL; }
	_ready = false;

	_api = new TessBaseAPI();
	_api2 = new TessBaseAPI();
	bool ok1 = (_api->Init(tessdata_dir, lang) == 0);
	bool ok2 = (_api2->Init(tessdata_dir, lang) == 0);
	_ready = ok1 && ok2;
	_model = (lang != NULL) ? CString(lang) : CString("");
	return _ready;
}

Mat CTrainerOcr::binarize_array_opencv(Mat image, int threshold)
{
	Mat img;
	cvtColor(image, img, COLOR_BGR2RGB);
	cvtColor(img, img, COLOR_BGR2GRAY);
	Mat thresh, blur;
	cv::threshold(img, thresh, threshold, 255, THRESH_BINARY_INV);
	float kernel_data[9] = { 1, 2, 1, 2, 4, 2, 1, 2, 1 };
	Mat kernel = Mat(1, 1, CV_32F, kernel_data);
	cv::filter2D(thresh, blur, -1, kernel);
	Mat ret;
	cv::threshold(blur, ret, 250, 255, THRESH_BINARY);
	return ret;
}

static Mat AddCharacterSpacing(const Mat &binary, int gap_px)
{
	if (binary.empty() || binary.channels() != 1 || gap_px <= 0) return binary;
	try {
		int total = binary.rows * binary.cols;
		int white = countNonZero(binary);
		uchar fg = (white <= total - white) ? 255 : 0;
		Mat ink;
		compare(binary, fg, ink, CMP_EQ);
		Mat col_has_ink;
		reduce(ink, col_has_ink, 0, REDUCE_MAX);

		std::vector<std::pair<int, int> > spans;
		bool in_char = false;
		int start = 0;
		for (int c = 0; c < binary.cols; c++) {
			bool has_ink = col_has_ink.at<uchar>(0, c) > 0;
			if (has_ink && !in_char) { in_char = true; start = c; }
			else if (!has_ink && in_char) { in_char = false; spans.push_back(std::make_pair(start, c)); }
		}
		if (in_char) spans.push_back(std::make_pair(start, binary.cols));
		if (spans.size() <= 1) return binary;

		uchar bg = (uchar)(255 - fg);
		int extra = gap_px * (int)(spans.size() - 1);
		Mat out(binary.rows, binary.cols + extra, binary.type(), Scalar(bg));
		int read_x = 0, write_x = 0;
		for (size_t i = 0; i < spans.size(); i++) {
			int seg_w = spans[i].second - read_x;
			binary(Rect(read_x, 0, seg_w, binary.rows)).copyTo(out(Rect(write_x, 0, seg_w, binary.rows)));
			write_x += seg_w;
			read_x = spans[i].second;
			if (i + 1 < spans.size()) write_x += gap_px;
		}
		if (read_x < binary.cols) {
			int seg_w = binary.cols - read_x;
			binary(Rect(read_x, 0, seg_w, binary.rows)).copyTo(out(Rect(write_x, 0, seg_w, binary.rows)));
		}
		return out;
	}
	catch (const cv::Exception &) {
		return binary;
	}
}

Mat CTrainerOcr::ScaleOcrViewImage(Mat img_bounded)
{
	if (img_bounded.empty() || kOcrViewScaleFactor == kOcrScaleUpFactor) return img_bounded;
	Mat scaled;
	double f = (double)kOcrViewScaleFactor / kOcrScaleUpFactor;
	resize(img_bounded, scaled, Size(), f, f, INTER_AREA);
	return scaled;
}

Mat CTrainerOcr::buildOcrImage(Mat img_orig, bool binarize, int threshold)
{
	// "No preprocessing": hand the untouched crop straight to Tesseract -- no
	// grayscale, resize, binarize or character-spacing. (Run() also forces
	// word-box cropping off in this mode.) This is what the "No preprocessing"
	// checkbox promises; the threshold/spacing controls simply don't apply here.
	// The page-seg mode is still set in process_ocr, so "Mode" works either way.
	if (_s.no_preprocess) {
		return img_orig.clone();
	}

	Mat img_resized;
	int basewidth, hsize;
	float wpercent;
	if (img_orig.cols > img_orig.rows * 1.25) {
		basewidth = kMatWidth * kOcrScaleUpFactor;
		wpercent = (basewidth / (float)img_orig.cols);
		hsize = (int)((float)img_orig.rows * wpercent);
	} else {
		hsize = kMatHeight * kOcrScaleUpFactor;
		wpercent = (hsize / (float)img_orig.rows);
		basewidth = (int)((float)img_orig.cols * wpercent);
	}
	cvtColor(img_orig, img_resized, COLOR_BGR2GRAY);
	resize(img_resized, img_resized, Size(basewidth, hsize), INTER_LANCZOS4);
	if (binarize) {
		img_resized = binarize_array_opencv(img_resized, threshold);
		img_resized = AddCharacterSpacing(img_resized, _s.char_spacing);
	}
	return img_resized;
}

Mat CTrainerOcr::prepareImage(Mat img_orig, bool binarize, int threshold, bool second_pass)
{
	Mat img_ocr = buildOcrImage(img_orig, binarize, threshold);
	process_ocr(img_ocr, second_pass);

	// Preview = exactly what Tesseract received. The normal pipeline yields a
	// 1-channel (gray/binary) image we colour-convert for display and shrink back
	// to preview scale; the raw "no preprocessing" crop is already BGR at region
	// scale, so show it as-is.
	if (img_ocr.channels() == 1) {
		Mat img_bounded;
		img_ocr.convertTo(img_bounded, CV_8UC1);
		cvtColor(img_bounded, img_bounded, COLOR_GRAY2BGR);
		return ScaleOcrViewImage(img_bounded);
	}
	return img_ocr.clone();
}

void CTrainerOcr::process_ocr(Mat img_orig, bool second_pass)
{
	if (!_ready) return;
	PageSegMode page_seg_mode = (PageSegMode)_s.page_seg_mode;   // always applied
	// Disabling the whitelist clears it entirely (""), so Tesseract may emit any
	// character. Otherwise restrict to the per-region set chosen in Run().
	const char *wl = _s.no_whitelist ? "" : (_whitelist.IsEmpty() ? kGeneralWhitelist : _whitelist.GetString());

	_api->SetPageSegMode(page_seg_mode);
	_api->SetVariable("user_defined_dpi", "300");
	_api->SetVariable("tessedit_char_whitelist", wl);
	_api->SetImage(img_orig.data, img_orig.cols, img_orig.rows, img_orig.channels(), (int)img_orig.step);
	_api->Recognize(0);

	char *t = _api->GetUTF8Text();
	CString s = StripAllWhitespace(t).c_str();
	if (t) delete[] t;
	if (second_pass) _result2 = s; else _result = s;
	_last_conf = _api->MeanTextConf();
	_api->Clear();
	_api2->Clear();
}

static Mat ToBgr(const Mat &m)
{
	if (m.empty()) return m;
	Mat out;
	if (m.channels() == 1) {
		Mat u8; m.convertTo(u8, CV_8UC1);
		cvtColor(u8, out, COLOR_GRAY2BGR);
	} else if (m.channels() == 4) {
		cvtColor(m, out, COLOR_BGRA2BGR);
	} else {
		out = m.clone();
	}
	return out;
}

// Locate the decimal separator (the small ink blob sitting low on the baseline,
// between the tall digits). Mirrors user_scripts/split_at_decimal.py: binarize,
// connected components, measure the digit band, then pick the small/low/square
// component; the right-most one wins (the decimal follows any grouping commas).
bool CTrainerOcr::FindDecimalSplit(const Mat &img, int *out_left, int *out_right)
{
	if (img.empty()) return false;

	Mat gray;
	if (img.channels() == 1) gray = img;
	else if (img.channels() == 4) cvtColor(img, gray, COLOR_BGRA2GRAY);
	else cvtColor(img, gray, COLOR_BGR2GRAY);

	Mat th;
	cv::threshold(gray, th, 0, 255, THRESH_BINARY | THRESH_OTSU);
	if (countNonZero(th) > (int)(th.total() / 2)) bitwise_not(th, th);   // ink = white minority

	Mat labels, stats, centroids;
	int n = connectedComponentsWithStats(th, labels, stats, centroids, 8, CV_32S);

	struct Comp { int x, y, w, h; };
	std::vector<Comp> comps;
	int max_h = 0;
	for (int i = 1; i < n; i++) {
		int w = stats.at<int>(i, CC_STAT_WIDTH);
		int h = stats.at<int>(i, CC_STAT_HEIGHT);
		int a = stats.at<int>(i, CC_STAT_AREA);
		if (a >= 2 && w >= 1 && h >= 1) {
			Comp c; c.x = stats.at<int>(i, CC_STAT_LEFT); c.y = stats.at<int>(i, CC_STAT_TOP);
			c.w = w; c.h = h;
			comps.push_back(c);
			if (h > max_h) max_h = h;
		}
	}
	if (comps.empty()) return false;

	// Digit band from the tall components.
	int baseline = 0, cap_top = gray.rows;
	bool have_digit = false;
	for (size_t i = 0; i < comps.size(); i++) {
		if (comps[i].h >= 0.6 * max_h) {
			have_digit = true;
			baseline = max(baseline, comps[i].y + comps[i].h);
			cap_top = min(cap_top, comps[i].y);
		}
	}
	if (!have_digit) { baseline = 0; cap_top = gray.rows;
		for (size_t i = 0; i < comps.size(); i++) {
			baseline = max(baseline, comps[i].y + comps[i].h);
			cap_top = min(cap_top, comps[i].y);
		}
	}
	double band = max(1, baseline - cap_top);

	int best_right = -1, bx = 0, bw = 0;
	for (size_t i = 0; i < comps.size(); i++) {
		const Comp &c = comps[i];
		double cy = c.y + c.h / 2.0;
		double ar = c.w / (double)c.h;
		bool is_short  = c.h <= 0.55 * band;
		bool narrow    = c.w <= 0.70 * band;
		bool low       = cy >= cap_top + 0.45 * band;
		bool onbase    = (c.y + c.h) >= baseline - 0.30 * band;
		bool squarish  = ar >= 0.4 && ar <= 2.5;
		if (is_short && narrow && low && onbase && squarish) {
			int rt = c.x + c.w;
			if (rt > best_right) { best_right = rt; bx = c.x; bw = c.w; }
		}
	}
	if (best_right < 0) return false;

	*out_left = bx;
	*out_right = bx + bw;
	return true;
}

// Left | red-marker | right, so the split is visible in the main view. Scaled to
// the normal preview size unless we're showing the raw (no-preprocess) crop.
Mat CTrainerOcr::BuildSplitPreview(const Mat &left, const Mat &right)
{
	Mat L = ToBgr(left), R = ToBgr(right);
	Mat preview;
	if (L.empty() && R.empty()) return preview;
	if (R.empty()) {
		preview = L;
	} else if (L.empty()) {
		preview = R;
	} else {
		Mat sep(L.rows, 4, CV_8UC3, Scalar(0, 0, 255));   // red split marker
		std::vector<Mat> parts; parts.push_back(L); parts.push_back(sep); parts.push_back(R);
		hconcat(parts, preview);
	}
	if (!_s.no_preprocess && !preview.empty()) {
		preview = ScaleOcrViewImage(preview);
	}
	return preview;
}

// Build the OCR-input image, split it at the decimal, OCR each half with the
// current settings, and join the two halves with a "." -- Tesseract is far more
// reliable on single digit-runs than on a number containing a tiny decimal dot.
bool CTrainerOcr::RunDecimalSplit(const Mat &crop_bgr, int threshold,
	Mat *preview_bgr, CString *text, int *mean_conf)
{
	Mat ocr_img = buildOcrImage(crop_bgr, true, threshold);
	if (ocr_img.empty()) return false;

	// Decimal detection needs resolution: under "no preprocessing" the OCR image is
	// the small raw crop, whose decimal dot is too few pixels for the connected-
	// component heuristics. Detect on an upscaled copy and map the split columns
	// back to the OCR image. (The normal pipeline is already large, so scale == 1
	// and detection runs on the OCR image unchanged.)
	const int kDetectMinHeight = 120;
	double scale = 1.0;
	Mat detect_img = ocr_img;
	if (ocr_img.rows > 0 && ocr_img.rows < kDetectMinHeight) {
		scale = (double)kDetectMinHeight / ocr_img.rows;
		resize(ocr_img, detect_img, Size(), scale, scale, INTER_LANCZOS4);
	}

	int dleft = 0, dright = 0;
	if (!FindDecimalSplit(detect_img, &dleft, &dright) || dleft <= 0 || dright >= detect_img.cols) {
		// No decimal: let Run() OCR the whole image normally.
		return false;
	}

	// Map the decimal's column span back to the OCR image and cut it out with a
	// small safety margin, so neither half keeps any of the decimal's pixels.
	int oleft  = (int)(dleft  / scale + 0.5);
	int oright = (int)(dright / scale + 0.5);
	if (oleft <= 0 || oright >= ocr_img.cols || oleft >= oright) return false;
	// Small margin only -- just enough to swallow rounding/aliasing around the
	// decimal. A larger margin eats the gap and pushes the first right-side digit
	// against the image edge, which Tesseract then misreads.
	int margin = max(1, (int)((oright - oleft) * 0.10 + 0.5));
	int cut_l = max(1, oleft - margin);
	int cut_r = min(ocr_img.cols - 1, oright + margin);
	if (cut_l >= cut_r) return false;

	Mat leftImg  = ocr_img.colRange(0, cut_l).clone();
	Mat rightImg = ocr_img.colRange(cut_r, ocr_img.cols).clone();

	process_ocr(leftImg, false);    CString l = _result;  int confL = _last_conf;
	process_ocr(rightImg, true);    CString r = _result2; int confR = _last_conf;
	// Each half is one side of the decimal; drop any stray dot Tesseract emits.
	l.Remove('.'); r.Remove('.'); l.Trim(); r.Trim();
	// The decimal is always the joint between the two halves' OCR results.
	CString combined = l + "." + r;
	_did_split = true; _split_left = l; _split_right = r;

	Mat preview = BuildSplitPreview(leftImg, rightImg);
	if (preview_bgr) *preview_bgr = preview;
	if (text) *text = combined;
	if (mean_conf) *mean_conf = min(confL, confR);
	return true;
}

void CTrainerOcr::Run(const Mat &crop_bgr, const STrainerOcrSettings &settings,
	const CString &region_name, Mat *preview_bgr, CString *text, int *mean_conf)
{
	_s = settings;
	// Word-box cropping is itself a preprocessing step; skip it (and its empty-box
	// fallthrough that would blank the result) when preprocessing or decimal
	// splitting is active.
	if (_s.no_preprocess || _s.use_decimal_split) _s.use_cropping = false;
	_boxes.clear(); _boxes2.clear();
	_result = _result2 = "";
	_bestRect = Rect(); _bestRect2 = Rect();
	_last_conf = 0;
	_did_split = false;
	_split_left = ""; _split_right = "";

	if (preview_bgr) *preview_bgr = Mat();
	if (text) *text = "";
	if (mean_conf) *mean_conf = 0;

	if (!_ready || crop_bgr.empty()) return;

	int threshold = (_s.threshold > 0) ? _s.threshold : kTOcrDefaultThreshold;

	// Balance regions: digits + dot only. Others: general set.
	bool is_balance = (CString(region_name).MakeLower().Find("balance") != -1);
	if (is_balance)
		_whitelist = "0123456789.";
	else
		_whitelist = kGeneralWhitelist;

	// Decimal-splitting path: split, OCR each half, join with ".". If no decimal
	// is found it returns false and we fall through to normal whole-image OCR.
	if (_s.use_decimal_split &&
		RunDecimalSplit(crop_bgr, threshold, preview_bgr, text, mean_conf)) {
		return;
	}

	Mat img_resized = prepareImage(crop_bgr, true, threshold, false);
	Mat img_resized2 = prepareImage(crop_bgr, true, threshold, true);

	CString ocr_result = _result;
	CString ocr_result2 = _result2;

	// Clean noise but never strip valid OCR characters.
	const char *blacklist = "!%&*+;=?@^/\"`#<{([])}>|";
	for (size_t i = 0; i < strlen(blacklist); i++) {
		char c = blacklist[i];
		if (ocr_result.Find(c) != -1) ocr_result.Replace(c, '\0');
		if (ocr_result2.Find(c) != -1) ocr_result2.Replace(c, '\0');
	}

	if (CString(region_name).MakeLower().Find("balance") != -1) {
		ocr_result = StripBalanceUnitSuffix(ocr_result);
		ocr_result2 = StripBalanceUnitSuffix(ocr_result2);
	}

	CString final_text;
	Mat preview = img_resized;
	if (!ocr_result.IsEmpty()) {
		final_text = ocr_result;
		preview = img_resized;
	} else if (!ocr_result2.IsEmpty()) {
		final_text = ocr_result2;
		preview = img_resized2;
	} else {
		final_text = "";
	}

	if (preview_bgr) *preview_bgr = preview;
	if (text) *text = final_text;
	if (mean_conf) *mean_conf = _last_conf;
}
