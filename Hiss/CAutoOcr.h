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
	CString get_ocr_result(Mat img_orig, RMapCI region, bool fast = false);
	// Re-read the per-transform model/threshold/mode settings from the DB (called by the
	// OCR preferences page so a model change takes effect without a restart).
	void LoadModelSettings();
private:
	RECT detectTemplate(Mat area, Mat tpl, int match_mode);
	void process_ocr(Mat img_orig, const SAutoOcrSettings &settings, bool fast = false, bool second_pass = false);
	Mat prepareImage(Mat img_orig, const SAutoOcrSettings &settings, bool binarize = true, int threshold = 100, bool second_pass = false);
	Mat binarize_array_opencv(Mat image, int threshold);
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
	CString     _model_a0;
	CString     _model_a1;
	CString     _current_model;   // model currently loaded into api/api2
	// Per-transform OCR settings (settings table keys autoocr0 / autoocr1).
	int         _thr_a0, _thr_a1;       // binarize threshold
	int         _mode_a0, _mode_a1;     // tesseract page-seg mode (always applied)
	bool        _nopre_a0, _nopre_a1;   // skip resize/char-spacing enhancement
	bool        _nowl_a0, _nowl_a1;     // disable the char whitelist
	bool        _nocs_a0, _nocs_a1;     // disable only the character-spacing step
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
