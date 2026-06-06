#pragma once
//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: AutoOcr-class for OpenHoldem
//
//******************************************************************************

#ifndef INC_CAUTO_OCR_H
#define INC_CAUTO_OCR_H

#include <vector>
#include <regex>

#include "..\CTablemap\CTablemap.h"
#include "CSpaceOptimizedGlobalObject.h"

using namespace cv;
using namespace tesseract;

struct SAutoOcrSettings {
	int threshold;
	bool use_cropping;
	int crop_size;
	int page_seg_mode;
	int sharpen;	// Unsharp-mask amount in percent (100 = 1.0x); <0 = default
	CString whitelist;	// Tesseract char whitelist; empty = no restriction
	bool no_preprocess;	// skip the resize/char-spacing enhancement (threshold+mode still apply)
	bool no_char_spacing;	// skip only the character-spacing step (ignored when no_preprocess)
	bool auto_threshold;	// true => Otsu auto-threshold (non-balance/text regions) instead of `threshold`
};

// Decimal-split detail from the most recent get_ocr_result(), so the Scraper Output
// window can mimic the trainer (show the left/right halves, their texts, and a red
// split line). did == false when the region was OCR'd whole.
struct SAutoOcrSplit {
	bool did;
	CString left, right;        // recognized text of each half
	cv::Mat left_img, right_img; // processed (binarized) image fed to Tesseract per half
	int split_x;                // separator x in the region's pixel coordinates
	SAutoOcrSplit() : did(false), split_x(0) {}
};

class CAutoOcr : public CSpaceOptimizedGlobalObject {
	friend class CScraper;
public:
	// public functions
	CAutoOcr();
	~CAutoOcr();
public:
	CString GetDetectTemplateResult(CString area_name, CString tpl_name, RECT* rect_result);
	vector<CString> GetDetectTemplatesResult(CString area_name);
	CString get_ocr_result(Mat img_orig, RMapCI region, bool fast = false, SAutoOcrSplit *split_out = NULL);
	// Re-read the per-transform model/threshold/mode settings from the DB (called by the
	// OCR preferences page so a model change takes effect without a restart).
	void LoadModelSettings();
private:
	RECT detectTemplate(Mat area, Mat tpl, int match_mode);
	void process_ocr(Mat img_orig, const SAutoOcrSettings &settings, bool fast = false, bool second_pass = false);
	Mat prepareImage(Mat img_orig, const SAutoOcrSettings &settings, bool binarize = true, int threshold = 100, bool second_pass = false);
	Mat binarize_array_opencv(Mat image, int threshold, bool auto_threshold = false);
	COLORREF AverageFourByFour(Mat img, int center_x, int center_y);
	bool ReadColorPresetColor(int index, COLORREF *color);
	bool ReadColorPresetSamplePoint(int index, RMapCI region, int *rel_x, int *rel_y);
	bool TryColorPresetSettings(Mat img_orig, RMapCI region, SAutoOcrSettings *settings);
	bool EnsureTesseractInitialized();
	// Per-transform Tesseract model selection (A0 -> a0 model, A1 -> a1 model),
	// read from the `settings` table. Models are switched lazily; a re-Init only
	// happens when the required model actually changes. (LoadModelSettings is public.)
	bool EnsureModelLoaded(const CString &model);

	string trim(string str) {
		return regex_replace(str, regex("\\s"), "");
	}

	float convertTofloat(const string& str) {
		float result;
		istringstream iss(str);
		iss >> result;
		return result;
	}

private:
	// Counter of GDI objects (potential memorz leak)
	// Should be 0 at end of program -- will be checked.
	int         _leaking_GDI_objects;
	bool        _api_initialized;
	bool        _api2_initialized;
	bool        _api_init_failed;
	bool        _models_loaded;
	// The two Tesseract engines. These were file-scope globals; moving them to
	// instance members lets several independent CAutoOcr engines run on parallel
	// worker threads (each owns its own api/api2 and result state).
	TessBaseAPI *api;
	TessBaseAPI *api2;
	// Per-transform OCR settings, indexed by AutoOcr engine number (A0=0, A1=1, A2=2).
	// (settings table keys autoocr0 / autoocr1 / autoocr2.)
	static const int kNumAutoOcr = 3;
	CString     _model[kNumAutoOcr];
	CString     _current_model;   // model currently loaded into api/api2
	int         _thr[kNumAutoOcr];    // binarize threshold
	int         _mode[kNumAutoOcr];   // tesseract page-seg mode (always applied)
	bool        _nopre[kNumAutoOcr];  // skip resize/char-spacing enhancement
	bool        _nowl[kNumAutoOcr];   // disable the char whitelist
	bool        _nocs[kNumAutoOcr];   // disable only the character-spacing step
	bool        _dcorr[kNumAutoOcr];  // post-process decimal correction (re-insert a dropped '.')
	// Map a transform code ("A0"/"A1"/"A2") to an engine index 0..kNumAutoOcr-1.
	static int  AutoOcrIndex(const CString &transform);
	std::vector<CString> _decimal_fields;   // field types that use decimal splitting (Vision Settings > Fields)
	bool RegionUsesDecimalSplit(const CString &region_name);

	vector<pair<Rect, CString>> ResultBoxes, ResultBoxes2;
	CString ResultString, ResultString2;
	Rect	bestRect, bestRect2;

	CCritSec		m_critsec;
};

extern CAutoOcr *p_auto_ocr;
CAutoOcr *AutoOcr();

#endif // INC_CAUTO_OCR_H
