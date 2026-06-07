//******************************************************************************
//
// Auto Cropper - shared helper (header-only).
//
// Crops a scrape image to the bounding box of pixels whose colour matches ANY of
// up to three enabled colour cubes (RGB within each colour's own tolerance). Used
// by Vision (preview), Hiss (live OCR) and the Trainer (Sample Gen capture) so all
// three crop identically. Each colour is a COLORREF packing ARGB (alpha high byte)
// exactly like CTablemap's `color`; alpha is ignored for matching (GDI screenshots
// don't carry reliable alpha), matching the rest of the colour-cube code.
//
// Include this AFTER <opencv2/...> and <windows.h> are available in the TU.
//
//******************************************************************************
#pragma once

#include <cmath>

// One colour-cube filter: target colour (ARGB-packed COLORREF) + tolerance + on.
struct SAutoCropColor {
	COLORREF color;
	int      tolerance;
	bool     enabled;
};

// Returns the sub-image cropped to the bounding box of matching pixels. If the
// master switch is off, no colour is enabled, or the image is empty/too few
// channels, the input image is returned unchanged (a shallow header copy).
//
// When nothing matches:
//   - blank_if_unmatched == false: the input image is returned unchanged.
//   - blank_if_unmatched == true:  an all-white image (same size/type) is returned,
//     so the downstream OCR sees a blank crop and yields no text. Used to suppress a
//     field entirely when its expected colours are absent.
inline cv::Mat AutoCropToColors(const cv::Mat &img, bool master_enabled,
	const SAutoCropColor cols[3], bool blank_if_unmatched = false)
{
	if (!master_enabled || img.empty()) return img;
	int ch = img.channels();
	if (ch < 3) return img;   // need BGR(A)

	int tr[3], tg[3], tb[3];
	bool any_on = false;
	for (int k = 0; k < 3; ++k) {
		if (!cols[k].enabled) { tr[k] = tg[k] = tb[k] = 0; continue; }
		any_on = true;
		tr[k] = GetRValue(cols[k].color);
		tg[k] = GetGValue(cols[k].color);
		tb[k] = GetBValue(cols[k].color);
	}
	if (!any_on) return img;

	int minx = img.cols, miny = img.rows, maxx = -1, maxy = -1;
	for (int y = 0; y < img.rows; ++y) {
		const unsigned char *row = img.ptr<unsigned char>(y);
		for (int x = 0; x < img.cols; ++x) {
			int b = row[x * ch + 0], g = row[x * ch + 1], r = row[x * ch + 2];
			bool match = false;
			for (int k = 0; k < 3 && !match; ++k) {
				if (!cols[k].enabled) continue;
				int dr = r - tr[k], dg = g - tg[k], db = b - tb[k];
				int dist = (int)sqrt((double)(dr * dr + dg * dg + db * db));
				if (dist <= cols[k].tolerance) match = true;
			}
			if (match) {
				if (x < minx) minx = x;
				if (x > maxx) maxx = x;
				if (y < miny) miny = y;
				if (y > maxy) maxy = y;
			}
		}
	}
	if (maxx < minx || maxy < miny) {
		// Nothing matched.
		if (blank_if_unmatched) {
			return cv::Mat(img.rows, img.cols, img.type(), cv::Scalar::all(255));
		}
		return img;   // leave the scrape unchanged
	}
	cv::Rect roi(minx, miny, maxx - minx + 1, maxy - miny + 1);
	return img(roi).clone();
}
