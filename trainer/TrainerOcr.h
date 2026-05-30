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
	CString transform;       // "AutoOcr0" / "AutoOcr1"
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

	// Runs the full pipeline on a BGR crop. Returns the processed preview image
	// (original-region scale), the recognized text, and a mean confidence.
	void Run(const cv::Mat &crop_bgr, const STrainerOcrSettings &settings,
		const CString &region_name, cv::Mat *preview_bgr, CString *text, int *mean_conf);

private:
	cv::Mat prepareImage(cv::Mat img_orig, bool binarize, int threshold, bool second_pass);
	void process_ocr(cv::Mat img_orig, bool second_pass);
	cv::Mat binarize_array_opencv(cv::Mat image, int threshold);
	cv::Mat ScaleOcrViewImage(cv::Mat img_bounded);

	tesseract::TessBaseAPI *_api;
	tesseract::TessBaseAPI *_api2;
	bool _ready;

	// Per-run pipeline state (mirrors Vision's CDlgTableMap members).
	STrainerOcrSettings _s;
	CString _whitelist;
	std::vector<std::pair<cv::Rect, CString> > _boxes, _boxes2;
	CString _result, _result2;
	cv::Rect _bestRect, _bestRect2;
	int _last_conf;
};
