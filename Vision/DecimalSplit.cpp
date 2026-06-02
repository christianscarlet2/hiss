//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Decimal-separator locator. See DecimalSplit.h. Mirrors the
//   connected-component logic of trainer's CTrainerOcr::FindDecimalSplit.
//
//******************************************************************************

#include "stdafx.h"
#include "DecimalSplit.h"

#include <opencv2/opencv.hpp>
#include <vector>

using namespace cv;

int FindDecimalSplitX(const unsigned char *bgr, int width, int height, int stride)
{
	if (bgr == NULL || width < 3 || height < 3) {
		return -1;
	}

	Mat img(height, width, CV_8UC3, (void *)bgr, (size_t)stride);
	Mat gray;
	cvtColor(img, gray, COLOR_BGR2GRAY);

	Mat th;
	threshold(gray, th, 0, 255, THRESH_BINARY | THRESH_OTSU);
	if (countNonZero(th) > (int)(th.total() / 2)) {
		bitwise_not(th, th);   // ink = white minority
	}

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
			Comp c;
			c.x = stats.at<int>(i, CC_STAT_LEFT);
			c.y = stats.at<int>(i, CC_STAT_TOP);
			c.w = w; c.h = h;
			comps.push_back(c);
			if (h > max_h) max_h = h;
		}
	}
	if (comps.empty()) {
		return -1;
	}

	// Digit band from the tall components.
	int baseline = 0, cap_top = gray.rows;
	bool have_digit = false;
	for (size_t i = 0; i < comps.size(); i++) {
		if (comps[i].h >= 0.6 * max_h) {
			have_digit = true;
			if (comps[i].y + comps[i].h > baseline) baseline = comps[i].y + comps[i].h;
			if (comps[i].y < cap_top) cap_top = comps[i].y;
		}
	}
	if (!have_digit) {
		baseline = 0; cap_top = gray.rows;
		for (size_t i = 0; i < comps.size(); i++) {
			if (comps[i].y + comps[i].h > baseline) baseline = comps[i].y + comps[i].h;
			if (comps[i].y < cap_top) cap_top = comps[i].y;
		}
	}
	double band = (baseline - cap_top) > 1 ? (baseline - cap_top) : 1;

	int best_right = -1, bx = 0, bw = 0;
	for (size_t i = 0; i < comps.size(); i++) {
		const Comp &c = comps[i];
		double cy = c.y + c.h / 2.0;
		double ar = c.w / (double)c.h;
		bool is_short = c.h <= 0.55 * band;
		bool narrow   = c.w <= 0.70 * band;
		bool low      = cy >= cap_top + 0.45 * band;
		bool onbase   = (c.y + c.h) >= baseline - 0.30 * band;
		bool squarish = ar >= 0.4 && ar <= 2.5;
		if (is_short && narrow && low && onbase && squarish) {
			int rt = c.x + c.w;
			if (rt > best_right) { best_right = rt; bx = c.x; bw = c.w; }
		}
	}
	if (best_right < 0) {
		return -1;
	}
	return bx + bw / 2;
}
