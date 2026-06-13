//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Scraping the poker-table and providing access to the scraped data.
//  As the CScraper is low-level and quite large we created 
//  a interface SCraperAccess that provides higher-level accessors
//  like "UserHasCards()".
//  Better use that interface to access scraper-data whenever possible.
//
//******************************************************************************

#include "StdAfx.h"
#include "Bitmaps.h"

bool BitmapsAreEqual(HBITMAP HBitmapLeft, HBITMAP HBitmapRight)
{
	if (HBitmapLeft == HBitmapRight)
		return true;

	if ((HBitmapLeft == NULL) || (HBitmapRight == NULL))
		return false;

	BITMAP left_bitmap = { 0 };
	BITMAP right_bitmap = { 0 };
	if ((GetObject(HBitmapLeft, sizeof(left_bitmap), &left_bitmap) == 0)
			|| (GetObject(HBitmapRight, sizeof(right_bitmap), &right_bitmap) == 0)) {
		return false;
	}

	if ((left_bitmap.bmWidth != right_bitmap.bmWidth)
			|| (left_bitmap.bmHeight != right_bitmap.bmHeight)) {
		return false;
	}

	int width = left_bitmap.bmWidth;
	int height = abs(left_bitmap.bmHeight);
	int image_size = width * height * 4;
	BYTE *pLeftBits = new BYTE[image_size];
	BYTE *pRightBits = new BYTE[image_size];
	if ((pLeftBits == NULL) || (pRightBits == NULL)) {
		delete[] pLeftBits;
		delete[] pRightBits;
		return false;
	}

	BITMAPINFO bitmap_info;
	ZeroMemory(&bitmap_info, sizeof(bitmap_info));
	bitmap_info.bmiHeader.biSize = sizeof(bitmap_info.bmiHeader);
	bitmap_info.bmiHeader.biWidth = width;
	bitmap_info.bmiHeader.biHeight = -height;
	bitmap_info.bmiHeader.biPlanes = 1;
	bitmap_info.bmiHeader.biBitCount = 32;
	bitmap_info.bmiHeader.biCompression = BI_RGB;
	bitmap_info.bmiHeader.biSizeImage = image_size;

	HDC hdc = GetDC(NULL);
	bool same = false;
	if ((GetDIBits(hdc, HBitmapLeft, 0, height, pLeftBits, &bitmap_info, DIB_RGB_COLORS) != 0)
			&& (GetDIBits(hdc, HBitmapRight, 0, height, pRightBits, &bitmap_info, DIB_RGB_COLORS) != 0)) {
		same = (memcmp(pLeftBits, pRightBits, image_size) == 0);
	}
	ReleaseDC(NULL, hdc);

	delete[] pLeftBits;
	delete[] pRightBits;
	return same;
}

bool BitmapsAreSimilar(HBITMAP HBitmapLeft, HBITMAP HBitmapRight,
                       int pixel_delta, int min_changed_pixels)
{
	// Off / exact mode: behave exactly like BitmapsAreEqual.
	if (pixel_delta <= 0) {
		return BitmapsAreEqual(HBitmapLeft, HBitmapRight);
	}
	if (HBitmapLeft == HBitmapRight) return true;
	if ((HBitmapLeft == NULL) || (HBitmapRight == NULL)) return false;

	BITMAP left_bitmap = { 0 };
	BITMAP right_bitmap = { 0 };
	if ((GetObject(HBitmapLeft, sizeof(left_bitmap), &left_bitmap) == 0)
			|| (GetObject(HBitmapRight, sizeof(right_bitmap), &right_bitmap) == 0)) {
		return false;
	}
	if ((left_bitmap.bmWidth != right_bitmap.bmWidth)
			|| (left_bitmap.bmHeight != right_bitmap.bmHeight)) {
		return false;
	}

	int width = left_bitmap.bmWidth;
	int height = abs(left_bitmap.bmHeight);
	int image_size = width * height * 4;
	if (image_size <= 0) return false;
	BYTE *pLeftBits = new BYTE[image_size];
	BYTE *pRightBits = new BYTE[image_size];
	if ((pLeftBits == NULL) || (pRightBits == NULL)) {
		delete[] pLeftBits;
		delete[] pRightBits;
		return false;
	}

	BITMAPINFO bitmap_info;
	ZeroMemory(&bitmap_info, sizeof(bitmap_info));
	bitmap_info.bmiHeader.biSize = sizeof(bitmap_info.bmiHeader);
	bitmap_info.bmiHeader.biWidth = width;
	bitmap_info.bmiHeader.biHeight = -height;
	bitmap_info.bmiHeader.biPlanes = 1;
	bitmap_info.bmiHeader.biBitCount = 32;
	bitmap_info.bmiHeader.biCompression = BI_RGB;
	bitmap_info.bmiHeader.biSizeImage = image_size;

	HDC hdc = GetDC(NULL);
	bool similar = false;
	if ((GetDIBits(hdc, HBitmapLeft, 0, height, pLeftBits, &bitmap_info, DIB_RGB_COLORS) != 0)
			&& (GetDIBits(hdc, HBitmapRight, 0, height, pRightBits, &bitmap_info, DIB_RGB_COLORS) != 0)) {
		int changed = 0;
		// 32bpp BGRA; compare the 3 colour channels per pixel.
		for (int i = 0; i < image_size; i += 4) {
			int db = abs((int)pLeftBits[i]     - (int)pRightBits[i]);
			int dg = abs((int)pLeftBits[i + 1] - (int)pRightBits[i + 1]);
			int dr = abs((int)pLeftBits[i + 2] - (int)pRightBits[i + 2]);
			int dmax = db; if (dg > dmax) dmax = dg; if (dr > dmax) dmax = dr;
			if (dmax > pixel_delta) {
				if (++changed > min_changed_pixels) break;   // genuinely changed
			}
		}
		similar = (changed <= min_changed_pixels);
	}
	ReleaseDC(NULL, hdc);

	delete[] pLeftBits;
	delete[] pRightBits;
	return similar;
}
