#pragma once

#include <vector>
#include <utility>

// OCR processing settings, mirroring Vision's "Image Processing" controls.
struct STrainerOcrSettings {
	bool    use_default;     // use Vision's default threshold + page-seg mode
	int     threshold;       // binarize threshold (when !use_default)
	int     page_seg_mode;   // tesseract::PageSegMode value (when !use_default)
	bool    use_cropping;    // word-box crop mode
	int     crop_size;       // % diff factor for concurrent boxes
	int     sharpen;         // unsharp amount in percent (100 = 1.0x)
	int     char_spacing;    // blank columns inserted into each glyph gap (upscaled px; 0 = off)
	CString transform;       // "AutoOcr0" / "AutoOcr1"
	bool    no_preprocess;   // feed the raw crop straight to Tesseract (skip resize/sharpen/binarize/spacing/crop)
	bool    no_whitelist;    // do not restrict recognized characters at all
	bool    use_decimal_split; // split the image at the decimal, OCR each half, join with "."
	int     decimal_split_margin_pct; // extra trim each side of the decimal, as % of its width
	bool    use_decimal_correct; // post-process: re-insert a dropped '.' detected in the image
};

STrainerOcrSettings DefaultOcrSettings();

// Self-contained port of Vision's prepareImage/process_ocr/get_ocr_result so
// the capture guess and the live preview use the identical pipeline.
// Tesseract is NOT thread-safe: call only from the UI thread.
class CTrainerOcr {
public:
	CTrainerOcr();
	~CTrainerOcr();

	bool Init(const char *tessdata_dir, const char *lang);
	bool ready() const { return _ready; }
	const CString &model_name() const { return _model; }

	// Per-half results from the most recent Run() when decimal splitting fired.
	bool did_split() const { return _did_split; }
	const CString &split_left() const { return _split_left; }
	const CString &split_right() const { return _split_right; }

	// Decimal-correction post-processing details from the most recent Run() (for "Debug this field").
	bool dcorr_enabled() const { return _dcorr_enabled; }
	int  dcorr_places() const { return _dcorr_places; }
	const CString &dcorr_before() const { return _dcorr_before; }
	const CString &dcorr_after() const { return _dcorr_after; }

	// Runs the full pipeline on a BGR crop. Returns the processed preview image
	// (original-region scale), the recognized text, and a mean confidence.
	void Run(const cv::Mat &crop_bgr, const STrainerOcrSettings &settings,
		const CString &region_name, cv::Mat *preview_bgr, CString *text, int *mean_conf);

private:
	cv::Mat prepareImage(cv::Mat img_orig, bool binarize, int threshold, bool second_pass);
	void process_ocr(cv::Mat img_orig, bool second_pass);
	cv::Mat binarize_array_opencv(cv::Mat image, int threshold);
	cv::Mat ScaleOcrViewImage(cv::Mat img_bounded);

	// Build the exact image fed to Tesseract (raw crop when no_preprocess is set,
	// otherwise grayscale -> upscale -> sharpen -> binarize -> char-spacing).
	cv::Mat buildOcrImage(cv::Mat img_orig, bool binarize, int threshold);
	// Decimal-splitting OCR: find the separator, OCR each half, join with ".".
	bool FindDecimalSplit(const cv::Mat &img, int *out_left, int *out_right);
	// Returns true if a decimal was found and handled; false to let Run() fall
	// back to the normal whole-image OCR path.
	bool RunDecimalSplit(const cv::Mat &crop_bgr, int threshold, bool is_balance,
		cv::Mat *preview_bgr, CString *text, int *mean_conf);
	cv::Mat BuildSplitPreview(const cv::Mat &left, const cv::Mat &right);

	tesseract::TessBaseAPI *_api;
	tesseract::TessBaseAPI *_api2;
	bool _ready;
	CString _model;          // language/model name currently loaded (for status)

	// Per-run pipeline state (mirrors Vision's CDlgTableMap members).
	STrainerOcrSettings _s;
	CString _whitelist;
	std::vector<std::pair<cv::Rect, CString> > _boxes, _boxes2;
	CString _result, _result2;
	cv::Rect _bestRect, _bestRect2;
	int _last_conf;

	// Decimal-split state from the last Run().
	bool _did_split;
	CString _split_left, _split_right;

	// Decimal-correction state from the last Run().
	bool _dcorr_enabled;
	int  _dcorr_places;
	CString _dcorr_before, _dcorr_after;
};
