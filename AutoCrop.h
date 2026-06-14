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
#include <vector>
#include <climits>
#include <algorithm>
#include <opencv2/imgproc.hpp>   // connectedComponentsWithStats

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
//   - blank_if_unmatched == true:  an all-white image (same size/type) is returned
//     AND *out_blanked is set to true. OCR callers should check out_blanked and return
//     an EMPTY result WITHOUT running OCR -- feeding a white image to some models (e.g.
//     the bets model) makes them hallucinate a value ("1 BB"), so the white image is
//     only useful as a visual placeholder (e.g. Vision's preview); it must not be OCR'd.
inline cv::Mat AutoCropToColors(const cv::Mat &img, bool master_enabled,
	const SAutoCropColor cols[3], bool blank_if_unmatched = false,
	bool *out_blanked = nullptr)
{
	if (out_blanked) *out_blanked = false;
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

	// Build a binary mask of colour-matching pixels.
	cv::Mat mask(img.rows, img.cols, CV_8U, cv::Scalar(0));
	bool any_match = false;
	for (int y = 0; y < img.rows; ++y) {
		const unsigned char *row = img.ptr<unsigned char>(y);
		unsigned char *mrow = mask.ptr<unsigned char>(y);
		for (int x = 0; x < img.cols; ++x) {
			int b = row[x * ch + 0], g = row[x * ch + 1], r = row[x * ch + 2];
			bool match = false;
			for (int k = 0; k < 3 && !match; ++k) {
				if (!cols[k].enabled) continue;
				int dr = r - tr[k], dg = g - tg[k], db = b - tb[k];
				int dist = (int)sqrt((double)(dr * dr + dg * dg + db * db));
				if (dist <= cols[k].tolerance) match = true;
			}
			if (match) { mrow[x] = 255; any_match = true; }
		}
	}
	if (!any_match) {
		// Nothing matched.
		if (blank_if_unmatched) {
			if (out_blanked) *out_blanked = true;
			return cv::Mat(img.rows, img.cols, img.type(), cv::Scalar::all(255));
		}
		return img;   // leave the scrape unchanged
	}

	// Cluster the matches so a stray colour match elsewhere in the region can't
	// inflate the crop. Connected components, anchor on the largest blob (the bet
	// text/chip), then region-grow to neighbouring blobs (the other digits / "BB")
	// while EXCLUDING components that sit far from that cluster (the stray matches).
	cv::Mat labels, stats, cent;
	int n = cv::connectedComponentsWithStats(mask, labels, stats, cent, 8, CV_32S);
	if (n <= 1) {                          // only background -> treat as no match
		if (blank_if_unmatched) {
			if (out_blanked) *out_blanked = true;
			return cv::Mat(img.rows, img.cols, img.type(), cv::Scalar::all(255));
		}
		return img;
	}
	int anchor = -1, amax = 0;
	for (int k = 1; k < n; ++k) {
		int a = stats.at<int>(k, cv::CC_STAT_AREA);
		if (a > amax) { amax = a; anchor = k; }
	}
	int ah = stats.at<int>(anchor, cv::CC_STAT_HEIGHT);
	int minArea = std::max(2, amax / 50);          // drop tiny specks
	int gapx = std::max(4, ah * 2);                // horizontal reach (inter-glyph + space)
	int gapy = std::max(3, ah);                    // vertical reach (same text line)
	std::vector<char> keep(n, 0);
	keep[anchor] = 1;
	bool changed = true;
	while (changed) {
		changed = false;
		int Kx1 = INT_MAX, Ky1 = INT_MAX, Kx2 = -1, Ky2 = -1;
		for (int k = 1; k < n; ++k) {
			if (!keep[k]) continue;
			int x1 = stats.at<int>(k, cv::CC_STAT_LEFT), y1 = stats.at<int>(k, cv::CC_STAT_TOP);
			int x2 = x1 + stats.at<int>(k, cv::CC_STAT_WIDTH) - 1, y2 = y1 + stats.at<int>(k, cv::CC_STAT_HEIGHT) - 1;
			Kx1 = std::min(Kx1, x1); Ky1 = std::min(Ky1, y1);
			Kx2 = std::max(Kx2, x2); Ky2 = std::max(Ky2, y2);
		}
		for (int k = 1; k < n; ++k) {
			if (keep[k]) continue;
			if (stats.at<int>(k, cv::CC_STAT_AREA) < minArea) continue;
			int x1 = stats.at<int>(k, cv::CC_STAT_LEFT), y1 = stats.at<int>(k, cv::CC_STAT_TOP);
			int x2 = x1 + stats.at<int>(k, cv::CC_STAT_WIDTH) - 1, y2 = y1 + stats.at<int>(k, cv::CC_STAT_HEIGHT) - 1;
			bool nearx = (x1 <= Kx2 + gapx) && (x2 >= Kx1 - gapx);
			bool neary = (y1 <= Ky2 + gapy) && (y2 >= Ky1 - gapy);
			if (nearx && neary) { keep[k] = 1; changed = true; }
		}
	}
	int minx = img.cols, miny = img.rows, maxx = -1, maxy = -1;
	for (int k = 1; k < n; ++k) {
		if (!keep[k]) continue;
		int x1 = stats.at<int>(k, cv::CC_STAT_LEFT), y1 = stats.at<int>(k, cv::CC_STAT_TOP);
		int x2 = x1 + stats.at<int>(k, cv::CC_STAT_WIDTH) - 1, y2 = y1 + stats.at<int>(k, cv::CC_STAT_HEIGHT) - 1;
		minx = std::min(minx, x1); miny = std::min(miny, y1);
		maxx = std::max(maxx, x2); maxy = std::max(maxy, y2);
	}
	cv::Rect roi(minx, miny, maxx - minx + 1, maxy - miny + 1);
	return img(roi).clone();
}
