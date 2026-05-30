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
static const int kDefaultAutoOcrThreshold = 65;
static const int kDefaultCropSize = 30;
static const char *kGeneralWhitelist =
	"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._";

STrainerOcrSettings DefaultOcrSettings()
{
	STrainerOcrSettings s;
	s.use_default = true;
	s.threshold = kDefaultAutoOcrThreshold;
	s.page_seg_mode = (int)PSM_SINGLE_COLUMN;
	s.use_cropping = false;
	s.crop_size = kDefaultCropSize;
	s.sharpen = 100;
	s.transform = "AutoOcr0";
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
	_s = DefaultOcrSettings();
}

CTrainerOcr::~CTrainerOcr()
{
	if (_api != NULL) { _api->End(); delete _api; _api = NULL; }
	if (_api2 != NULL) { _api2->End(); delete _api2; _api2 = NULL; }
}

bool CTrainerOcr::Init(const char *tessdata_dir, const char *lang)
{
	_api = new TessBaseAPI();
	_api2 = new TessBaseAPI();
	bool ok1 = (_api->Init(tessdata_dir, lang) == 0);
	bool ok2 = (_api2->Init(tessdata_dir, lang) == 0);
	_ready = ok1 && ok2;
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

Mat CTrainerOcr::prepareImage(Mat img_orig, bool binarize, int threshold, bool second_pass)
{
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

	double sharpen_amount = _s.sharpen / 100.0;
	if (sharpen_amount > 0.0) {
		Mat blurred;
		GaussianBlur(img_resized, blurred, Size(0, 0), kOcrSharpenSigma);
		addWeighted(img_resized, 1.0 + sharpen_amount, blurred, -sharpen_amount, 0, img_resized);
	}

	if (binarize) {
		img_resized = binarize_array_opencv(img_resized, threshold);
		img_resized = AddCharacterSpacing(img_resized, kOcrCharSpacingPx);
	}

	Mat img_bounded = img_resized.clone();
	img_bounded.convertTo(img_bounded, CV_8UC3);
	cvtColor(img_bounded, img_bounded, COLOR_GRAY2BGR);
	const Scalar previewColor(255, 255, 255);

	if (_s.use_cropping) {
		double cropSize = (double)_s.crop_size / 100.0;
		if (cropSize < 0.01) return ScaleOcrViewImage(img_bounded);

		process_ocr(img_resized, second_pass);
		std::vector<std::pair<Rect, CString> > resBoxes = second_pass ? _boxes2 : _boxes;
		if (resBoxes.empty()) return ScaleOcrViewImage(img_bounded);

		std::vector<Rect> boundRect, boundRect2;
		for (size_t idx = 0; idx < resBoxes.size(); idx++) {
			Rect rect = resBoxes[idx].first;
			if (rect.area() > 50) boundRect.push_back(rect);
		}
		if (boundRect.empty()) return ScaleOcrViewImage(img_bounded);

		std::vector<double> boxArea, boxDist;
		double wCenter = img_bounded.cols / 2;
		double hCenter = img_bounded.rows / 2;
		Point pCenter((int)wCenter, (int)hCenter);
		Rect best_rect = Rect();
		if (second_pass) _bestRect2 = Rect(); else _bestRect = Rect();
		for (size_t i = 0; i < boundRect.size(); i++)
			boxArea.push_back(boundRect[i].width * boundRect[i].height);
		std::vector<double>::iterator ita = max_element(boxArea.begin(), boxArea.end());
		int maxArea = (int)*ita;
		std::vector<int> maxIndex;
		for (size_t i = 0; i < boxArea.size(); i++)
			if (boxArea[i] > maxArea * (1 - cropSize)) maxIndex.push_back((int)i);
		for (size_t i = 0; i < maxIndex.size(); i++) {
			int j = maxIndex[i];
			best_rect = boundRect[j];
			rectangle(img_bounded, best_rect, previewColor, 1);
			boundRect2.push_back(best_rect);
		}
		if (boundRect2.size() > 1) {
			for (size_t i = 0; i < boundRect2.size(); i++) {
				double wc = boundRect2[i].x + boundRect2[i].width / 2;
				double hc = boundRect2[i].y + boundRect2[i].height / 2;
				Point pc((int)wc, (int)hc);
				boxDist.push_back(abs(norm(pCenter - pc)));
			}
			std::vector<double>::iterator itd = min_element(boxDist.begin(), boxDist.end());
			int minDist = (int)*itd;
			for (size_t i = 0; i < boxDist.size(); i++)
				if (boxDist[i] == minDist) { best_rect = boundRect2[i]; break; }
		}
		if (second_pass) _bestRect2 = best_rect; else _bestRect = best_rect;
		rectangle(img_bounded, best_rect, previewColor, 2);
		return ScaleOcrViewImage(img_bounded);
	}

	process_ocr(img_resized, second_pass);
	return ScaleOcrViewImage(img_bounded);
}

void CTrainerOcr::process_ocr(Mat img_orig, bool second_pass)
{
	if (!_ready) return;
	PageSegMode page_seg_mode = (PageSegMode)(_s.use_default ? (int)PSM_SINGLE_COLUMN : _s.page_seg_mode);
	const char *wl = _whitelist.IsEmpty() ? kGeneralWhitelist : _whitelist.GetString();

	_api->SetPageSegMode(page_seg_mode);
	_api->SetVariable("user_defined_dpi", "300");
	_api->SetVariable("tessedit_char_whitelist", wl);
	_api->SetImage(img_orig.data, img_orig.cols, img_orig.rows, img_orig.channels(), (int)img_orig.step);
	_api->Recognize(0);

	if (_s.use_cropping) {
		ResultIterator *ri = _api->GetIterator();
		PageIteratorLevel level = RIL_WORD;
		if (ri != 0) {
			do {
				int x1, y1, x2, y2;
				ri->BoundingBox(level, &x1, &y1, &x2, &y2);
				Mat img_cropped;
				try {
					img_cropped = img_orig(Rect(x1, y1, x2 - x1, y2 - y1));
				} catch (std::exception &) {
					continue;
				}
				_api2->SetPageSegMode(page_seg_mode);
				_api2->SetVariable("user_defined_dpi", "300");
				_api2->SetVariable("tessedit_char_whitelist", wl);
				_api2->SetImage(img_cropped.data, img_cropped.cols, img_cropped.rows, img_cropped.channels(), (int)img_cropped.step);
				_api2->Recognize(0);
				char *t2 = _api2->GetUTF8Text();
				CString word = StripAllWhitespace(t2).c_str();
				if (t2) delete[] t2;
				std::pair<Rect, CString> matchPair(Rect(x1, y1, x2 - x1, y2 - y1), word);
				if (second_pass) _boxes2.push_back(matchPair); else _boxes.push_back(matchPair);
			} while (ri->Next(level));
		}
		_last_conf = 100;   // per-word; treat crop mode as confident
	} else {
		char *t = _api->GetUTF8Text();
		CString s = StripAllWhitespace(t).c_str();
		if (t) delete[] t;
		if (second_pass) _result2 = s; else _result = s;
		_last_conf = _api->MeanTextConf();
	}
	_api->Clear();
	_api2->Clear();
}

void CTrainerOcr::Run(const Mat &crop_bgr, const STrainerOcrSettings &settings,
	const CString &region_name, Mat *preview_bgr, CString *text, int *mean_conf)
{
	_s = settings;
	_boxes.clear(); _boxes2.clear();
	_result = _result2 = "";
	_bestRect = Rect(); _bestRect2 = Rect();
	_last_conf = 0;

	if (preview_bgr) *preview_bgr = Mat();
	if (text) *text = "";
	if (mean_conf) *mean_conf = 0;

	if (!_ready || crop_bgr.empty()) return;

	int threshold = _s.use_default ? kDefaultAutoOcrThreshold : _s.threshold;

	// Balance regions: digits + dot only. Others: general set.
	if (CString(region_name).MakeLower().Find("balance") != -1)
		_whitelist = "0123456789.";
	else
		_whitelist = kGeneralWhitelist;

	Mat img_resized = prepareImage(crop_bgr, true, threshold, false);
	Mat img_resized2 = prepareImage(crop_bgr, true, threshold, true);

	CString ocr_result, ocr_result2;
	if (_s.use_cropping) {
		for (size_t i = 0; i < _boxes.size(); i++)
			if (_boxes[i].first == _bestRect) { ocr_result = _boxes[i].second; break; }
		for (size_t i = 0; i < _boxes2.size(); i++)
			if (_boxes2[i].first == _bestRect2) { ocr_result2 = _boxes2[i].second; break; }
	} else {
		ocr_result = _result;
		ocr_result2 = _result2;
	}

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
