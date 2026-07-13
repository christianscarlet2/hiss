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

// ---------------------------------------------------------------------------------------------
// Direct access to a DIB section's pixels.
//
// This is the hot path of the whole scraper: it is what decides "did this region change since the
// last frame?", and 99.877% of the time the answer is no. It was doing that with a GetDC(NULL) (a
// system-wide DC), TWO GetDIBits kernel round-trips and TWO heap allocations -- per region, per
// heartbeat (~874 times a second) -- plus, for the whole-window compare, allocating and copying
// 2 x 1.4 MB every single heartbeat. The change DETECTOR cost more than the work it was avoiding.
// The original authors left the question in the source ("!! How costly is the comparison?"); this
// is the answer.
//
// All of these bitmaps are already created as DIB sections (WindowCaptureCreateDIBSection), which
// means their pixels are ALREADY mapped into our address space. GetObject() hands us the pointer,
// so the comparison becomes a straight memcmp with no DC, no kernel transition and no allocation.
//
// Returns NULL for anything that is not a 32-bpp DIB section, in which case the callers below fall
// back to the original GetDIBits path -- correctness never depends on the fast path being taken.
static const BYTE *DibPixels(HBITMAP bitmap, int *width, int *height, int *stride) {
	DIBSECTION ds = { 0 };
	// GetObject returns sizeof(DIBSECTION) only for a real DIB section; a compatible bitmap
	// yields sizeof(BITMAP). That is exactly the discriminator we want.
	if (GetObject(bitmap, sizeof(DIBSECTION), &ds) != sizeof(DIBSECTION)) return NULL;
	if (ds.dsBm.bmBits == NULL) return NULL;
	if (ds.dsBm.bmBitsPixel != 32) return NULL;
	*width  = ds.dsBm.bmWidth;
	*height = abs(ds.dsBm.bmHeight);
	*stride = ds.dsBm.bmWidthBytes;
	return (const BYTE *) ds.dsBm.bmBits;
}

// Both bitmaps mapped, same geometry? Then they can be compared in place.
static bool MappedPair(HBITMAP left, HBITMAP right,
                       const BYTE **lp, const BYTE **rp, int *w, int *h, int *stride) {
	int lw = 0, lh = 0, ls = 0, rw = 0, rh = 0, rs = 0;
	const BYTE *l = DibPixels(left, &lw, &lh, &ls);
	const BYTE *r = DibPixels(right, &rw, &rh, &rs);
	if (l == NULL || r == NULL) return false;
	if (lw != rw || lh != rh || ls != rs) return false;
	*lp = l; *rp = r; *w = lw; *h = lh; *stride = ls;
	return true;
}

bool BitmapsAreEqual(HBITMAP HBitmapLeft, HBITMAP HBitmapRight)
{
	if (HBitmapLeft == HBitmapRight)
		return true;

	if ((HBitmapLeft == NULL) || (HBitmapRight == NULL))
		return false;

	{	// Fast path: compare the mapped pixels directly.
		const BYTE *l = NULL, *r = NULL;
		int w = 0, h = 0, stride = 0;
		if (MappedPair(HBitmapLeft, HBitmapRight, &l, &r, &w, &h, &stride)) {
			return memcmp(l, r, (size_t) stride * (size_t) h) == 0;
		}
	}

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

	{	// Fast path: walk the mapped pixels in place. Same tolerance rule as below, and it bails
		// out the moment the change is big enough to matter -- so an actually-changed region costs
		// even less than an unchanged one.
		const BYTE *l = NULL, *r = NULL;
		int w = 0, h = 0, stride = 0;
		if (MappedPair(HBitmapLeft, HBitmapRight, &l, &r, &w, &h, &stride)) {
			int changed = 0;
			for (int y = 0; y < h; ++y) {
				const BYTE *lrow = l + (size_t) y * stride;
				const BYTE *rrow = r + (size_t) y * stride;
				for (int x = 0; x < w * 4; x += 4) {          // 32bpp BGRA
					int db = abs((int) lrow[x]     - (int) rrow[x]);
					int dg = abs((int) lrow[x + 1] - (int) rrow[x + 1]);
					int dr = abs((int) lrow[x + 2] - (int) rrow[x + 2]);
					int dmax = db; if (dg > dmax) dmax = dg; if (dr > dmax) dmax = dr;
					if (dmax > pixel_delta && ++changed > min_changed_pixels) {
						return false;                          // genuinely changed
					}
				}
			}
			return true;
		}
	}

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
